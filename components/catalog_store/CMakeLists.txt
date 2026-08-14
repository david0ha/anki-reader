idf_component_register(
    SRCS "catalog_store.c" "catalog_store_core.c"
    INCLUDE_DIRS "."
    REQUIRES vault_core esp_partition zlib)

# These entry points run on 8 KiB firmware tasks. Reject future inline card or
# runtime copies before they can silently consume the task stack again.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wframe-larger-than=2048 -fstack-usage)
