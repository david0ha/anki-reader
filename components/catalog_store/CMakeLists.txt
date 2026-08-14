idf_component_register(
    SRCS "catalog_store.c"
    INCLUDE_DIRS "."
    REQUIRES vault_core esp_partition zlib)
