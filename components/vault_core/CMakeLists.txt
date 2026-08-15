# IDF component: the portable study core + the device-only HTTP port.
# (The desktop simulator does NOT use this file — it compiles the portable
#  sources directly and supplies http_port_curl.c instead of http_port_esp.c.)
#
# cJSON is vendored (third_party/cJSON) rather than depended on: ESP-IDF v6 moved
# cJSON out of core components, and vendoring keeps the firmware, simulator and
# host tests on byte-identical parser code.
set(CJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cJSON)

idf_component_register(
    SRCS
        "kanji_model.c"
        "kanji_parse.c"
        "kanji_catalog.c"
        "kanji_state.c"
        "kanji_mock.c"
        "kanji_service.c"
        "kanji_nav.c"
        "kanji_clock.c"
        "kanji_fsrs.c"
        "device_api_json.c"
        "ui_kanji.c"
        "ui_kanji_layout.c"
        "ui_card_front.c"
        "ui_card_back.c"
        "ui_common.c"
        "ui_icons.c"
        "fonts/ui_font_kr_16.c"
        "fonts/ui_font_kr_20.c"
        "fonts/ui_font_kr_28.c"
        "fonts/ui_font_jp_56.c"
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
# this the generated faces in fonts/ are the only files in the project that
# fail to compile.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# Card models are intentionally full-fidelity and therefore much larger than a
# firmware task may keep by value.  Make every production translation unit
# prove that future parser/UI changes do not put one back on an 8 KiB stack.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wframe-larger-than=2048 -Werror=frame-larger-than=2048 -fstack-usage)
