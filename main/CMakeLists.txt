idf_component_register(
    SRCS "main.cpp"
    # No explicit REQUIRES: the `main` component implicitly depends on every other component in
    # the build (incl. device_api / user_app / provisioning), which is how main.cpp reaches
    # esp_timer.h, board_io.h, etc. Listing one requirement here would opt out of that implicit
    # behavior and force every include to be declared.
    INCLUDE_DIRS "./")

set(KANJI_CATALOG_DB
    "${CMAKE_SOURCE_DIR}/../kanjis-backend/data/kanjis-backend.sqlite3"
    CACHE FILEPATH "Read-only kanjis backend SQLite path")
set(KANJI_CATALOG_USER_ID ""
    CACHE STRING "Optional source user UUID")
set(KANJI_CATALOG_SEED "0"
    CACHE STRING "Deterministic catalog ordering seed")

set(KANJI_CATALOG_IMAGE "${CMAKE_BINARY_DIR}/kanji-catalog.bin")
set(KANJI_CATALOG_GENERATOR "${CMAKE_SOURCE_DIR}/tools/gen_offline_catalog.py")
set(KANJI_CATALOG_ARGS
    --db "${KANJI_CATALOG_DB}"
    --output "${KANJI_CATALOG_IMAGE}"
    --partition-size 0x770000
    --seed "${KANJI_CATALOG_SEED}"
    --verify)
if(NOT KANJI_CATALOG_USER_ID STREQUAL "")
    list(APPEND KANJI_CATALOG_ARGS --user-id "${KANJI_CATALOG_USER_ID}")
endif()

# This target deliberately runs the generator whenever it is requested. The
# generator compares and atomically replaces the output only when bytes change.
add_custom_target(catalog_image
    COMMAND "${PYTHON}" "${KANJI_CATALOG_GENERATOR}" ${KANJI_CATALOG_ARGS}
    BYPRODUCTS "${KANJI_CATALOG_IMAGE}"
    DEPENDS "${KANJI_CATALOG_GENERATOR}" "${KANJI_CATALOG_DB}"
    COMMENT "Generating deterministic offline study catalog"
    VERBATIM)

add_dependencies(flash catalog_image)
esptool_py_flash_to_partition(
    flash catalog "${KANJI_CATALOG_IMAGE}")

# ESP-IDF 5.4.3 exposes the same official helper used for app-flash, so the
# catalog can also be updated without writing the application or study_state.
esptool_py_custom_target(catalog-flash catalog "catalog_image")
esptool_py_flash_to_partition(
    catalog-flash catalog "${KANJI_CATALOG_IMAGE}")
