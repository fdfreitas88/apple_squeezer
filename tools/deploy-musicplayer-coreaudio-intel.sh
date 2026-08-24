#!/bin/bash

# Deploy and control the Intel CoreAudio candidate on the dedicated musicplayer
# Mac. All remote installation changes are managed by test-coreaudio-intel.sh,
# including its rollback snapshot.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MANAGER="$SCRIPT_DIR/test-coreaudio-intel.sh"
BUILD_BIN="$REPO_DIR/apple-squeezer-intel"

REMOTE_HOST=${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}
REMOTE_STAGE='Library/Caches/AppleSqueezerIntelDeploy'
REMOTE_MANAGER="$REMOTE_STAGE/test-coreaudio-intel.sh"
REMOTE_BINARY="$REMOTE_STAGE/apple-squeezer-intel"
PLAYER_NAME='AppleSqueezerIntelTest'
PLAYER_MAC='02:41:53:49:4e:54'
OUTPUT_DEVICE='Mojo'
LMS_SERVER='127.0.0.1'
AUDIO_MODE=${APPLE_SQUEEZER_MODE:-bitperfect}

usage() {
	cat <<'EOF'
Usage:
  tools/deploy-musicplayer-coreaudio-intel.sh install [--mode MODE]
  tools/deploy-musicplayer-coreaudio-intel.sh mode MODE
  tools/deploy-musicplayer-coreaudio-intel.sh devices
  tools/deploy-musicplayer-coreaudio-intel.sh status
  tools/deploy-musicplayer-coreaudio-intel.sh diagnostics [--json]
  tools/deploy-musicplayer-coreaudio-intel.sh validate
  tools/deploy-musicplayer-coreaudio-intel.sh mojo-report
  tools/deploy-musicplayer-coreaudio-intel.sh soak [SECONDS]
  tools/deploy-musicplayer-coreaudio-intel.sh logs
  tools/deploy-musicplayer-coreaudio-intel.sh restart
  tools/deploy-musicplayer-coreaudio-intel.sh stop
  tools/deploy-musicplayer-coreaudio-intel.sh rollback

Target: musicplayer@10.73.254.20
Override only when necessary with MUSICPLAYER_SSH=user@host.
Select native, bitperfect, exclusive, audiophile, or pcm-studio with --mode or
APPLE_SQUEEZER_MODE (default: bitperfect). Exclusive modes cannot share the
Mojo with the standard LocalPlayer instance.

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
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--mode)
				[ "$#" -ge 2 ] || fail "--mode requires a value"
				AUDIO_MODE=$2
				shift 2
				;;
			*) fail "unknown install option: $1" ;;
		esac
	done
	case "$AUDIO_MODE" in
		native|bitperfect|exclusive|audiophile|pcm-studio) ;;
		*) fail "invalid mode: $AUDIO_MODE" ;;
	esac
	check_connection
	"$MANAGER" build
	file "$BUILD_BIN" | grep -q 'x86_64' || fail "local build is not x86_64"
	copy_manager
	scp "$BUILD_BIN" "$REMOTE_HOST:$REMOTE_BINARY"
	remote_manager install-prebuilt \
		"$REMOTE_BINARY" \
		--server "$LMS_SERVER" --name "$PLAYER_NAME" \
		--device "$OUTPUT_DEVICE" --mac "$PLAYER_MAC" --mode "$AUDIO_MODE"
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
			native|bitperfect|exclusive|audiophile|pcm-studio) ;;
			*) fail "invalid mode: $1" ;;
		esac
		check_connection
		copy_manager
		remote_manager mode "$1"
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
		remote_manager soak "$@"
		;;
	help|-h|--help) [ "$#" -eq 0 ] || fail "$command takes no additional arguments"; usage ;;
	*) usage >&2; fail "unknown command: $command" ;;
esac
