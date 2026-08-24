#!/bin/bash

# Build, install and exercise the Intel CoreAudio technical candidate without
# modifying the system-wide Squeezelite installation. Compatible with the
# Bash 3.2 shipped by macOS 11.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_FILE="$REPO_DIR/Makefile.coreaudio-intel"
BUILD_BIN="$REPO_DIR/apple-squeezer-intel"

APP_DIR="$HOME/Library/Application Support/AppleSqueezerIntel"
INSTALL_BIN="$APP_DIR/apple-squeezer-intel"
CONFIG_DIR="$APP_DIR/config"
STATE_DIR="$APP_DIR/test-state"
BACKUP_DIR="$STATE_DIR/rollback"
PID_FILE="$STATE_DIR/apple-squeezer-intel.pid"
LOG_FILE="$STATE_DIR/apple-squeezer-intel.log"

SERVER=""
PLAYER_NAME="Apple Squeezer Intel Test"
DEVICE="default"
PLAYER_MAC=""
AUDIO_MODE="native"

usage() {
	cat <<'EOF'
Usage:
  tools/test-coreaudio-intel.sh install [options]
  tools/test-coreaudio-intel.sh install-prebuilt BINARY [options]
  tools/test-coreaudio-intel.sh build
  tools/test-coreaudio-intel.sh devices
  tools/test-coreaudio-intel.sh start
  tools/test-coreaudio-intel.sh stop
  tools/test-coreaudio-intel.sh restart
  tools/test-coreaudio-intel.sh mode [native|bitperfect|exclusive|audiophile|pcm-studio]
  tools/test-coreaudio-intel.sh status
  tools/test-coreaudio-intel.sh diagnostics [--json]
  tools/test-coreaudio-intel.sh validate
  tools/test-coreaudio-intel.sh mojo-report
  tools/test-coreaudio-intel.sh soak [SECONDS]
  tools/test-coreaudio-intel.sh logs
  tools/test-coreaudio-intel.sh rollback

Install options:
  --server HOST[:PORT]  LMS address. Omit to use automatic discovery.
  --name NAME           LMS player name (default: Apple Squeezer Intel Test).
  --device NAME_OR_ID   CoreAudio device name or ID (default: default).
  --mac MAC             Dedicated LMS player MAC (format: ab:cd:ef:12:34:56).
  --mode MODE           native, bitperfect, exclusive, audiophile, or pcm-studio.
                        audiophile enables bit-perfect plus exclusive access.
                        pcm-studio enables VHQ linear-phase family-preserving
                        upsampling, 1 dB headroom and 24-bit TPDF dither.
  --no-start            Install but do not launch the player.

Examples:
  tools/test-coreaudio-intel.sh install --server 192.168.1.20 --name "Office Mac"
  tools/test-coreaudio-intel.sh devices
  tools/test-coreaudio-intel.sh logs
  tools/test-coreaudio-intel.sh rollback

This candidate decodes PCM, FLAC, ALAC and DSD/DoP. CoreAudio buffer profiles
can be selected in -a parameters as profile=safe, balanced, or lowlatency.
EOF
}

fail() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

note() {
	printf '%s\n' "$*"
}

ensure_macos() {
	[ "$(uname -s)" = "Darwin" ] || fail "this tester requires macOS"
}

ensure_single_line() {
	case "$2" in
		*'
'*) fail "$1 cannot contain a newline" ;;
	esac
}

process_is_running() {
	[ -f "$PID_FILE" ] || return 1
	pid=$(sed -n '1p' "$PID_FILE" 2>/dev/null)
	case "$pid" in
		''|*[!0-9]*) return 1 ;;
	esac
	kill -0 "$pid" 2>/dev/null || return 1
	command_name=$(ps -p "$pid" -o comm= 2>/dev/null)
	case "$command_name" in
		*apple-squeezer-intel) return 0 ;;
	esac
	return 1
}

build_candidate() {
	ensure_macos
	[ -f "$BUILD_FILE" ] || fail "missing $BUILD_FILE"
	note "Building the x86_64 CoreAudio candidate..."
	make -C "$REPO_DIR" -f "$BUILD_FILE" clean
	make -C "$REPO_DIR" -f "$BUILD_FILE"
	[ -x "$BUILD_BIN" ] || fail "build did not produce $BUILD_BIN"
	file "$BUILD_BIN" | grep -q 'x86_64' || fail "candidate is not an x86_64 executable"
	note "Build passed: $(file "$BUILD_BIN")"
}

save_value() {
	key=$1
	value=$2
	printf '%s' "$value" > "$CONFIG_DIR/$key"
}

load_value() {
	key=$1
	default_value=$2
	if [ -f "$CONFIG_DIR/$key" ]; then
		sed -n '1p' "$CONFIG_DIR/$key"
	else
		printf '%s' "$default_value"
	fi
}

create_rollback_snapshot() {
	if [ -f "$BACKUP_DIR/active" ]; then
		note "Keeping the existing rollback snapshot."
		return
	fi

	mkdir -p "$BACKUP_DIR"
	if [ -f "$INSTALL_BIN" ]; then
		cp -p "$INSTALL_BIN" "$BACKUP_DIR/apple-squeezer-intel"
		: > "$BACKUP_DIR/binary-existed"
	fi
	if [ -d "$CONFIG_DIR" ]; then
		cp -R "$CONFIG_DIR" "$BACKUP_DIR/config"
		: > "$BACKUP_DIR/config-existed"
	fi
	: > "$BACKUP_DIR/active"
}

stop_candidate() {
	if ! process_is_running; then
		[ ! -f "$PID_FILE" ] || rm -f "$PID_FILE"
		note "Apple Squeezer Intel is not running."
		return
	fi

	pid=$(sed -n '1p' "$PID_FILE")
	note "Stopping Apple Squeezer Intel (PID $pid)..."
	kill "$pid"
	count=0
	while kill -0 "$pid" 2>/dev/null && [ "$count" -lt 20 ]; do
		sleep 0.25
		count=$((count + 1))
	done
	if kill -0 "$pid" 2>/dev/null; then
		fail "process $pid did not stop; no files were changed"
	fi
	rm -f "$PID_FILE"
}

start_candidate() {
	ensure_macos
	[ -x "$INSTALL_BIN" ] || fail "candidate is not installed; run install first"
	if process_is_running; then
		fail "candidate is already running (PID $(sed -n '1p' "$PID_FILE"))"
	fi
	rm -f "$PID_FILE"
	mkdir -p "$STATE_DIR"

	configured_server=$(load_value server "")
	configured_name=$(load_value player-name "$PLAYER_NAME")
	configured_device=$(load_value device "default")
	configured_mac=$(load_value mac "")
	configured_audio_params=$(load_value audio-params "")
	configured_playback_mode=$(load_value playback-mode "native")
	: > "$LOG_FILE"

	mac_args=""
	[ -z "$configured_mac" ] || mac_args="-m $configured_mac"
	audio_args=""
	[ -z "$configured_audio_params" ] || audio_args="-a $configured_audio_params"
	resample_params=""
	[ "$configured_playback_mode" != "pcm-studio" ] || resample_params="vL::1:28:95:100:50"
	if [ -n "$configured_server" ]; then
		if [ -n "$resample_params" ]; then
			nohup "$INSTALL_BIN" -s "$configured_server" -n "$configured_name" \
				-o "$configured_device" $mac_args $audio_args -u "$resample_params" -D 0:dop \
				-f "$LOG_FILE" -d all=info >/dev/null 2>&1 &
		else
			nohup "$INSTALL_BIN" -s "$configured_server" -n "$configured_name" \
				-o "$configured_device" $mac_args $audio_args -D 0:dop -f "$LOG_FILE" -d all=info \
				>/dev/null 2>&1 &
		fi
	else
		if [ -n "$resample_params" ]; then
			nohup "$INSTALL_BIN" -n "$configured_name" -o "$configured_device" $mac_args $audio_args \
				-u "$resample_params" -D 0:dop -f "$LOG_FILE" -d all=info >/dev/null 2>&1 &
		else
			nohup "$INSTALL_BIN" -n "$configured_name" -o "$configured_device" $mac_args $audio_args \
				-D 0:dop -f "$LOG_FILE" -d all=info >/dev/null 2>&1 &
		fi
	fi
	pid=$!
	printf '%s\n' "$pid" > "$PID_FILE"
	sleep 1
	if ! process_is_running; then
		rm -f "$PID_FILE"
		note "Startup failed. Recent log output:"
		tail -n 40 "$LOG_FILE" 2>/dev/null
		fail "candidate exited during startup"
	fi
	note "Running as PID $pid. Open LMS and select '$configured_name'."
	note "Log: $LOG_FILE"
}

install_candidate() {
	parse_install_options "$@"
	process_is_running && stop_candidate
	build_candidate
	install_binary "$BUILD_BIN"
}

parse_install_options() {
	start_after_install=1
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--server)
				[ "$#" -ge 2 ] || fail "--server requires a value"
				SERVER=$2
				shift 2
				;;
			--name)
				[ "$#" -ge 2 ] || fail "--name requires a value"
				PLAYER_NAME=$2
				shift 2
				;;
			--device)
				[ "$#" -ge 2 ] || fail "--device requires a value"
				DEVICE=$2
				shift 2
				;;
			--mac)
				[ "$#" -ge 2 ] || fail "--mac requires a value"
				PLAYER_MAC=$2
				shift 2
				;;
			--mode)
				[ "$#" -ge 2 ] || fail "--mode requires a value"
				AUDIO_MODE=$2
				shift 2
				;;
			--no-start)
				start_after_install=0
				shift
				;;
			-h|--help)
				usage
				exit 0
				;;
			*) fail "unknown install option: $1" ;;
		esac
	done

	ensure_single_line server "$SERVER"
	ensure_single_line name "$PLAYER_NAME"
	ensure_single_line device "$DEVICE"
	ensure_single_line mac "$PLAYER_MAC"
	ensure_single_line mode "$AUDIO_MODE"
	case "$PLAYER_MAC" in
		''|[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]) ;;
		*) fail "invalid --mac value: $PLAYER_MAC" ;;
	esac
	case "$AUDIO_MODE" in
		native) AUDIO_PARAMS="mode=native:profile=balanced" ;;
		bitperfect) AUDIO_PARAMS="mode=native:bitperfect:profile=balanced" ;;
		exclusive) AUDIO_PARAMS="mode=native:exclusive:profile=balanced" ;;
		audiophile) AUDIO_PARAMS="mode=native:exclusive:bitperfect:profile=safe" ;;
		pcm-studio) AUDIO_PARAMS="mode=pcm-studio:exclusive:dither:headroom=1:profile=safe" ;;
		*) fail "invalid --mode value: $AUDIO_MODE" ;;
	esac
}

install_binary() {
	source_binary=$1
	[ -f "$source_binary" ] || fail "prebuilt candidate not found: $source_binary"
	file "$source_binary" | grep -q 'x86_64' || fail "prebuilt candidate is not x86_64"
	mkdir -p "$APP_DIR" "$STATE_DIR"
	create_rollback_snapshot
	mkdir -p "$CONFIG_DIR"
	cp -p "$source_binary" "$INSTALL_BIN"
	chmod 755 "$INSTALL_BIN"
	save_value server "$SERVER"
	save_value player-name "$PLAYER_NAME"
	save_value device "$DEVICE"
	save_value mac "$PLAYER_MAC"
	save_value audio-params "$AUDIO_PARAMS"
	save_value playback-mode "$AUDIO_MODE"
	note "Installed test candidate in $APP_DIR"
	if [ "$start_after_install" -eq 1 ]; then
		start_candidate
	else
		note "Not started. Run: $0 start"
	fi
}

install_prebuilt() {
	[ "$#" -ge 1 ] || fail "install-prebuilt requires a binary path"
	source_binary=$1
	shift
	parse_install_options "$@"
	process_is_running && stop_candidate
	install_binary "$source_binary"
}

show_devices() {
	ensure_macos
	if [ -x "$INSTALL_BIN" ]; then
		"$INSTALL_BIN" -l
	elif [ -x "$BUILD_BIN" ]; then
		"$BUILD_BIN" -l
	else
		build_candidate
		"$BUILD_BIN" -l
	fi
}

show_status() {
	if process_is_running; then
		note "RUNNING: PID $(sed -n '1p' "$PID_FILE")"
		note "Binary: $INSTALL_BIN"
		note "Player: $(load_value player-name "$PLAYER_NAME")"
		note "Device: $(load_value device default)"
		note "MAC: $(load_value mac automatic)"
		note "Mode: $(load_value audio-params native)"
		note "Playback profile: $(load_value playback-mode native)"
	else
		note "STOPPED"
		[ -x "$INSTALL_BIN" ] && note "Installed: $INSTALL_BIN"
	fi
	return 0
}

show_logs() {
	[ -f "$LOG_FILE" ] || fail "no test log exists yet"
	tail -n 100 "$LOG_FILE"
}

latest_telemetry() {
	[ -f "$LOG_FILE" ] || return 1
	grep 'CoreAudio telemetry:' "$LOG_FILE" | tail -n 1
}

telemetry_value() {
	printf '%s\n' "$1" | sed -n "s/.*[ ,]$2=\([^ ,]*\).*/\1/p"
}

show_diagnostics() {
	format=${1:-text}
	[ "$format" = text ] || [ "$format" = --json ] || fail "diagnostics accepts only --json"
	running=false
	process_is_running && running=true
	line=$(latest_telemetry 2>/dev/null || true)
	mode=$(load_value playback-mode native)
	device=$(load_value device default)
	rate=$(telemetry_value "$line" rate); rate=${rate:-0}
	buffer=$(telemetry_value "$line" buffer); buffer=${buffer:-0}
	latency=$(telemetry_value "$line" latency); latency=${latency:-0}
	underruns=$(telemetry_value "$line" underruns); underruns=${underruns:-0}
	overloads=$(telemetry_value "$line" overloads); overloads=${overloads:-0}
	reopens=$(telemetry_value "$line" reopens); reopens=${reopens:-0}
	processed=$(telemetry_value "$line" processed); processed=${processed:-0}
	clipped=$(telemetry_value "$line" clipped); clipped=${clipped:-0}
	transport=$(telemetry_value "$line" transport); transport=${transport:-unknown}
	physical=$(grep 'CoreAudio physical stream' "$LOG_FILE" 2>/dev/null | tail -n 1 || true)
	signal=$(grep 'signal path:' "$LOG_FILE" 2>/dev/null | tail -n 1 || true)
	if [ "$format" = --json ]; then
		json_device=$(printf '%s' "$device" | sed 's/\\/\\\\/g; s/"/\\"/g')
		json_signal=$(printf '%s' "$signal" | sed 's/\\/\\\\/g; s/"/\\"/g')
		json_physical=$(printf '%s' "$physical" | sed 's/\\/\\\\/g; s/"/\\"/g')
		printf '{"running":%s,"mode":"%s","transport":"%s","device":"%s","rate":%s,"buffer_frames":%s,"latency_frames":%s,"underruns":%s,"overloads":%s,"reopens":%s,"processed_frames":%s,"clipped_samples":%s,"signal_path":"%s","physical_stream":"%s"}\n' \
			"$running" "$mode" "$transport" "$json_device" "$rate" "$buffer" "$latency" "$underruns" "$overloads" "$reopens" "$processed" "$clipped" "$json_signal" "$json_physical"
	else
		note "Running: $running"
		note "Mode: $mode"
		note "Device: $device"
		note "Rate: $rate Hz"
		note "Transport: $transport"
		note "Buffer / latency: $buffer / $latency frames"
		note "Underruns / overloads / reopens: $underruns / $overloads / $reopens"
		note "Processed / clipped samples: $processed / $clipped"
		[ -z "$signal" ] || note "Signal path: $signal"
		[ -z "$physical" ] || note "Physical stream: $physical"
	fi
}

mojo_report() {
	[ -f "$LOG_FILE" ] || fail "no test log exists yet"
	missing=0
	note "Chord Mojo PCM transition evidence"
	for rate in 44100 48000 88200 96000 176400 192000; do
		if grep -q "hardware=$rate Hz" "$LOG_FILE" && grep -q " $rate Hz," "$LOG_FILE"; then
			note "PASS: $rate Hz opened and physical stream observed"
		else
			note "MISSING: $rate Hz (play a track at this native rate)"
			missing=$((missing + 1))
		fi
	done
	if grep -q 'transport=DoP rate=176400' "$LOG_FILE"; then
		note "PASS: DSD64/DoP transport at 176400 Hz observed"
	else
		note "MISSING: DSD64/DoP at 176400 Hz"
		missing=$((missing + 1))
	fi
	if [ "$missing" -eq 0 ]; then
		note "PASS: complete logged Mojo transition matrix. Confirm the Mojo indicator matched each transition."
		return 0
	fi
	fail "$missing Mojo matrix item(s) still need physical playback evidence"
}

validate_candidate() {
	failures=0
	process_is_running || { note "FAIL: player is not running"; failures=$((failures + 1)); }
	line=$(latest_telemetry 2>/dev/null || true)
	[ -n "$line" ] || { note "FAIL: no CoreAudio telemetry has been recorded"; failures=$((failures + 1)); }
	grep -q 'opened CoreAudio device' "$LOG_FILE" 2>/dev/null || { note "FAIL: CoreAudio did not open"; failures=$((failures + 1)); }
	grep -q 'CoreAudio physical stream' "$LOG_FILE" 2>/dev/null || { note "FAIL: physical stream was not verified"; failures=$((failures + 1)); }
	underruns=$(telemetry_value "$line" underruns); underruns=${underruns:-0}
	overloads=$(telemetry_value "$line" overloads); overloads=${overloads:-0}
	clipped=$(telemetry_value "$line" clipped); clipped=${clipped:-0}
	[ "$underruns" = 0 ] || { note "FAIL: $underruns output underrun(s)"; failures=$((failures + 1)); }
	[ "$overloads" = 0 ] || { note "FAIL: $overloads CoreAudio overload(s)"; failures=$((failures + 1)); }
	[ "$clipped" = 0 ] || { note "FAIL: $clipped clipped sample(s)"; failures=$((failures + 1)); }
	if [ "$(load_value playback-mode native)" = audiophile ]; then
		grep -q 'exclusive bit-perfect' "$LOG_FILE" 2>/dev/null || { note "FAIL: audiophile mode lacks exclusive bit-perfect confirmation"; failures=$((failures + 1)); }
	fi
	[ "$failures" -eq 0 ] || fail "$failures validation check(s) failed"
	note "PASS: CoreAudio health and configured signal-path checks passed."
	show_diagnostics text
}

soak_candidate() {
	duration=${1:-3600}
	case "$duration" in ''|*[!0-9]*) fail "soak duration must be seconds" ;; esac
	[ "$duration" -ge 10 ] || fail "soak duration must be at least 10 seconds"
	process_is_running || fail "player is not running"
	before=$(latest_telemetry 2>/dev/null || true)
	before_underruns=$(telemetry_value "$before" underruns); before_underruns=${before_underruns:-0}
	before_overloads=$(telemetry_value "$before" overloads); before_overloads=${before_overloads:-0}
	before_clipped=$(telemetry_value "$before" clipped); before_clipped=${before_clipped:-0}
	note "Monitoring Apple Squeezer for $duration seconds; keep representative music playing."
	elapsed=0
	while [ "$elapsed" -lt "$duration" ]; do
		sleep_for=10
		remaining=$((duration - elapsed))
		[ "$remaining" -ge "$sleep_for" ] || sleep_for=$remaining
		sleep "$sleep_for"
		elapsed=$((elapsed + sleep_for))
		process_is_running || fail "player exited after $elapsed seconds"
	done
	after=$(latest_telemetry 2>/dev/null || true)
	after_underruns=$(telemetry_value "$after" underruns); after_underruns=${after_underruns:-0}
	after_overloads=$(telemetry_value "$after" overloads); after_overloads=${after_overloads:-0}
	after_clipped=$(telemetry_value "$after" clipped); after_clipped=${after_clipped:-0}
	[ "$after_underruns" -eq "$before_underruns" ] || fail "underruns increased from $before_underruns to $after_underruns"
	[ "$after_overloads" -eq "$before_overloads" ] || fail "overloads increased from $before_overloads to $after_overloads"
	[ "$after_clipped" -eq "$before_clipped" ] || fail "clipping increased from $before_clipped to $after_clipped"
	note "PASS: $duration-second soak completed with no new underruns, overloads, clipping, or process exit."
}

set_audio_mode() {
	[ "$#" -eq 1 ] || fail "mode requires native, bitperfect, exclusive, audiophile, or pcm-studio"
	case "$1" in
		native) audio_params="mode=native:profile=balanced" ;;
		bitperfect) audio_params="mode=native:bitperfect:profile=balanced" ;;
		exclusive) audio_params="mode=native:exclusive:profile=balanced" ;;
		audiophile) audio_params="mode=native:exclusive:bitperfect:profile=safe" ;;
		pcm-studio) audio_params="mode=pcm-studio:exclusive:dither:headroom=1:profile=safe" ;;
		*) fail "invalid mode: $1" ;;
	esac
	[ -x "$INSTALL_BIN" ] || fail "candidate is not installed"
	mkdir -p "$CONFIG_DIR"
	save_value audio-params "$audio_params"
	save_value playback-mode "$1"
	if process_is_running; then
		stop_candidate
		start_candidate
	fi
	note "Audio mode changed to $1."
}

rollback_candidate() {
	stop_candidate
	[ -f "$BACKUP_DIR/active" ] || fail "no active rollback snapshot exists"

	if [ -f "$BACKUP_DIR/binary-existed" ]; then
		cp -p "$BACKUP_DIR/apple-squeezer-intel" "$INSTALL_BIN"
		note "Restored the previous test binary."
	else
		rm -f "$INSTALL_BIN"
		note "Removed the test binary created by this script."
	fi

	if [ -f "$BACKUP_DIR/config-existed" ]; then
		# Remove only the configuration files managed by this tester.
		rm -f "$CONFIG_DIR/server" "$CONFIG_DIR/player-name" "$CONFIG_DIR/device" "$CONFIG_DIR/mac" \
			"$CONFIG_DIR/audio-params" "$CONFIG_DIR/playback-mode"
		cp -R "$BACKUP_DIR/config/." "$CONFIG_DIR/"
		note "Restored the previous test configuration."
	else
		rm -f "$CONFIG_DIR/server" "$CONFIG_DIR/player-name" "$CONFIG_DIR/device" "$CONFIG_DIR/mac" \
			"$CONFIG_DIR/audio-params" "$CONFIG_DIR/playback-mode"
		rmdir "$CONFIG_DIR" 2>/dev/null || true
		note "Removed the test configuration created by this script."
	fi

	rm -f "$BACKUP_DIR/active" "$BACKUP_DIR/binary-existed" \
		"$BACKUP_DIR/config-existed" "$BACKUP_DIR/apple-squeezer-intel"
	rm -f "$BACKUP_DIR/config/server" "$BACKUP_DIR/config/player-name" \
		"$BACKUP_DIR/config/device" "$BACKUP_DIR/config/mac" "$BACKUP_DIR/config/audio-params" \
		"$BACKUP_DIR/config/playback-mode"
	rmdir "$BACKUP_DIR/config" 2>/dev/null || true
	rmdir "$BACKUP_DIR" 2>/dev/null || true
	note "Rollback complete. The restored binary was not started automatically."
}

command=${1:-help}
[ "$#" -eq 0 ] || shift

case "$command" in
	install) install_candidate "$@" ;;
	install-prebuilt) install_prebuilt "$@" ;;
	build) [ "$#" -eq 0 ] || fail "build takes no arguments"; build_candidate ;;
	devices) [ "$#" -eq 0 ] || fail "devices takes no arguments"; show_devices ;;
	start) [ "$#" -eq 0 ] || fail "start takes no arguments"; start_candidate ;;
	stop) [ "$#" -eq 0 ] || fail "stop takes no arguments"; stop_candidate ;;
	restart)
		[ "$#" -eq 0 ] || fail "restart takes no arguments"
		stop_candidate
		start_candidate
		;;
	mode) set_audio_mode "$@" ;;
	status) [ "$#" -eq 0 ] || fail "status takes no arguments"; show_status ;;
	diagnostics) [ "$#" -le 1 ] || fail "diagnostics accepts only --json"; show_diagnostics "${1:-text}" ;;
	validate) [ "$#" -eq 0 ] || fail "validate takes no arguments"; validate_candidate ;;
	mojo-report) [ "$#" -eq 0 ] || fail "mojo-report takes no arguments"; mojo_report ;;
	soak) [ "$#" -le 1 ] || fail "soak accepts an optional duration in seconds"; soak_candidate "${1:-3600}" ;;
	logs) [ "$#" -eq 0 ] || fail "logs takes no arguments"; show_logs ;;
	rollback) [ "$#" -eq 0 ] || fail "rollback takes no arguments"; rollback_candidate ;;
	help|-h|--help) usage ;;
	*) usage >&2; fail "unknown command: $command" ;;
esac
