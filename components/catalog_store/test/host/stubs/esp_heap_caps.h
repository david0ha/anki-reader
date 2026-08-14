#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM 0x01u
#define MALLOC_CAP_8BIT 0x02u

void *heap_caps_malloc(size_t size, uint32_t capabilities);
