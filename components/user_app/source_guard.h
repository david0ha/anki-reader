/* Portable source-generation guard shared by user_app.cpp and its host race test. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t generation;
} source_guard_t;

static inline uint32_t source_guard_capture(const source_guard_t *guard)
{
    return guard->generation;
}

static inline void source_guard_advance(source_guard_t *guard)
{
    guard->generation++;
}

static inline bool source_guard_accepts(const source_guard_t *guard, uint32_t generation)
{
    return guard->generation == generation;
}
