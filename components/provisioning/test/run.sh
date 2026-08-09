#!/bin/sh
# Compile and run the host unit tests for the provisioning component's pure logic.
# Only host-safe (no ESP-IDF dependency) sources are listed here.
set -e

DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/.." && pwd)

PURE_SRCS="$ROOT/prov_config.c $ROOT/form_parse.c $ROOT/prov_json.c"
TEST_SRCS="$DIR/tf_main.c $DIR"/test_*.c

# shellcheck disable=SC2086
# Note: AddressSanitizer is intentionally not used — its shadow-memory mmap is
# blocked in the sandbox and hangs. UndefinedBehaviorSanitizer works fine.
cc -std=c11 -Wall -Wextra -Werror -g -O0 \
    -fsanitize=undefined -fno-sanitize-recover=all \
    -I"$ROOT" -I"$DIR" \
    $PURE_SRCS $TEST_SRCS \
    -o "$DIR/run_tests"

"$DIR/run_tests"
