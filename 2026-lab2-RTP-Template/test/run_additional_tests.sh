#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
CXX=${CXX:-c++}
GTEST_LIBRARY="$BUILD_DIR/lib/libgtest.a"
TEST_BIN_SOURCE="$ROOT_DIR/test/bin"

if [[ ! -x "$BUILD_DIR/sender" || ! -x "$BUILD_DIR/receiver" ]]; then
    printf 'Build sender and receiver first: cmake -S . -B build && cmake --build build -j\n' >&2
    exit 2
fi
for helper in "$TEST_BIN_SOURCE/test_sender" "$TEST_BIN_SOURCE/test_receiver"; do
    if [[ ! -f "$helper" ]]; then
        printf 'Reference executable is missing: %s\n' "$helper" >&2
        exit 2
    fi
done
if [[ ! -f "$GTEST_LIBRARY" ]]; then
    printf 'GoogleTest library not found: %s\n' "$GTEST_LIBRARY" >&2
    exit 2
fi

RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/rtp-additional-run.XXXXXX")
trap 'rm -rf "$RUN_DIR"' EXIT
TEST_BINARY="$RUN_DIR/rtp_additional_test"
cp "$TEST_BIN_SOURCE/test_sender" "$TEST_BIN_SOURCE/test_receiver" "$RUN_DIR/"
chmod 755 "$RUN_DIR/test_sender" "$RUN_DIR/test_receiver"

"$CXX" -std=c++17 -Wall \
    -I"$ROOT_DIR/third_party/googletest-release-1.12.1/googletest/include" \
    -I"$ROOT_DIR/src" \
    -DSOURCE_DIR=\""$ROOT_DIR"\" \
    -DREFERENCE_BIN_DIR=\""$RUN_DIR"\" \
    "$ROOT_DIR/test/src/additional_test.cpp" "$GTEST_LIBRARY" -pthread \
    -o "$TEST_BINARY"

"$TEST_BINARY" "$@"
