#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Werror test_source_guard.c -o /tmp/obsidian_source_guard_test
/tmp/obsidian_source_guard_test
