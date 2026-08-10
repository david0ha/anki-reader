/*
 * th.h — the three assertions the vault_core host tests need.
 *
 * Each test is its own executable, so the counters are file-static and the
 * whole framework is a header. Anything larger would be a dependency to keep
 * working; this is thirty lines that never change.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_total = 0, g_fail = 0;

#define CHECK(cond) do { g_total++; if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_INT(a, b) do { g_total++; long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { g_fail++; \
    printf("  FAIL %s:%d  %s == %ld  got %ld\n", __FILE__, __LINE__, #a, _b, _a); } } while (0)

#define CHECK_STR(a, b) do { g_total++; const char *_a = (a); \
    if (_a == NULL || strcmp(_a, (b)) != 0) { g_fail++; \
    printf("  FAIL %s:%d  %s == \"%s\"  got \"%s\"\n", __FILE__, __LINE__, #a, (b), \
           _a ? _a : "(null)"); } } while (0)

#define TH_REPORT(name) do { \
    printf("%s: %d checks, %d failures\n", (name), g_total, g_fail); \
    return g_fail ? 1 : 0; } while (0)

/* Slurp a fixture into a malloc'd, NUL-terminated buffer. Aborts loudly rather
 * than letting a missing fixture turn into a silently-passing test. */
__attribute__((unused))
static char *th_slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  FATAL cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); printf("  FATAL out of memory\n"); exit(2); }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}
