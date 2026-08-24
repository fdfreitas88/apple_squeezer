#!/bin/bash

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
TEST_BIN=${TMPDIR:-/tmp}/apple-squeezer-test-flac-pcm

cc -arch x86_64 -mmacosx-version-min=11.0 -std=c99 -Wall -Wextra -Werror \
	"$SCRIPT_DIR/test-flac-pcm.c" -o "$TEST_BIN"
"$TEST_BIN"

make -C "$REPO_DIR" -f Makefile.coreaudio-intel
file "$REPO_DIR/apple-squeezer-intel" | grep -q x86_64
nm -gU "$REPO_DIR/apple-squeezer-intel" | grep -q _FLAC__stream_decoder_init_ogg_stream
nm -gU "$REPO_DIR/apple-squeezer-intel" | grep -q _ogg_sync_init

printf '%s\n' 'FLAC native/Ogg static linkage checks passed'
