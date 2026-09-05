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
DSPCTL_BIN="$APP_DIR/apple-squeezer-dspctl"
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
DSPCTL_SOURCE=""
DEVICE_WAS_SET=0

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
  tools/test-coreaudio-intel.sh mode [dac-priority|equalizer|osf|csf|native|bitperfect|exclusive|audiophile|pcm-studio]
  tools/test-coreaudio-intel.sh upsample-rate [auto|RATE]
  tools/test-coreaudio-intel.sh resample-filter [linear|minimum|intermediate|gentle|steep|apodizing]
  tools/test-coreaudio-intel.sh resample-expert reset|PRECISION PASSBAND STOPBAND PHASE
  tools/test-coreaudio-intel.sh device NAME_OR_ID
  tools/test-coreaudio-intel.sh dsp-status [--json]
  tools/test-coreaudio-intel.sh dsp-get
  tools/test-coreaudio-intel.sh dsp-apply < config.json
  tools/test-coreaudio-intel.sh dsp-bypass [on|off]
  tools/test-coreaudio-intel.sh dsp-rollback
  tools/test-coreaudio-intel.sh dsp-response [RATE] [POINTS]
  tools/test-coreaudio-intel.sh status
  tools/test-coreaudio-intel.sh diagnostics [--json]
  tools/test-coreaudio-intel.sh validate
  tools/test-coreaudio-intel.sh mojo-report
  tools/test-coreaudio-intel.sh soak [SECONDS]
  tools/test-coreaudio-intel.sh test-report [SECONDS] [--require-matrix]
  tools/test-coreaudio-intel.sh logs
  tools/test-coreaudio-intel.sh rollback

Install options:
  --server HOST[:PORT]  LMS address. Omit to use automatic discovery.
  --name NAME           LMS player name (default: Apple Squeezer Intel Test).
  --device NAME_OR_ID   CoreAudio device name or ID (default: default).
  --mac MAC             Dedicated LMS player MAC (format: ab:cd:ef:12:34:56).
  --mode MODE           native, bitperfect, exclusive, audiophile, or pcm-studio.
  --dspctl BINARY       Matching Apple Squeezer DSP configuration helper.
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
	if [ -f "$DSPCTL_BIN" ]; then
		cp -p "$DSPCTL_BIN" "$BACKUP_DIR/apple-squeezer-dspctl"
		: > "$BACKUP_DIR/dspctl-existed"
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
	configured_upsample_rate=$(load_value upsample-rate "auto")
	configured_resample_filter=$(load_value resample-filter "linear")
	configured_resample_expert=$(load_value resample-expert "")
	configured_dsp=$(load_value dsp-config "")
	: > "$LOG_FILE"

	resample_params=""
	if [ "$configured_playback_mode" = "pcm-studio" ] || [ "$configured_playback_mode" = osf ] || [ "$configured_playback_mode" = csf ]; then
		resample_params="preset=$configured_resample_filter::1"
		if [ "$configured_playback_mode" = csf ] && [ -n "$configured_resample_expert" ]; then resample_params="vL::1:$configured_resample_expert"; fi
		[ "$configured_upsample_rate" = auto ] || resample_params="$resample_params:$configured_upsample_rate"
	fi
	# Build the argument vector without word-splitting paths such as
	# "$HOME/Library/Application Support/.../native-dsp.json".
	set -- -n "$configured_name" -o "$configured_device"
	[ -z "$configured_server" ] || set -- "$@" -s "$configured_server"
	[ -z "$configured_mac" ] || set -- "$@" -m "$configured_mac"
	[ -z "$configured_audio_params" ] || set -- "$@" -a "$configured_audio_params"
	if [ "$configured_playback_mode" != dac-priority ] && [ "$configured_playback_mode" != audiophile ]; then
		[ -z "$configured_dsp" ] || set -- "$@" -Q "@$configured_dsp"
	fi
	[ -z "$resample_params" ] || set -- "$@" -u "$resample_params"
	case "$configured_playback_mode" in
		dac-priority|exclusive|audiophile) set -- "$@" -D 0:dop ;;
	esac
	set -- "$@" -f "$LOG_FILE" -d all=info
	nohup "$INSTALL_BIN" "$@" >/dev/null 2>&1 &
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
				DEVICE_WAS_SET=1
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
			--dspctl)
				[ "$#" -ge 2 ] || fail "--dspctl requires a binary"
				DSPCTL_SOURCE=$2
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
		dac-priority) AUDIO_PARAMS="mode=native:exclusive:bitperfect:profile=safe" ;;
		equalizer) AUDIO_PARAMS="mode=native:profile=safe" ;;
		osf|csf) AUDIO_PARAMS="mode=pcm-studio:dither:headroom=1:profile=safe" ;;
		native) AUDIO_PARAMS="mode=native:profile=balanced" ;;
		bitperfect) AUDIO_PARAMS="mode=native:bitperfect:profile=balanced" ;;
		exclusive) AUDIO_PARAMS="mode=native:exclusive:profile=balanced" ;;
		audiophile) AUDIO_PARAMS="mode=native:exclusive:bitperfect:profile=safe" ;;
		pcm-studio) AUDIO_PARAMS="mode=pcm-studio:dither:headroom=1:profile=safe" ;;
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
	if [ -n "$DSPCTL_SOURCE" ]; then
		[ -f "$DSPCTL_SOURCE" ] || fail "DSP helper not found: $DSPCTL_SOURCE"
		file "$DSPCTL_SOURCE" | grep -q 'x86_64' || fail "DSP helper is not x86_64"
		cp -p "$DSPCTL_SOURCE" "$DSPCTL_BIN"
		chmod 755 "$DSPCTL_BIN"
	fi
	save_value server "$SERVER"
	save_value player-name "$PLAYER_NAME"
	if [ "$DEVICE_WAS_SET" -eq 1 ] || [ ! -f "$CONFIG_DIR/device" ]; then
		save_value device "$DEVICE"
	fi
	save_value mac "$PLAYER_MAC"
	save_value audio-params "$AUDIO_PARAMS"
	save_value playback-mode "$AUDIO_MODE"
	[ -f "$CONFIG_DIR/upsample-rate" ] || save_value upsample-rate "auto"
	[ -f "$CONFIG_DIR/resample-filter" ] || save_value resample-filter "linear"
	[ -f "$CONFIG_DIR/dsp-player-id" ] || save_value dsp-player-id "$(load_value mac "$PLAYER_MAC")"
	note "Installed test candidate in $APP_DIR"
	if [ "$start_after_install" -eq 1 ]; then
		if ! ( start_candidate ); then
			note "Candidate startup failed; restoring the rollback snapshot."
			rollback_candidate
			fail "candidate installation was rolled back"
		fi
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
		note "PCM Studio target: $(load_value upsample-rate auto)"
		note "Resample filter: $(load_value resample-filter linear)"
		if [ -n "$(load_value dsp-config "")" ]; then
			note "Native DSP: configured ($(load_value dsp-config ""))"
		else
			note "Native DSP: disabled"
		fi
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
	upsample_rate=$(load_value upsample-rate auto)
	resample_filter=$(load_value resample-filter linear)
	device=$(load_value device default)
	rate=$(telemetry_value "$line" rate); rate=${rate:-0}
	buffer=$(telemetry_value "$line" buffer); buffer=${buffer:-0}
	latency=$(telemetry_value "$line" latency); latency=${latency:-0}
	underruns=$(telemetry_value "$line" underruns); underruns=${underruns:-0}
	overloads=$(telemetry_value "$line" overloads); overloads=${overloads:-0}
	reopens=$(telemetry_value "$line" reopens); reopens=${reopens:-0}
	processed=$(telemetry_value "$line" processed); processed=${processed:-0}
	clipped=$(telemetry_value "$line" clipped); clipped=${clipped:-0}
	physical_status=$(telemetry_value "$line" physical); physical_status=${physical_status:-unverified}
	volume_status=$(telemetry_value "$line" volume); volume_status=${volume_status:-unverified}
	exclusive_status=$(telemetry_value "$line" exclusive); exclusive_status=${exclusive_status:-not-requested}
	transport=$(telemetry_value "$line" transport); transport=${transport:-unknown}
	physical=$(grep 'CoreAudio physical stream' "$LOG_FILE" 2>/dev/null | tail -n 1 || true)
	signal=$(grep 'signal path:' "$LOG_FILE" 2>/dev/null | tail -n 1 || true)
	if [ "$format" = --json ]; then
		json_device=$(printf '%s' "$device" | sed 's/\\/\\\\/g; s/"/\\"/g')
		json_signal=$(printf '%s' "$signal" | sed 's/\\/\\\\/g; s/"/\\"/g')
		json_physical=$(printf '%s' "$physical" | sed 's/\\/\\\\/g; s/"/\\"/g')
		printf '{"running":%s,"mode":"%s","upsample_rate":"%s","resample_filter":"%s","transport":"%s","device":"%s","rate":%s,"buffer_frames":%s,"latency_frames":%s,"underruns":%s,"overloads":%s,"reopens":%s,"processed_frames":%s,"clipped_samples":%s,"physical_status":"%s","volume_status":"%s","exclusive_status":"%s","signal_path":"%s","physical_stream":"%s"}\n' \
			"$running" "$mode" "$upsample_rate" "$resample_filter" "$transport" "$json_device" "$rate" "$buffer" "$latency" "$underruns" "$overloads" "$reopens" "$processed" "$clipped" "$physical_status" "$volume_status" "$exclusive_status" "$json_signal" "$json_physical"
	else
		note "Running: $running"
		note "Mode: $mode"
		note "PCM Studio target: $upsample_rate"
		note "Resample filter: $resample_filter"
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
	if grep -q 'transport=DoP rate=176400' "$LOG_FILE" ||
			grep -q 'signal path:.*S32 DoP.*CoreAudio 176400 Hz' "$LOG_FILE"; then
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
	# CoreAudio publishes its first telemetry record from the render callback;
	# hardware-control verification completes immediately afterwards. Give the
	# next record a bounded chance to reflect the verified state.
	wait_count=0
	while [ "$wait_count" -lt 15 ]; do
		ready_line=$(latest_telemetry 2>/dev/null || true)
		mode=$(load_value playback-mode native)
		if [ "$mode" != audiophile ] && [ "$mode" != dac-priority ]; then break; fi
		[ "$(telemetry_value "$ready_line" physical)" = verified ] && \
			[ "$(telemetry_value "$ready_line" volume)" = verified ] && \
			[ "$(telemetry_value "$ready_line" exclusive)" = verified ] && break
		sleep 1
		wait_count=$((wait_count + 1))
	done
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
	mode=$(load_value playback-mode native)
	if [ "$mode" = audiophile ] || [ "$mode" = dac-priority ]; then
		grep -q 'exclusive bit-perfect' "$LOG_FILE" 2>/dev/null || { note "FAIL: audiophile mode lacks exclusive bit-perfect confirmation"; failures=$((failures + 1)); }
		[ "$(telemetry_value "$line" physical)" = verified ] || { note "FAIL: physical format is unverified"; failures=$((failures + 1)); }
		[ "$(telemetry_value "$line" volume)" = verified ] || { note "FAIL: unity hardware volume is unverified"; failures=$((failures + 1)); }
		[ "$(telemetry_value "$line" exclusive)" = verified ] || { note "FAIL: exclusive access is unverified"; failures=$((failures + 1)); }
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
	before_processed=$(telemetry_value "$before" processed); before_processed=${before_processed:-0}
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
	after_processed=$(telemetry_value "$after" processed); after_processed=${after_processed:-0}
	[ "$after_underruns" -eq "$before_underruns" ] || fail "underruns increased from $before_underruns to $after_underruns"
	[ "$after_overloads" -eq "$before_overloads" ] || fail "overloads increased from $before_overloads to $after_overloads"
	[ "$after_clipped" -eq "$before_clipped" ] || fail "clipping increased from $before_clipped to $after_clipped"
	[ "$after_processed" -gt "$before_processed" ] || fail "no audio was processed during the soak; play representative music for the complete interval"
	note "PASS: $duration-second soak completed with no new underruns, overloads, clipping, or process exit."
}

report_section() {
	note ""
	note "===== $1 ====="
}

run_report_check() {
	check_name=$1
	shift
	report_section "$check_name"
	if ( "$@" ); then
		note "RESULT: PASS ($check_name)"
	else
		result=$?
		note "RESULT: FAIL ($check_name, exit $result)"
		REPORT_FAILURES=$((REPORT_FAILURES + 1))
	fi
}

test_report() {
	duration=${1:-300}
	require_matrix=${2:-false}
	case "$duration" in ''|*[!0-9]*) fail "test-report duration must be seconds" ;; esac
	[ "$duration" -ge 10 ] || fail "test-report duration must be at least 10 seconds"
	[ "$require_matrix" = false ] || [ "$require_matrix" = --require-matrix ] || [ "$require_matrix" = --skip-matrix ] || fail "test-report accepts --require-matrix or --skip-matrix after the duration"

	REPORT_FAILURES=0
	note "Apple Squeezer Intel consolidated test report"
	note "Generated (UTC): $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	note "Host: $(hostname)"
	note "Soak duration: $duration seconds"
	note "This report does not change playback mode, DSP configuration, or rollback state."

	run_report_check "Candidate status" show_status
	run_report_check "CoreAudio diagnostics (JSON)" show_diagnostics --json
	run_report_check "Native DSP status (JSON)" dsp_status --json
	report_section "Active native DSP configuration"
	configured_dsp=$(load_value dsp-config "")
	if [ -z "$configured_dsp" ]; then
		note "Native DSP is intentionally disabled; transport-only testing remains valid."
		note "RESULT: NOT ENABLED (Active native DSP configuration)"
	elif [ ! -f "$configured_dsp" ]; then
		note "Configured native DSP file is missing: $configured_dsp"
		note "RESULT: FAIL (Active native DSP configuration)"
		REPORT_FAILURES=$((REPORT_FAILURES + 1))
	elif ( dsp_get ); then
		note "RESULT: PASS (Active native DSP configuration)"
	else
		result=$?
		note "RESULT: FAIL (Active native DSP configuration, exit $result)"
		REPORT_FAILURES=$((REPORT_FAILURES + 1))
	fi
	run_report_check "CoreAudio validation" validate_candidate
	run_report_check "Playback soak" soak_candidate "$duration"
	if [ "$require_matrix" = --skip-matrix ]; then
		report_section "Chord Mojo transition matrix"
		note "RESULT: QUALIFIED EARLIER (matrix log was intentionally reset before endurance testing)"
	else
		report_section "Chord Mojo transition matrix"
		if ( mojo_report ); then
			note "RESULT: PASS (Chord Mojo transition matrix)"
		else
			result=$?
			if [ "$require_matrix" = --require-matrix ]; then
				note "RESULT: FAIL (Chord Mojo transition matrix, exit $result)"
				REPORT_FAILURES=$((REPORT_FAILURES + 1))
			else
				note "RESULT: INCOMPLETE (physical formats were not all exercised; use --require-matrix for release qualification)"
			fi
		fi
	fi
	report_section "Recent player log"
	if ( show_logs ); then
		note "RESULT: CAPTURED (Recent player log)"
	else
		result=$?
		note "RESULT: FAIL (Recent player log, exit $result)"
		REPORT_FAILURES=$((REPORT_FAILURES + 1))
	fi

	report_section "Summary"
	if [ "$REPORT_FAILURES" -eq 0 ]; then
		note "OVERALL: PASS"
		return 0
	fi
	note "OVERALL: FAIL ($REPORT_FAILURES section(s) need attention)"
	return 1
}

set_audio_mode() {
	[ "$#" -eq 1 ] || fail "mode requires dac-priority, equalizer, osf, csf, native, bitperfect, exclusive, audiophile, or pcm-studio"
	case "$1" in
		dac-priority) audio_params="mode=native:exclusive:bitperfect:profile=safe" ;;
		equalizer) audio_params="mode=native:profile=safe" ;;
		osf|csf) audio_params="mode=pcm-studio:dither:headroom=1:profile=safe" ;;
		native) audio_params="mode=native:profile=balanced" ;;
		bitperfect) audio_params="mode=native:bitperfect:profile=balanced" ;;
		exclusive) audio_params="mode=native:exclusive:profile=balanced" ;;
		audiophile) audio_params="mode=native:exclusive:bitperfect:profile=safe" ;;
		pcm-studio) audio_params="mode=pcm-studio:dither:headroom=1:profile=safe" ;;
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

set_upsample_rate() {
	[ "$#" -eq 1 ] || fail "upsample-rate requires auto or a supported sample rate"
	case "$1" in
		auto|44100|48000|88200|96000|176400|192000|352800|384000|705600|768000) ;;
		*) fail "invalid upsample rate: $1" ;;
	esac
	[ -x "$INSTALL_BIN" ] || fail "candidate is not installed"
	mkdir -p "$CONFIG_DIR"
	save_value upsample-rate "$1"
	mode=$(load_value playback-mode native)
	if process_is_running && { [ "$mode" = pcm-studio ] || [ "$mode" = osf ] || [ "$mode" = csf ]; }; then
		stop_candidate
		start_candidate
	fi
	note "PCM Studio upsample rate changed to $1."
}

set_resample_filter() {
	[ "$#" -eq 1 ] || fail "resample-filter requires one named preset"
	case "$1" in linear|minimum|intermediate|gentle|steep|apodizing) ;; *) fail "invalid resample filter: $1" ;; esac
	[ -x "$INSTALL_BIN" ] || fail "candidate is not installed"
	mkdir -p "$CONFIG_DIR"
	save_value resample-filter "$1"
	mode=$(load_value playback-mode native)
	if process_is_running && { [ "$mode" = pcm-studio ] || [ "$mode" = osf ] || [ "$mode" = csf ]; }; then
		stop_candidate
		start_candidate
	fi
	note "Resample filter changed to $1."
}

set_resample_expert() {
	[ "$#" -ge 1 ] || fail "resample-expert requires reset or four values"
	if [ "$1" = reset ]; then [ "$#" -eq 1 ] || fail "reset takes no values"; save_value resample-expert "";
	else
		[ "$#" -eq 4 ] || fail "resample-expert requires PRECISION PASSBAND STOPBAND PHASE"
		for value in "$@"; do case "$value" in ''|*[!0-9.]*) fail "expert values must be numeric" ;; esac; done
		awk -v p="$1" -v b="$2" -v s="$3" -v h="$4" 'BEGIN{exit !(p>=16&&p<=32&&b>=80&&b<99.9&&s>b&&s<=100&&h>=0&&h<=100)}' || fail "expert values outside safe bounds"
		save_value resample-expert "$1:$2:$3:$4"
	fi
	if process_is_running && [ "$(load_value playback-mode native)" = csf ]; then stop_candidate; start_candidate; fi
	note "CSF expert filter changed."
}

set_device() {
	[ "$#" -eq 1 ] || fail "device requires exactly one CoreAudio name or ID"
	ensure_single_line device "$1"
	[ -n "$1" ] || fail "device cannot be empty"
	[ -x "$INSTALL_BIN" ] || fail "candidate is not installed"
	mkdir -p "$CONFIG_DIR"
	previous_device=$(load_value device default)
	save_value device "$1"
	if process_is_running; then
		stop_candidate
		if ! (start_candidate); then
			save_value device "$previous_device"
			(start_candidate) || fail "new device failed and the previous device could not be restarted"
			fail "CoreAudio device '$1' could not be opened; restored '$previous_device'"
		fi
	fi
	note "CoreAudio device changed to $1."
}

dsp_config_path() {
	printf '%s' "$CONFIG_DIR/native-dsp.json"
}

dsp_player_id() {
	player=$(load_value dsp-player-id "")
	[ -n "$player" ] || player=$(load_value mac "")
	[ -n "$player" ] || player=$(load_value player-name "$PLAYER_NAME")
	printf '%s' "$player"
}

require_dspctl() {
	[ -x "$DSPCTL_BIN" ] || fail "DSP helper is not installed; reinstall this candidate"
}

restart_after_dsp_change() {
	# The candidate watches its versioned config and atomically crossfades a
	# validated replacement. A stopped player simply loads it on next start.
	if process_is_running; then
		sleep 0.5
		process_is_running || fail "candidate exited while applying live DSP configuration"
		note "DSP configuration queued for click-free live activation."
	fi
}

dsp_apply() {
	[ "$#" -eq 0 ] || fail "dsp-apply reads one JSON document from standard input"
	require_dspctl
	mkdir -p "$CONFIG_DIR"
	path=$(dsp_config_path)
	player=$(dsp_player_id)
	"$DSPCTL_BIN" save "$path" "$player" || fail "DSP configuration validation failed; active settings were not changed"
	save_value dsp-config "$path"
	restart_after_dsp_change
	note "Native DSP configuration applied for $player."
}

dsp_get() {
	[ "$#" -eq 0 ] || fail "dsp-get takes no arguments"
	path=$(dsp_config_path)
	[ -f "$path" ] || fail "native DSP has not been configured"
	cat "$path"
}

dsp_response() {
	[ "$#" -le 2 ] || fail "dsp-response accepts optional RATE and POINTS"
	rate=${1:-48000}
	points=${2:-128}
	case "$rate" in *[!0-9]*|'') fail "invalid response sample rate" ;; esac
	case "$points" in *[!0-9]*|'') fail "invalid response point count" ;; esac
	require_dspctl
	path=$(dsp_config_path)
	[ -f "$path" ] || fail "native DSP has not been configured"
	"$DSPCTL_BIN" response "$path" "$(dsp_player_id)" "$rate" "$points"
}

latest_dsp_telemetry() {
	[ -f "$LOG_FILE" ] || return 1
	grep 'native DSP track:' "$LOG_FILE" | tail -n 1
}

json_number() {
	case "$1" in
		''|*nan*|*NaN*|*inf*|*Inf*) printf null ;;
		*[!0-9.eE+-]*) printf null ;;
		*) printf '%s' "$1" ;;
	esac
}

dsp_status() {
	format=${1:-text}
	[ "$format" = text ] || [ "$format" = --json ] || fail "dsp-status accepts only --json"
	path=$(dsp_config_path)
	enabled=false
	[ -f "$path" ] && [ "$(load_value dsp-config "")" = "$path" ] && enabled=true
	line=$(latest_dsp_telemetry 2>/dev/null || true)
	frames=$(telemetry_value "$line" frames); frames=${frames:-0}
	peak=$(telemetry_value "$line" peak); peak=${peak%dBFS}; peak=$(json_number "$peak")
	clipped=$(telemetry_value "$line" clipped); clipped=${clipped:-0}
	gain=$(telemetry_value "$line" gain); gain=${gain%dB}; gain=$(json_number "$gain")
	response=$(telemetry_value "$line" response_peak); response=${response%dB}; response=$(json_number "$response")
	true_peak=$(telemetry_value "$line" true_peak); true_peak=${true_peak%dBTP}; true_peak=$(json_number "$true_peak")
	true_peak_overs=$(telemetry_value "$line" true_peak_overs); true_peak_overs=${true_peak_overs:-0}
	swaps=$(telemetry_value "$line" swaps); swaps=${swaps:-0}
	latency=$(telemetry_value "$line" latency); latency=${latency:-0}
	fir_taps=$(telemetry_value "$line" fir_taps); fir_taps=${fir_taps:-0}
	fir_partitions=$(telemetry_value "$line" fir_partitions); fir_partitions=${fir_partitions:-0}
	fir_checksum=$(telemetry_value "$line" fir_checksum); fir_checksum=${fir_checksum:-0000000000000000}
	player=$(dsp_player_id)
	if [ "$format" = --json ]; then
		json_player=$(printf '%s' "$player" | sed 's/\\/\\\\/g; s/"/\\"/g')
		printf '{"enabled":%s,"configured":%s,"player_id":"%s","frames":%s,"peak_dbfs":%s,"true_peak_dbfs":%s,"true_peak_overs":%s,"clipped_samples":%s,"applied_gain_db":%s,"response_peak_db":%s,"config_swaps":%s,"latency_frames":%s,"fir_taps":%s,"fir_partitions":%s,"fir_checksum":"%s"}\n' \
			"$enabled" "$([ -f "$path" ] && printf true || printf false)" "$json_player" "$frames" "$peak" "$true_peak" "$true_peak_overs" "$clipped" "$gain" "$response" "$swaps" "$latency" "$fir_taps" "$fir_partitions" "$fir_checksum"
	else
		note "Enabled: $enabled"
		note "Player: $player"
		note "Frames / peak / clipped: $frames / $peak dBFS / $clipped"
		note "True peak / overs: $true_peak dBTP / $true_peak_overs"
		note "Gain / response peak: $gain / $response dB"
		note "Configuration swaps / latency: $swaps / $latency frames"
		note "FIR taps / partitions / checksum: $fir_taps / $fir_partitions / $fir_checksum"
	fi
}

dsp_bypass() {
	[ "$#" -eq 1 ] || fail "dsp-bypass requires on or off"
	case "$1" in on) value=true ;; off) value=false ;; *) fail "dsp-bypass requires on or off" ;; esac
	require_dspctl
	path=$(dsp_config_path)
	[ -f "$path" ] || fail "native DSP has not been configured"
	"$DSPCTL_BIN" bypass "$path" "$(dsp_player_id)" "$value" || fail "unable to change DSP bypass"
	restart_after_dsp_change
	note "Native DSP bypass changed to $1."
}

dsp_rollback_config() {
	[ "$#" -eq 0 ] || fail "dsp-rollback takes no arguments"
	require_dspctl
	"$DSPCTL_BIN" rollback "$(dsp_config_path)" || fail "no DSP rollback configuration is available"
	restart_after_dsp_change
	note "Previous native DSP configuration restored."
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
	if [ -f "$BACKUP_DIR/dspctl-existed" ]; then
		cp -p "$BACKUP_DIR/apple-squeezer-dspctl" "$DSPCTL_BIN"
		note "Restored the previous DSP helper."
	else
		rm -f "$DSPCTL_BIN"
	fi

	if [ -f "$BACKUP_DIR/config-existed" ]; then
		# Remove only the configuration files managed by this tester.
		rm -f "$CONFIG_DIR/server" "$CONFIG_DIR/player-name" "$CONFIG_DIR/device" "$CONFIG_DIR/mac" \
			"$CONFIG_DIR/audio-params" "$CONFIG_DIR/playback-mode"
		rm -f "$CONFIG_DIR/upsample-rate"
		rm -f "$CONFIG_DIR/dsp-config" "$CONFIG_DIR/dsp-player-id" \
			"$CONFIG_DIR/native-dsp.json" "$CONFIG_DIR/native-dsp.json.bak" \
			"$CONFIG_DIR/native-dsp.json.rejected"
		cp -R "$BACKUP_DIR/config/." "$CONFIG_DIR/"
		note "Restored the previous test configuration."
	else
		rm -f "$CONFIG_DIR/server" "$CONFIG_DIR/player-name" "$CONFIG_DIR/device" "$CONFIG_DIR/mac" \
			"$CONFIG_DIR/audio-params" "$CONFIG_DIR/playback-mode"
		rm -f "$CONFIG_DIR/upsample-rate"
		rm -f "$CONFIG_DIR/dsp-config" "$CONFIG_DIR/dsp-player-id" \
			"$CONFIG_DIR/native-dsp.json" "$CONFIG_DIR/native-dsp.json.bak" \
			"$CONFIG_DIR/native-dsp.json.rejected"
		rmdir "$CONFIG_DIR" 2>/dev/null || true
		note "Removed the test configuration created by this script."
	fi

	rm -f "$BACKUP_DIR/active" "$BACKUP_DIR/binary-existed" "$BACKUP_DIR/dspctl-existed" \
		"$BACKUP_DIR/config-existed" "$BACKUP_DIR/apple-squeezer-intel" \
		"$BACKUP_DIR/apple-squeezer-dspctl"
	rm -f "$BACKUP_DIR/config/server" "$BACKUP_DIR/config/player-name" \
		"$BACKUP_DIR/config/device" "$BACKUP_DIR/config/mac" "$BACKUP_DIR/config/audio-params" \
		"$BACKUP_DIR/config/playback-mode"
	rm -f "$BACKUP_DIR/config/upsample-rate"
	rm -f "$BACKUP_DIR/config/dsp-config" "$BACKUP_DIR/config/dsp-player-id" \
		"$BACKUP_DIR/config/native-dsp.json" "$BACKUP_DIR/config/native-dsp.json.bak" \
		"$BACKUP_DIR/config/native-dsp.json.rejected"
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
	upsample-rate) set_upsample_rate "$@" ;;
	resample-filter) set_resample_filter "$@" ;;
	resample-expert) set_resample_expert "$@" ;;
	device) set_device "$@" ;;
	dsp-status) [ "$#" -le 1 ] || fail "dsp-status accepts only --json"; dsp_status "${1:-text}" ;;
	dsp-get) dsp_get "$@" ;;
	dsp-apply) dsp_apply "$@" ;;
	dsp-bypass) dsp_bypass "$@" ;;
	dsp-rollback) dsp_rollback_config "$@" ;;
	dsp-response) dsp_response "$@" ;;
	status) [ "$#" -eq 0 ] || fail "status takes no arguments"; show_status ;;
	diagnostics) [ "$#" -le 1 ] || fail "diagnostics accepts only --json"; show_diagnostics "${1:-text}" ;;
	validate) [ "$#" -eq 0 ] || fail "validate takes no arguments"; validate_candidate ;;
	mojo-report) [ "$#" -eq 0 ] || fail "mojo-report takes no arguments"; mojo_report ;;
	soak) [ "$#" -le 1 ] || fail "soak accepts an optional duration in seconds"; soak_candidate "${1:-3600}" ;;
	test-report) [ "$#" -le 2 ] || fail "test-report accepts a duration and optional --require-matrix or --skip-matrix"; test_report "${1:-300}" "${2:-false}" ;;
	logs) [ "$#" -eq 0 ] || fail "logs takes no arguments"; show_logs ;;
	rollback) [ "$#" -eq 0 ] || fail "rollback takes no arguments"; rollback_candidate ;;
	help|-h|--help) usage ;;
	*) usage >&2; fail "unknown command: $command" ;;
esac
