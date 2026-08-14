#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Werror test_source_guard.c -o /tmp/obsidian_source_guard_test
/tmp/obsidian_source_guard_test

cc -std=c11 -Wall -Wextra -Werror \
    -I../../vault_core/include \
    test_study_source.c ../study_source.c \
    -o /tmp/obsidian_study_source_test
/tmp/obsidian_study_source_test
