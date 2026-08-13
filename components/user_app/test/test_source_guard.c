#include "../source_guard.h"

#include <stdio.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        failures++; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
    } \
} while (0)

int main(void)
{
    source_guard_t guard = {0};

    uint32_t old_fetch = source_guard_capture(&guard);
    CHECK(source_guard_accepts(&guard, old_fetch));

    /* Reproduce the queue race: SET_URL advances first, then the old fetch's
     * delayed APP_CMD_DATA arrives. It must not reach the panel. */
    source_guard_advance(&guard);
    CHECK(!source_guard_accepts(&guard, old_fetch));

    uint32_t new_fetch = source_guard_capture(&guard);
    CHECK(source_guard_accepts(&guard, new_fetch));
    source_guard_advance(&guard);
    CHECK(!source_guard_accepts(&guard, new_fetch));

    printf("source guard: %d failures\n", failures);
    return failures ? 1 : 0;
}
