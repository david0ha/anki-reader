#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t esp_partition_type_t;
typedef uint8_t esp_partition_subtype_t;
typedef int esp_err_t;

#define ESP_OK 0

typedef struct {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
    bool readonly;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(
    esp_partition_type_t type, esp_partition_subtype_t subtype,
    const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *dst, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                              const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size);
