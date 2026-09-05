#!/bin/bash

# Deploy and control the Intel CoreAudio candidate on the dedicated musicplayer
# Mac. All remote installation changes are managed by test-coreaudio-intel.sh,
# including its rollback snapshot.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MANAGER="$SCRIPT_DIR/test-coreaudio-intel.sh"
BUILD_BIN="$REPO_DIR/apple-squeezer-intel"
BUILD_DSPCTL="$REPO_DIR/apple-squeezer-dspctl"

REMOTE_HOST=${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}
REMOTE_STAGE='Library/Caches/AppleSqueezerIntelDeploy'
REMOTE_MANAGER="$REMOTE_STAGE/test-coreaudio-intel.sh"
REMOTE_BINARY="$REMOTE_STAGE/apple-squeezer-intel"
REMOTE_DSPCTL="$REMOTE_STAGE/apple-squeezer-dspctl"
PLAYER_NAME='AppleSqueezerIntelTest'
PLAYER_MAC='02:41:53:49:4e:54'
OUTPUT_DEVICE=${APPLE_SQUEEZER_DEVICE:-}
LMS_SERVER='127.0.0.1'
AUDIO_MODE=${APPLE_SQUEEZER_MODE:-native}

usage() {
	cat <<'EOF'
Usage:
  tools/deploy-musicplayer-coreaudio-intel.sh install [--mode MODE] [--device NAME_OR_ID]
  tools/deploy-musicplayer-coreaudio-intel.sh device NAME_OR_ID
  tools/deploy-musicplayer-coreaudio-intel.sh mode MODE
  tools/deploy-musicplayer-coreaudio-intel.sh upsample-rate auto|RATE
  tools/deploy-musicplayer-coreaudio-intel.sh resample-filter linear|minimum|intermediate|gentle|steep|apodizing
  tools/deploy-musicplayer-coreaudio-intel.sh resample-expert reset|PRECISION PASSBAND STOPBAND PHASE
  tools/deploy-musicplayer-coreaudio-intel.sh dsp-status [--json]
  tools/deploy-musicplayer-coreaudio-intel.sh dsp-get
  tools/deploy-musicplayer-coreaudio-intel.sh dsp-apply FILE.json
  tools/deploy-musicplayer-coreaudio-intel.sh dsp-bypass on|off
  tools/deploy-musicplayer-coreaudio-intel.sh dsp-rollback
  tools/deploy-musicplayer-coreaudio-intel.sh dsp-response [RATE] [POINTS]
  tools/deploy-musicplayer-coreaudio-intel.sh devices
  tools/deploy-musicplayer-coreaudio-intel.sh status
  tools/deploy-musicplayer-coreaudio-intel.sh diagnostics [--json]
  tools/deploy-musicplayer-coreaudio-intel.sh validate
  tools/deploy-musicplayer-coreaudio-intel.sh mojo-report
  tools/deploy-musicplayer-coreaudio-intel.sh soak [SECONDS]
  tools/deploy-musicplayer-coreaudio-intel.sh test-report [SECONDS] [--require-matrix]
  tools/deploy-musicplayer-coreaudio-intel.sh logs
  tools/deploy-musicplayer-coreaudio-intel.sh restart
  tools/deploy-musicplayer-coreaudio-intel.sh stop
  tools/deploy-musicplayer-coreaudio-intel.sh rollback

Target: musicplayer@10.73.254.20
Override only when necessary with MUSICPLAYER_SSH=user@host.
Select dac-priority, equalizer, osf, csf, native, bitperfect, exclusive,
audiophile, or pcm-studio with --mode or
APPLE_SQUEEZER_MODE (default: native). Exclusive modes cannot share the
Mojo with the standard LocalPlayer instance.
The CoreAudio device defaults to "default". Override it with --device or
APPLE_SQUEEZER_DEVICE. The selected device is persisted on Musicplayer.

The install command builds locally, transfers the x86_64 binary, installs it
for the musicplayer user, and starts it against the LMS on that same Mac.
EOF
}

fail() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

check_files() {
	[ -x "$MANAGER" ] || fail "missing executable manager: $MANAGER"
}

check_connection() {
	ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" \
		'test "$(uname -s)" = Darwin && test "$(uname -m)" = x86_64' || \
		fail "cannot reach an Intel Mac at $REMOTE_HOST"
}

prepare_remote_stage() {
	ssh "$REMOTE_HOST" 'mkdir -p "$HOME/Library/Caches/AppleSqueezerIntelDeploy"'
}

copy_manager() {
	prepare_remote_stage
	scp "$MANAGER" "$REMOTE_HOST:$REMOTE_MANAGER"
	ssh "$REMOTE_HOST" 'chmod 755 "$HOME/Library/Caches/AppleSqueezerIntelDeploy/test-coreaudio-intel.sh"'
}

remote_manager() {
	ssh "$REMOTE_HOST" '"$HOME/Library/Caches/AppleSqueezerIntelDeploy/test-coreaudio-intel.sh"' "$@"
}

install_remote() {
	device_was_set=0
	[ -z "$OUTPUT_DEVICE" ] || device_was_set=1
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--mode)
				[ "$#" -ge 2 ] || fail "--mode requires a value"
				AUDIO_MODE=$2
				shift 2
				;;
			--device)
				[ "$#" -ge 2 ] || fail "--device requires a value"
				OUTPUT_DEVICE=$2
				device_was_set=1
				shift 2
				;;
			*) fail "unknown install option: $1" ;;
		esac
	done
	case "$AUDIO_MODE" in
		dac-priority|equalizer|osf|csf|native|bitperfect|exclusive|audiophile|pcm-studio) ;;
		*) fail "invalid mode: $AUDIO_MODE" ;;
	esac
	check_connection
	"$MANAGER" build
	file "$BUILD_BIN" | grep -q 'x86_64' || fail "local build is not x86_64"
	copy_manager
	scp "$BUILD_BIN" "$REMOTE_HOST:$REMOTE_BINARY"
	scp "$BUILD_DSPCTL" "$REMOTE_HOST:$REMOTE_DSPCTL"
	if [ "$device_was_set" -eq 1 ]; then
		remote_manager install-prebuilt "$REMOTE_BINARY" --dspctl "$REMOTE_DSPCTL" \
			--server "$LMS_SERVER" --name "$PLAYER_NAME" --device "$OUTPUT_DEVICE" \
			--mac "$PLAYER_MAC" --mode "$AUDIO_MODE"
	else
		remote_manager install-prebuilt "$REMOTE_BINARY" --dspctl "$REMOTE_DSPCTL" \
			--server "$LMS_SERVER" --name "$PLAYER_NAME" --mac "$PLAYER_MAC" --mode "$AUDIO_MODE"
	fi
}

list_remote_devices() {
	check_connection
	"$MANAGER" build
	prepare_remote_stage
	scp "$BUILD_BIN" "$REMOTE_HOST:$REMOTE_BINARY"
	ssh "$REMOTE_HOST" '"$HOME/Library/Caches/AppleSqueezerIntelDeploy/apple-squeezer-intel" -l'
}

command=${1:-help}
[ "$#" -eq 0 ] || shift
check_files

case "$command" in
	install) install_remote "$@" ;;
	mode)
		[ "$#" -eq 1 ] || fail "mode requires exactly one value"
		case "$1" in
			dac-priority|equalizer|osf|csf|native|bitperfect|exclusive|audiophile|pcm-studio) ;;
			*) fail "invalid mode: $1" ;;
		esac
		check_connection
		copy_manager
		remote_manager mode "$1"
		;;
	upsample-rate)
		[ "$#" -eq 1 ] || fail "upsample-rate requires exactly one value"
		check_connection
		copy_manager
		remote_manager upsample-rate "$1"
		;;
	resample-filter)
		[ "$#" -eq 1 ] || fail "resample-filter requires exactly one value"
		case "$1" in linear|minimum|intermediate|gentle|steep|apodizing) ;; *) fail "invalid resample filter: $1" ;; esac
		check_connection
		copy_manager
		remote_manager resample-filter "$1"
		;;
	resample-expert)
		[ "$#" -ge 1 ] && [ "$#" -le 4 ] || fail "resample-expert requires reset or four values"
		check_connection
		copy_manager
		remote_manager resample-expert "$@"
		;;
	device)
		[ "$#" -eq 1 ] || fail "device requires exactly one value"
		check_connection
		copy_manager
		remote_manager device "$1"
		;;
	dsp-status|dsp-get|dsp-rollback)
		[ "$#" -le 1 ] || fail "$command accepts at most one argument"
		check_connection
		copy_manager
		remote_manager "$command" "$@"
		;;
	dsp-apply)
		[ "$#" -eq 1 ] || fail "dsp-apply requires one JSON file"
		[ -f "$1" ] || fail "DSP configuration not found: $1"
		check_connection
		copy_manager
		ssh "$REMOTE_HOST" '"$HOME/Library/Caches/AppleSqueezerIntelDeploy/test-coreaudio-intel.sh" dsp-apply' < "$1"
		;;
	dsp-bypass)
		[ "$#" -eq 1 ] || fail "dsp-bypass requires on or off"
		check_connection
		copy_manager
		remote_manager dsp-bypass "$1"
		;;
	dsp-response)
		[ "$#" -le 2 ] || fail "dsp-response accepts optional RATE and POINTS"
		check_connection
		copy_manager
		remote_manager dsp-response "$@"
		;;
	devices) [ "$#" -eq 0 ] || fail "devices takes no additional arguments"; list_remote_devices ;;
	rollback)
		[ "$#" -eq 0 ] || fail "rollback takes no additional arguments"
		check_connection
		copy_manager
		remote_manager rollback
		ssh "$REMOTE_HOST" 'rm -f "$HOME/Library/Caches/AppleSqueezerIntelDeploy/apple-squeezer-intel" "$HOME/Library/Caches/AppleSqueezerIntelDeploy/test-coreaudio-intel.sh"; rmdir "$HOME/Library/Caches/AppleSqueezerIntelDeploy" 2>/dev/null || true'
		;;
	status|logs|restart|stop|validate|mojo-report)
		[ "$#" -eq 0 ] || fail "$command takes no additional arguments"
		check_connection
		copy_manager
		remote_manager "$command"
		;;
	diagnostics)
		[ "$#" -le 1 ] || fail "diagnostics accepts only --json"
		check_connection
		copy_manager
		remote_manager diagnostics "$@"
		;;
	soak)
		[ "$#" -le 1 ] || fail "soak accepts an optional duration in seconds"
		check_connection
		copy_manager
		remote_manager "$command" "$@"
		;;
	test-report)
		[ "$#" -le 2 ] || fail "test-report accepts a duration and optional --require-matrix"
		check_connection
		copy_manager
		remote_manager "$command" "$@"
		;;
	help|-h|--help) [ "$#" -eq 0 ] || fail "$command takes no additional arguments"; usage ;;
	*) usage >&2; fail "unknown command: $command" ;;
esac
