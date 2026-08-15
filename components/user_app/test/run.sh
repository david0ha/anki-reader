#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Werror test_source_guard.c -o /tmp/obsidian_source_guard_test
/tmp/obsidian_source_guard_test

cc -std=c11 -Wall -Wextra -Werror \
    -I../../vault_core/include \
    test_study_source.c ../study_source.c \
    ../../vault_core/kanji_model.c ../../vault_core/kanji_mock.c \
    ../../vault_core/kanji_nav.c \
    -o /tmp/obsidian_study_source_test
/tmp/obsidian_study_source_test

cc -std=c11 -Wall -Wextra -Werror \
    test_task_lifecycle.c ../task_lifecycle.c \
    -o /tmp/obsidian_task_lifecycle_test
/tmp/obsidian_task_lifecycle_test

cc -std=c11 -Wall -Wextra -Werror \
    test_startup_delivery.c ../startup_delivery.c \
    -o /tmp/obsidian_startup_delivery_test
/tmp/obsidian_startup_delivery_test
