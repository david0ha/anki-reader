cmake_minimum_required(VERSION 3.16)
project(kanji_sim C)

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

# The core is compiled verbatim — same UI, same nav state machine, same fonts,
# same parser as the firmware. Only the HTTP port is host-specific.
add_executable(kanji_sim
    main_kanji_sim.c
    http_port_curl.c
    ${CORE}/kanji_model.c
    ${CORE}/kanji_parse.c
    ${CORE}/kanji_mock.c
    ${CORE}/kanji_service.c
    ${CORE}/kanji_nav.c
    ${CORE}/kanji_clock.c
    ${CORE}/kanji_fsrs.c
    ${CORE}/ui_common.c
    ${CORE}/ui_icons.c
    ${CORE}/ui_kanji.c
    ${CORE}/ui_kanji_layout.c
    ${CORE}/ui_card_front.c
    ${CORE}/ui_card_back.c
    ${CORE}/fonts/ui_font_kr_16.c
    ${CORE}/fonts/ui_font_kr_20.c
    ${CORE}/fonts/ui_font_kr_28.c
    ${CORE}/fonts/ui_font_jp_56.c
    ${CJSON}/cJSON.c)

# ${CORE} (not just ${CORE}/include) is on the path so the simulator can include
# ui_internal.h. That header is private to the UI files by design — but the
# simulator asserts on the grid it defines, and duplicating the grid here is the
# one way to make the assertions lie.
target_include_directories(kanji_sim PRIVATE ${CORE}/include ${CORE} ${CJSON})
target_link_libraries(kanji_sim lvgl curl m)
