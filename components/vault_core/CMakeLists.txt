# IDF component: the portable vault core + the device-only HTTP port.
# (The desktop simulator does NOT use this file — it compiles the portable
#  sources directly and supplies http_port_curl.c instead of http_port_esp.c.)
#
# cJSON is vendored (third_party/cJSON) rather than depended on: ESP-IDF v6 moved
# cJSON out of core components, and vendoring keeps the firmware, simulator and
# host tests on byte-identical parser code.
set(CJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cJSON)

idf_component_register(
    SRCS
        "vault_model.c"
        "vault_parse.c"
        "vault_mock.c"
        "vault_service.c"
        "device_api_json.c"
        "ui_common.c"
        "ui_vault.c"
        "ui_page_stats.c"
        "ui_page_graph.c"
        "ui_page_agents.c"
        "ui_page_notes.c"
        "ui_graph.c"
        "ui_icons.c"
        "fonts/ui_font_kr_16.c"
        "fonts/ui_font_kr_20.c"
        "http_port_esp.c"
        "${CJSON_DIR}/cJSON.c"
    INCLUDE_DIRS "include" "${CJSON_DIR}"
    REQUIRES
        lvgl__lvgl
        esp_http_client
        esp-tls
        mbedtls)

# lv_font_conv emits `#include "lvgl/lvgl.h"` unless LV_LVGL_H_INCLUDE_SIMPLE is
# defined, and the IDF component exposes the header as plain "lvgl.h". Without
# this the two generated faces in fonts/ are the only files in the project that
# fail to compile.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
