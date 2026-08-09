// Tiny self-contained host test framework (no external deps).
// Each TEST() auto-registers via a constructor; tf_main.c provides the runner.
#pragma once

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

extern int tf_failures;
extern int tf_checks;

typedef void (*tf_func)(void);
void tf_register(const char *name, tf_func fn);

#define TEST(name)                                                       \
    static void name(void);                                              \
    __attribute__((constructor)) static void tf_reg_##name(void) {       \
        tf_register(#name, name);                                        \
    }                                                                    \
    static void name(void)

#define CHECK(cond)                                                      \
    do {                                                                 \
        tf_checks++;                                                     \
        if (!(cond)) {                                                   \
            tf_failures++;                                               \
            printf("  FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                \
    } while (0)

#define CHECK_INT(a, b)                                                  \
    do {                                                                 \
        tf_checks++;                                                     \
        long _a = (long)(a), _b = (long)(b);                             \
        if (_a != _b) {                                                  \
            tf_failures++;                                               \
            printf("  FAIL %s:%d: %s == %s  (%ld != %ld)\n",             \
                   __FILE__, __LINE__, #a, #b, _a, _b);                  \
        }                                                                \
    } while (0)

#define CHECK_STR(a, b)                                                  \
    do {                                                                 \
        tf_checks++;                                                     \
        const char *_a = (a), *_b = (b);                                 \
        if (strcmp(_a, _b) != 0) {                                       \
            tf_failures++;                                               \
            printf("  FAIL %s:%d: %s == %s  (\"%s\" != \"%s\")\n",       \
                   __FILE__, __LINE__, #a, #b, _a, _b);                  \
        }                                                                \
    } while (0)
