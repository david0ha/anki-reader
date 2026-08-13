cmake_minimum_required(VERSION 3.16)
project(vault_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Our lv_conf.h (via #include "lv_conf.h").
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Trim the desktop LVGL build to the same widgets the firmware uses.
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

set(EX    ${CMAKE_CURRENT_SOURCE_DIR}/..)               # repo root = the IDF project
set(CORE  ${EX}/components/vault_core)
set(CJSON ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cJSON)

# Reuse the IDF project's LVGL source (fetched into managed_components/).
add_subdirectory(${EX}/managed_components/lvgl__lvgl ${CMAKE_BINARY_DIR}/lvgl)

# The vault core is compiled verbatim — same UI, same fonts, same logic as the
# firmware. Only the HTTP port is host-specific.
add_executable(sim
    main_sim.c
    http_port_curl.c
    ${CORE}/vault_model.c
    ${CORE}/vault_parse.c
    ${CORE}/vault_mock.c
    ${CORE}/vault_service.c
    ${CORE}/ui_common.c
    ${CORE}/ui_artwork.c
    ${CORE}/ui_artwork_layout.c
    ${CORE}/assets/tarot/major.c
    ${CORE}/assets/tarot/cups.c
    ${CORE}/assets/tarot/pentacles.c
    ${CORE}/assets/tarot/swords.c
    ${CORE}/assets/tarot/wands.c
    ${CORE}/assets/tarot/catalog.c
    ${CORE}/ui_vault.c
    ${CORE}/ui_page_stats.c
    ${CORE}/ui_page_graph.c
    ${CORE}/ui_page_agents.c
    ${CORE}/ui_page_notes.c
    ${CORE}/ui_graph.c
    ${CORE}/ui_icons.c
    ${CORE}/fonts/ui_font_kr_16.c
    ${CORE}/fonts/ui_font_kr_20.c
    ${CORE}/fonts/ui_font_kr_28.c
    ${CJSON}/cJSON.c)

# ${CORE} (not just ${CORE}/include) is on the path so the simulator can include
# ui_internal.h. That header is private to the UI files by design — but the
# simulator asserts on the grid it defines, and duplicating the grid here is the
# one way to make the assertions lie.
target_include_directories(sim PRIVATE ${CORE}/include ${CORE} ${CJSON})
target_link_libraries(sim lvgl curl m)

# Landscape artwork acceptance renderer. This is intentionally a second binary:
# the legacy dashboard simulator remains available as regression coverage while
# this one exercises the exact fixed-template UI now shown on the panel.
add_executable(artwork_sim
    main_artwork_sim.c
    http_port_curl.c
    ${CORE}/vault_model.c
    ${CORE}/vault_parse.c
    ${CORE}/vault_mock.c
    ${CORE}/vault_service.c
    ${CORE}/ui_common.c
    ${CORE}/ui_artwork.c
    ${CORE}/ui_artwork_layout.c
    ${CORE}/assets/tarot/major.c
    ${CORE}/assets/tarot/cups.c
    ${CORE}/assets/tarot/pentacles.c
    ${CORE}/assets/tarot/swords.c
    ${CORE}/assets/tarot/wands.c
    ${CORE}/assets/tarot/catalog.c
    ${CORE}/fonts/ui_font_kr_16.c
    ${CORE}/fonts/ui_font_kr_20.c
    ${CORE}/fonts/ui_font_kr_28.c
    ${CJSON}/cJSON.c)
target_include_directories(artwork_sim PRIVATE ${CORE}/include ${CORE} ${CJSON})
target_link_libraries(artwork_sim lvgl curl m)
