#!/bin/bash

# Authoritative one-command Intel Musicplayer release-candidate qualification.
# A failed post-install test automatically restores the prior candidate.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DEPLOY="$SCRIPT_DIR/deploy-musicplayer-coreaudio-intel.sh"
RESULTS_DIR="$REPO_DIR/test-results"
MODE=dac-priority
DEVICE=""
SOAK_SECONDS=300
ROLLBACK_ON_FAILURE=1
REQUIRE_MATRIX=1
AUTO_FIXTURES=1
PLAYER_ID='02:41:53:49:4e:54'
FIXTURE_DIR=""
PLAYBACK_PID=""
REMOTE_FIXTURE_DIR=""
REMOTE_FIXTURE_URL=""
PLUGIN_PAUSED=0
REMOTE_LOG='/Users/musicplayer/Library/Application Support/AppleSqueezerIntel/test-state/apple-squeezer-intel.log'
HARNESS_VERSION='13-definitive-one-shot-qualification'

usage() {
	cat <<'EOF'
Usage:
  tools/deploy-and-test-musicplayer.sh [--mode MODE] [--device NAME_OR_ID] [--soak SECONDS] [--keep-on-failure]
  tools/deploy-and-test-musicplayer.sh rollback

Builds the Intel candidate, installs it on musicplayer@10.73.254.20, runs the
complete consolidated CoreAudio/DSP/Mojo report, and saves the output under
test-results/. If installation or testing fails, the previous candidate and
configuration are restored automatically unless --keep-on-failure is used.

The default invocation is fully automatic. It generates native FLAC and DSD64
fixtures, drives LMS, verifies fresh CoreAudio evidence for every transition,
runs a clean endurance interval, collects one report, and rolls back on failure.
If the standalone LMS plugin is active, the harness pauses it to avoid duplicate
player identity/exclusive-DAC contention and restores it on every exit path.
The legacy --auto-fixtures and --require-matrix flags remain accepted as no-ops.
EOF
}

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

if [ "${1:-}" = rollback ]; then
	[ "$#" -eq 1 ] || fail "rollback takes no additional arguments"
	exec "$DEPLOY" rollback
fi

while [ "$#" -gt 0 ]; do
	case "$1" in
		--mode) [ "$#" -ge 2 ] || fail "--mode requires a value"; MODE=$2; shift 2 ;;
		--device) [ "$#" -ge 2 ] || fail "--device requires a value"; DEVICE=$2; shift 2 ;;
		--soak) [ "$#" -ge 2 ] || fail "--soak requires seconds"; SOAK_SECONDS=$2; shift 2 ;;
		--keep-on-failure) ROLLBACK_ON_FAILURE=0; shift ;;
		--require-matrix) REQUIRE_MATRIX=1; shift ;;
		--auto-fixtures) AUTO_FIXTURES=1; REQUIRE_MATRIX=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) fail "unknown option: $1" ;;
	esac
done

case "$SOAK_SECONDS" in ''|*[!0-9]*) fail "--soak must be an integer" ;; esac
[ "$SOAK_SECONDS" -ge 10 ] || fail "--soak must be at least 10 seconds"
[ -x "$DEPLOY" ] || fail "missing deploy helper: $DEPLOY"
if [ "$REQUIRE_MATRIX" -eq 1 ]; then
	case "$MODE" in
		dac-priority|native|bitperfect|exclusive|audiophile) ;;
		*) fail "--require-matrix needs a native-rate mode; use --mode dac-priority" ;;
	esac
fi
[ "$AUTO_FIXTURES" -eq 0 ] || [ "$SOAK_SECONDS" -ge 150 ] || fail "--auto-fixtures requires --soak of at least 150 seconds"

mkdir -p "$RESULTS_DIR"
timestamp=$(date -u '+%Y%m%dT%H%M%SZ')
report="$RESULTS_DIR/deploy-test-musicplayer-$timestamp.txt"
installed=0
QUALIFICATION_FAILURES=0

require_command() {
	command -v "$1" >/dev/null 2>&1 || fail "required command is missing: $1"
}

preflight() {
	printf 'Running local and Musicplayer preflight...\n' | tee -a "$report"
	for command_name in bash cc c++ make file nm shasum ssh scp curl python3; do
		require_command "$command_name"
	done
	[ "$(uname -s)" = Darwin ] || fail "the Intel candidate must be built from macOS"
	[ -f "$SCRIPT_DIR/generate-test-flac.c" ] || fail "missing FLAC fixture generator source"
	attempt=1
	while [ "$attempt" -le 3 ]; do
		if ssh -o BatchMode=yes -o ConnectTimeout=8 "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
				'test "$(uname -s)" = Darwin && test "$(uname -m)" = x86_64 && test -w "$HOME/Library/Caches"' && \
				lms_request '["serverstatus","0","1"]'; then
			break
		fi
		[ "$attempt" -eq 3 ] && fail "Musicplayer/LMS remained unavailable or failed the Intel macOS preflight after 3 attempts"
		printf 'INFO: Musicplayer preflight retry %s/3 in 5 seconds.\n' "$attempt" | tee -a "$report"
		sleep 5
		attempt=$((attempt + 1))
	done
	printf 'PASS: prerequisites, Intel Musicplayer SSH, and LMS JSON-RPC are available.\n\n' | tee -a "$report"
}

cleanup_fixtures() {
	[ -z "$PLAYBACK_PID" ] || kill "$PLAYBACK_PID" 2>/dev/null || true
	if [ -n "$REMOTE_FIXTURE_DIR" ]; then
		case "$REMOTE_FIXTURE_DIR" in
			/Users/musicplayer/Library/Caches/AppleSqueezerIntelFixtures-[0-9A-Za-z]*) ;;
			*) fail "refusing to clean unexpected remote fixture path: $REMOTE_FIXTURE_DIR" ;;
		esac
		ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
			"if test -s '$REMOTE_FIXTURE_DIR/http.pid'; then kill \$(sed -n '1p' '$REMOTE_FIXTURE_DIR/http.pid') 2>/dev/null || true; fi; rm -f '$REMOTE_FIXTURE_DIR'/pcm-44100.flac '$REMOTE_FIXTURE_DIR'/pcm-48000.flac '$REMOTE_FIXTURE_DIR'/pcm-88200.flac '$REMOTE_FIXTURE_DIR'/pcm-96000.flac '$REMOTE_FIXTURE_DIR'/pcm-176400.flac '$REMOTE_FIXTURE_DIR'/pcm-192000.flac '$REMOTE_FIXTURE_DIR'/dop64-176400.flac '$REMOTE_FIXTURE_DIR'/endurance-44100.flac '$REMOTE_FIXTURE_DIR'/dsd64.dsf '$REMOTE_FIXTURE_DIR'/SHA256SUMS '$REMOTE_FIXTURE_DIR'/fixture-http-server '$REMOTE_FIXTURE_DIR'/http.pid '$REMOTE_FIXTURE_DIR'/http.log; rmdir '$REMOTE_FIXTURE_DIR' 2>/dev/null || true" \
			>/dev/null 2>&1 || true
	fi
	[ -z "$FIXTURE_DIR" ] || rm -rf "$FIXTURE_DIR"
}

lms_request() {
	command_json=$1
	response=$(curl -fsS --max-time 10 -X POST "http://10.73.254.20:9000/jsonrpc.js" \
		-H 'Content-Type: text/plain' \
		--data "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"$PLAYER_ID\",$command_json]}") || return 1
	printf '%s' "$response" | grep -q '"result"'
}

lms_plugin_request() {
	command_json=$1
	response=$(curl -fsS --max-time 10 -X POST "http://10.73.254.20:9000/jsonrpc.js" \
		-H 'Content-Type: text/plain' \
		--data "{\"id\":1,\"method\":\"slim.request\",\"params\":[\"\",$command_json]}") || return 1
	printf '%s' "$response"
}

pause_standalone_plugin() {
	plugin_status=$(lms_plugin_request '["apple_squeezer","status"]' 2>/dev/null || true)
	printf '%s' "$plugin_status" | grep -q '"result"' || return 0
	if printf '%s' "$plugin_status" | grep -Eq '"running":1|"autorun":1'; then
		printf 'Pausing the standalone LMS plugin during isolated qualification...\n' | tee -a "$report"
		stop_response=$(lms_plugin_request '["apple_squeezer","stop"]') || fail "unable to stop the standalone Apple Squeezer plugin"
		printf '%s' "$stop_response" | grep -q '"success":1' || fail "the standalone Apple Squeezer plugin rejected the stop request"
		PLUGIN_PAUSED=1
		elapsed=0
		while [ "$elapsed" -lt 15 ]; do
			plugin_status=$(lms_plugin_request '["apple_squeezer","status"]' 2>/dev/null || true)
			printf '%s' "$plugin_status" | grep -q '"running":0' && return 0
			sleep 1
			elapsed=$((elapsed + 1))
		done
		fail "standalone Apple Squeezer plugin did not release its process"
	fi
}

restore_standalone_plugin() {
	[ "$PLUGIN_PAUSED" -eq 1 ] || return 0
	printf 'Restoring the standalone LMS plugin...\n' | tee -a "$report"
	start_response=$(lms_plugin_request '["apple_squeezer","start"]' 2>/dev/null || true)
	if ! printf '%s' "$start_response" | grep -q '"success":1'; then
		printf 'WARNING: unable to restart the standalone plugin automatically; use its LMS Start control.\n' | tee -a "$report"
		return 1
	fi
	PLUGIN_PAUSED=0
	return 0
}

prepare_fixtures() {
	command -v python3 >/dev/null 2>&1 || fail "python3 is required to generate automated test signals"
	command -v curl >/dev/null 2>&1 || fail "curl is required to control LMS"
	FIXTURE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/apple-squeezer-fixtures.XXXXXX") || fail "unable to create fixture directory"
	python3 - "$FIXTURE_DIR" "$SOAK_SECONDS" <<'PY'
import os, struct, sys

root = sys.argv[1]
rate = 2822400
seconds = 12
sample_count = rate * seconds
bytes_per_channel = (sample_count + 7) // 8
block_size = 4096
padded = ((bytes_per_channel + block_size - 1) // block_size) * block_size
payload_size = padded * 2
file_size = 28 + 52 + 12 + payload_size
with open(os.path.join(root, "dsd64.dsf"), "wb") as out:
    out.write(b"DSD " + struct.pack("<QQQ", 28, file_size, 0))
    out.write(b"fmt " + struct.pack("<QIIIIIIQII", 52, 1, 0, 2, 2, rate, 1, sample_count, block_size, 0))
    out.write(b"data" + struct.pack("<Q", 12 + payload_size))
    silence = bytes((0x69,)) * block_size
    for offset in range(0, padded, block_size):
        out.write(silence)
        out.write(silence)
PY
	[ "$?" -eq 0 ] || fail "unable to generate automated audio fixtures"
	fixture_generator="$FIXTURE_DIR/generate-test-flac"
	cc -arch x86_64 -mmacosx-version-min=11.0 -std=c99 -O2 -I"$REPO_DIR/third_party/flac/include" \
		"$SCRIPT_DIR/generate-test-flac.c" \
		"$REPO_DIR/third_party/flac/build-intel/src/libFLAC/libFLAC.a" \
		"$REPO_DIR/third_party/ogg/build-intel/install/lib/libogg.a" -lm -o "$fixture_generator" \
		|| fail "unable to build the FLAC fixture generator"
	for rate in 44100 48000 88200 96000 176400 192000; do
		"$fixture_generator" "$FIXTURE_DIR/pcm-$rate.flac" "$rate" 12 \
			|| fail "unable to create $rate Hz FLAC fixture"
	done
	"$fixture_generator" "$FIXTURE_DIR/endurance-44100.flac" 44100 "$((SOAK_SECONDS + 30))" \
		|| fail "unable to create continuous endurance fixture"
	"$fixture_generator" "$FIXTURE_DIR/dop64-176400.flac" 176400 12 dop \
		|| fail "unable to create DSD64/DoP-in-FLAC fixture"
	rm -f "$fixture_generator"
	(
		cd "$FIXTURE_DIR" || exit 1
		shasum -a 256 pcm-*.flac dop64-176400.flac endurance-44100.flac dsd64.dsf > SHA256SUMS
	) || fail "unable to checksum generated fixtures"
	for fixture in "$FIXTURE_DIR"/pcm-*.flac "$FIXTURE_DIR"/endurance-44100.flac; do
		file "$fixture" | grep -q 'FLAC audio bitstream' || fail "invalid generated FLAC fixture: $fixture"
	done
	file "$FIXTURE_DIR/dsd64.dsf" | grep -qi 'DSD\|data' || fail "invalid generated DSF fixture"
	cc -arch x86_64 -mmacosx-version-min=11.0 -std=c99 -Wall -Wextra -Werror -O2 \
		"$SCRIPT_DIR/fixture-http-server.c" -o "$FIXTURE_DIR/fixture-http-server" \
		|| fail "unable to build the dependency-free Intel fixture server"
	file "$FIXTURE_DIR/fixture-http-server" | grep -q 'x86_64' \
		|| fail "fixture server is not an Intel x86_64 executable"
	REMOTE_FIXTURE_DIR="/Users/musicplayer/Library/Caches/AppleSqueezerIntelFixtures-$timestamp"
	# Stop only fixture servers recorded by earlier harness runs.  This recovers
	# ports after an interrupted terminal without touching unrelated services.
	ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
		'for pidfile in "$HOME"/Library/Caches/AppleSqueezerIntelFixtures-*/http.pid; do test -f "$pidfile" || continue; pid=$(sed -n "1p" "$pidfile"); case "$pid" in ""|*[!0-9]*) continue ;; esac; kill "$pid" 2>/dev/null || true; done' \
		>/dev/null 2>&1 || true
	ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" "mkdir -p '$REMOTE_FIXTURE_DIR'" || fail "unable to create remote fixture directory"
	scp "$FIXTURE_DIR"/* "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}:$REMOTE_FIXTURE_DIR/" || fail "unable to copy automated audio fixtures"
	ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" "cd '$REMOTE_FIXTURE_DIR' && shasum -a 256 -c SHA256SUMS" \
		|| fail "fixture checksum verification failed on Musicplayer"
	# Use a bounded port range and verify the actual HTTP payload. A stale test
	# process or an unrelated local service therefore cannot invalidate the run.
	port=18765
	while [ "$port" -le 18774 ]; do
		REMOTE_FIXTURE_URL="http://127.0.0.1:$port"
		ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
			"cd '$REMOTE_FIXTURE_DIR' && chmod 700 fixture-http-server && (nohup ./fixture-http-server '$port' >http.log 2>&1 </dev/null & echo \$! >http.pid)" \
			>/dev/null 2>&1 || true
		elapsed=0
		while [ "$elapsed" -lt 5 ]; do
			if ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
					"test \"\$(curl -fsS --max-time 2 '$REMOTE_FIXTURE_URL/SHA256SUMS' | shasum -a 256 | sed 's/[[:space:]].*//')\" = \"\$(shasum -a 256 '$REMOTE_FIXTURE_DIR/SHA256SUMS' | sed 's/[[:space:]].*//')\"" \
					2>/dev/null; then
				printf 'PASS: native fixture server ready on Musicplayer port %s.\n' "$port" | tee -a "$report"
				return 0
			fi
			sleep 1
			elapsed=$((elapsed + 1))
		done
		ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
			"if test -s '$REMOTE_FIXTURE_DIR/http.pid'; then kill \$(sed -n '1p' '$REMOTE_FIXTURE_DIR/http.pid') 2>/dev/null || true; fi; rm -f '$REMOTE_FIXTURE_DIR/http.pid'" \
			>/dev/null 2>&1 || true
		port=$((port + 1))
	done
	server_log=$(ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
		"sed -n '1,80p' '$REMOTE_FIXTURE_DIR/http.log' 2>/dev/null" 2>/dev/null || true)
	[ -z "$server_log" ] || printf 'Fixture server diagnostics:\n%s\n' "$server_log" | tee -a "$report"
	fail "unable to allocate a verified Musicplayer fixture HTTP server on ports 18765-18774"
}

wait_for_player() {
	timeout=${1:-30}
	elapsed=0
	while [ "$elapsed" -lt "$timeout" ]; do
		players=$(curl -fsS --max-time 10 -X POST 'http://10.73.254.20:9000/jsonrpc.js' \
			-H 'Content-Type: text/plain' \
			--data '{"id":1,"method":"slim.request","params":["",["players","0","100"]]}' 2>/dev/null || true)
		printf '%s' "$players" | grep -q '"playerid":"02:41:53:49:4e:54"' && \
			printf '%s' "$players" | grep -q '"modelname":"AppleSqueezerIntel"' && return 0
		sleep 2
		elapsed=$((elapsed + 2))
	done
	return 1
}

remote_log_line_count() {
	count=$(ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" "wc -l < '$REMOTE_LOG' 2>/dev/null || printf 0" 2>/dev/null) || count=0
	case "$count" in ''|*[!0-9]*) count=0 ;; esac
	printf '%s\n' "$count"
}

wait_for_log_evidence() {
	pattern=$1
	timeout=${2:-45}
	first_line=${3:-1}
	elapsed=0
	while [ "$elapsed" -lt "$timeout" ]; do
		if ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" "sed -n '${first_line},\$p' '$REMOTE_LOG' 2>/dev/null" 2>/dev/null | grep -q "$pattern"; then return 0; fi
		sleep 2
		elapsed=$((elapsed + 2))
	done
	return 1
}

start_endurance_fixture() {
	attempt=1
	while [ "$attempt" -le 3 ]; do
		lms_request '["power","1"]' >/dev/null 2>&1 || true
		lms_request '["stop"]' >/dev/null 2>&1 || true
		sleep 2
		first_line=$(( $(remote_log_line_count) + 1 ))
		if lms_request "[\"playlist\",\"play\",\"$REMOTE_FIXTURE_URL/endurance-44100.flac\"]" &&
				wait_for_log_evidence 'processed=[1-9][0-9]*' 30 "$first_line"; then
			printf 'PASS: endurance fixture entered the native decode path.\n' | tee -a "$report"
			return 0
		fi
		printf 'INFO: retrying endurance playback startup (%s/3).\n' "$attempt" | tee -a "$report"
		attempt=$((attempt + 1))
	done
	return 1
}

wait_for_dop_evidence() {
	first_line=$1
	timeout=${2:-60}
	report_transcode=${3:-yes}
	elapsed=0
	while [ "$elapsed" -lt "$timeout" ]; do
		new_log=$(ssh "${MUSICPLAYER_SSH:-musicplayer@10.73.254.20}" \
			"sed -n '${first_line},\$p' '$REMOTE_LOG' 2>/dev/null" 2>/dev/null || true)
		if printf '%s\n' "$new_log" | grep -q 'transport=DoP rate=176400' ||
				printf '%s\n' "$new_log" | grep -q 'signal path:.*S32 DoP.*CoreAudio 176400 Hz'; then
			return 0
		fi
		if printf '%s\n' "$new_log" | grep -q 'Content-Type: audio/x-flac' && \
				printf '%s\n' "$new_log" | grep -q 'track start sample rate: 352800'; then
			if [ "$report_transcode" = yes ]; then
				printf '%s\n' 'FAIL: LMS converted the native-DoP fixture to 352.8 kHz PCM/FLAC; native DoP did not reach the player.' | tee -a "$report"
			fi
			return 2
		fi
		sleep 2
		elapsed=$((elapsed + 2))
	done
	return 1
}

play_fixture_sequence() {
	base=$REMOTE_FIXTURE_URL
	for rate in 44100 48000 88200 96000 176400 192000; do
		fixture="pcm-$rate.flac"
		printf 'Playing automated fixture: %s\n' "$fixture" | tee -a "$report"
		passed=0
		attempt=1
		while [ "$attempt" -le 2 ]; do
			first_line=$(( $(remote_log_line_count) + 1 ))
			if lms_request "[\"playlist\",\"play\",\"$base/$fixture\"]" && \
					wait_for_log_evidence "hardware=$rate Hz" 45 "$first_line"; then
				passed=1
				break
			fi
			printf 'INFO: retrying %s transition (%s/2).\n' "$rate" "$attempt" | tee -a "$report"
			lms_request '["stop"]' >/dev/null 2>&1 || true
			sleep 2
			attempt=$((attempt + 1))
		done
		if [ "$passed" -eq 1 ]; then
			printf 'PASS: native %s Hz reached CoreAudio.\n' "$rate" | tee -a "$report"
		else
			printf 'FAIL: no new CoreAudio evidence for %s Hz\n' "$rate" | tee -a "$report"
			QUALIFICATION_FAILURES=$((QUALIFICATION_FAILURES + 1))
		fi
		sleep 2
	done
	printf 'Playing automated fixture: dop64-176400.flac (native DoP transport)\n' | tee -a "$report"
	passed=0
	attempt=1
	while [ "$attempt" -le 2 ]; do
		first_line=$(( $(remote_log_line_count) + 1 ))
		if lms_request "[\"playlist\",\"play\",\"$base/dop64-176400.flac\"]" && \
				wait_for_dop_evidence "$first_line" 60; then
			passed=1
			break
		fi
		printf 'INFO: retrying native DoP transition (%s/2).\n' "$attempt" | tee -a "$report"
		lms_request '["stop"]' >/dev/null 2>&1 || true
		sleep 2
		attempt=$((attempt + 1))
	done
	if [ "$passed" -eq 1 ]; then
		printf 'PASS: native DSD64/DoP reached CoreAudio.\n' | tee -a "$report"
	else
		printf 'FAIL: no new native DSD64/DoP evidence.\n' | tee -a "$report"
		QUALIFICATION_FAILURES=$((QUALIFICATION_FAILURES + 1))
	fi
	printf 'Checking DSF/LMS interoperability (informational)...\n' | tee -a "$report"
	first_line=$(( $(remote_log_line_count) + 1 ))
	if lms_request "[\"playlist\",\"play\",\"$base/dsd64.dsf\"]"; then
		if wait_for_dop_evidence "$first_line" 15 no; then
			printf 'PASS: LMS DSDPlayer is configured to deliver DSF as DoP.\n' | tee -a "$report"
		else
			printf 'INFO: DSF is converted by LMS; Apple Squeezer native DoP was already qualified with the DoP-in-FLAC fixture.\n' | tee -a "$report"
		fi
	else
		printf 'INFO: LMS did not accept the DSF interoperability fixture; native DoP qualification remains independent.\n' | tee -a "$report"
	fi
}

rollback_after_failure() {
	status=$?
	trap - EXIT INT TERM
	printf '\n===== Failure diagnostics =====\n' | tee -a "$report"
	curl -fsS --max-time 10 -X POST 'http://10.73.254.20:9000/jsonrpc.js' \
		-H 'Content-Type: text/plain' \
		--data '{"id":1,"method":"slim.request","params":["",["players","0","100"]]}' \
		2>/dev/null | tee -a "$report" || true
	printf '\n' | tee -a "$report"
	if [ "$installed" -eq 1 ]; then
		"$DEPLOY" diagnostics --json 2>&1 | tee -a "$report" || true
		"$DEPLOY" logs 2>&1 | tee -a "$report" || true
	fi
	cleanup_fixtures
	if [ "$installed" -eq 1 ] && [ "$ROLLBACK_ON_FAILURE" -eq 1 ]; then
		printf '\nValidation failed; restoring the previous Musicplayer candidate.\n' | tee -a "$report"
		"$DEPLOY" rollback 2>&1 | tee -a "$report" || true
	elif [ "$installed" -eq 1 ] && [ "$PLUGIN_PAUSED" -eq 1 ]; then
		"$DEPLOY" stop 2>&1 | tee -a "$report" || true
	fi
	restore_standalone_plugin || true
	printf '\nReport saved to %s\n' "$report"
	exit "$status"
}

trap rollback_after_failure EXIT INT TERM
printf 'Apple Squeezer Musicplayer deployment and test (harness %s)\n' "$HARNESS_VERSION" | tee "$report"
printf 'Mode: %s; soak: %s seconds\n\n' "$MODE" "$SOAK_SECONDS" | tee -a "$report"

preflight

printf 'Running local buffer, PCM conversion, and native DSP tests...\n' | tee -a "$report"
make -C "$REPO_DIR" -f Makefile.coreaudio-intel test-buffer test-pcm test-dsp 2>&1 | tee -a "$report"
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || exit "$status"

printf '\nRunning local FLAC/Ogg FLAC and Intel linkage tests...\n' | tee -a "$report"
"$SCRIPT_DIR/test-flac-coreaudio-intel.sh" 2>&1 | tee -a "$report"
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || exit "$status"

printf '\nChecking binary architecture, linked capabilities, and help reporting...\n' | tee -a "$report"
file "$REPO_DIR/apple-squeezer-intel" | grep -q 'x86_64' || fail "candidate is not x86_64"
nm -gU "$REPO_DIR/apple-squeezer-intel" | grep -q '_FLAC__stream_decoder_init_ogg_stream' || fail "Ogg FLAC decoder capability is missing"
nm -gU "$REPO_DIR/apple-squeezer-intel" | grep -q '_register_alac' || fail "ALAC decoder capability is missing"
nm -gU "$REPO_DIR/apple-squeezer-intel" | grep -q '_register_dsd' || fail "DSD decoder capability is missing"
nm -gU "$REPO_DIR/apple-squeezer-intel" | grep -q '_update_dop' || fail "DoP transport capability is missing"
nm -gU "$REPO_DIR/apple-squeezer-intel" | grep -q '_soxr_create' || fail "SoXR resampler capability is missing"
help_text=$("$REPO_DIR/apple-squeezer-intel" -? 2>&1 || true)
printf '%s' "$help_text" | grep -q 'CoreAudio options' || fail "CoreAudio help/capability reporting is missing"
printf 'Candidate SHA-256: %s\n' "$(shasum -a 256 "$REPO_DIR/apple-squeezer-intel" | sed 's/[[:space:]].*//')" | tee -a "$report"
printf 'PASS: x86_64 architecture, FLAC/Ogg FLAC/ALAC/DSD/DoP/SoXR linkage, and CoreAudio help reporting.\n' | tee -a "$report"

printf '\nDeploying candidate for physical CoreAudio tests...\n' | tee -a "$report"

pause_standalone_plugin

if [ "$AUTO_FIXTURES" -eq 1 ]; then
	printf 'Preparing automatic PCM and DSD64 playback fixtures...\n' | tee -a "$report"
	prepare_fixtures
fi

if [ -n "$DEVICE" ]; then
	"$DEPLOY" install --mode "$MODE" --device "$DEVICE" 2>&1 | tee -a "$report"
else
	"$DEPLOY" install --mode "$MODE" 2>&1 | tee -a "$report"
fi
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || exit "$status"
installed=1
wait_for_player 30 || fail "the dedicated Apple Squeezer player did not register with LMS"
printf 'PASS: dedicated Apple Squeezer player registered with LMS.\n' | tee -a "$report"

if [ "$AUTO_FIXTURES" -eq 1 ]; then
	play_fixture_sequence
	printf '\nValidating the automated Chord Mojo transition matrix...\n' | tee -a "$report"
	"$DEPLOY" mojo-report 2>&1 | tee -a "$report"
	status=${PIPESTATUS[0]}
	[ "$status" -eq 0 ] || QUALIFICATION_FAILURES=$((QUALIFICATION_FAILURES + 1))
	printf '\nValidating transition-phase CoreAudio health...\n' | tee -a "$report"
	"$DEPLOY" validate 2>&1 | tee -a "$report"
	status=${PIPESTATUS[0]}
	[ "$status" -eq 0 ] || QUALIFICATION_FAILURES=$((QUALIFICATION_FAILURES + 1))

	printf '\nRestarting with clean counters for the endurance test...\n' | tee -a "$report"
	lms_request '["stop"]' || fail "unable to stop LMS playback before the clean restart"
	sleep 2
	"$DEPLOY" restart 2>&1 | tee -a "$report"
	status=${PIPESTATUS[0]}
	[ "$status" -eq 0 ] || exit "$status"
	wait_for_player 30 || fail "the dedicated player did not reconnect after the clean restart"
	lms_request '["playlist","repeat","0"]' || fail "unable to disable playlist repeat"
	start_endurance_fixture || fail "endurance fixture did not enter the native decode path after 3 attempts"
fi

if [ "$REQUIRE_MATRIX" -eq 1 ] && [ "$AUTO_FIXTURES" -eq 0 ]; then
	"$DEPLOY" test-report "$SOAK_SECONDS" --require-matrix 2>&1 | tee -a "$report"
else
	"$DEPLOY" test-report "$SOAK_SECONDS" --skip-matrix 2>&1 | tee -a "$report"
fi
status=${PIPESTATUS[0]}
[ "$status" -eq 0 ] || QUALIFICATION_FAILURES=$((QUALIFICATION_FAILURES + 1))

if [ "$QUALIFICATION_FAILURES" -ne 0 ]; then
	fail "$QUALIFICATION_FAILURES qualification section(s) failed; all independent tests were completed"
fi

trap - EXIT INT TERM
"$DEPLOY" stop 2>&1 | tee -a "$report" || true
cleanup_fixtures
restore_standalone_plugin || fail "qualification passed, but the standalone plugin could not be restored"
printf '\nDeployment and consolidated validation passed. Candidate remains installed but stopped; the standalone LMS plugin was restored.\n' | tee -a "$report"
printf 'NOT AUTOMATABLE without external actions: Mojo LED observation, analog/USB loopback null measurement, and physical USB disconnect/reconnect.\n' | tee -a "$report"
printf 'Rollback command: %s rollback\n' "$0" | tee -a "$report"
printf 'Report saved to %s\n' "$report"
