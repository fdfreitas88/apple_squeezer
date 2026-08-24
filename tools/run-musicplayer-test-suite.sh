#!/bin/bash

# Run the complete Musicplayer validation through one command and retain the
# combined output locally. The remote test is observational and does not alter
# Apple Squeezer playback or DSP configuration.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DEPLOY="$SCRIPT_DIR/deploy-musicplayer-coreaudio-intel.sh"
RESULTS_DIR="$REPO_DIR/test-results"

usage() {
	cat <<'EOF'
Usage:
  tools/run-musicplayer-test-suite.sh [SOAK_SECONDS]

Runs status, CoreAudio diagnostics, native-DSP status/configuration, validation,
the Chord Mojo transition matrix, a playback soak, and recent-log collection.
The default soak is 300 seconds. Keep representative music playing throughout.

The complete report is printed and saved under test-results/. A failed or
incomplete section makes this script exit non-zero after all sections run.
EOF
}

case "${1:-}" in
	-h|--help) usage; exit 0 ;;
esac

duration=${1:-300}
case "$duration" in ''|*[!0-9]*) printf 'ERROR: SOAK_SECONDS must be an integer\n' >&2; exit 1 ;; esac
[ "$duration" -ge 10 ] || { printf 'ERROR: SOAK_SECONDS must be at least 10\n' >&2; exit 1; }
[ "$#" -le 1 ] || { usage >&2; exit 1; }
[ -x "$DEPLOY" ] || { printf 'ERROR: missing executable %s\n' "$DEPLOY" >&2; exit 1; }

mkdir -p "$RESULTS_DIR"
timestamp=$(date -u '+%Y%m%dT%H%M%SZ')
report="$RESULTS_DIR/musicplayer-$timestamp.txt"

printf 'Saving consolidated results to %s\n' "$report"
set +e
"$DEPLOY" test-report "$duration" 2>&1 | tee "$report"
status=${PIPESTATUS[0]}
set -e
printf '\nReport saved to %s\n' "$report"
exit "$status"
