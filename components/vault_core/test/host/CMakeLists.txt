cmake_minimum_required(VERSION 3.16)
project(vault_core_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Paths relative to this file.
set(CORE   ${CMAKE_CURRENT_SOURCE_DIR}/../..)         # components/vault_core
set(CJSON  ${CORE}/../../third_party/cJSON)           # <root>/third_party/cJSON

# Opt-in sanitizer build:  cmake -S . -B build -DSANITIZE=ON
# (Catches memory-safety regressions like UTF-8 over-reads; some sandboxed
#  macOS setups hang under ASan, so it's off by default.)
option(SANITIZE "Build with Address/UB sanitizers" OFF)

function(add_host_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CORE}/include ${CJSON})
    target_compile_definitions(${name} PRIVATE FIXDIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    target_compile_options(${name} PRIVATE -Wall -Wextra -O0 -g)
    target_link_libraries(${name} m)
    if(SANITIZE)
        target_compile_options(${name} PRIVATE -fsanitize=address,undefined)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# The wire format. Everything the network can do wrong lands here.
add_host_test(test_vault_parse test_vault_parse.c
              ${CORE}/vault_parse.c ${CORE}/vault_model.c ${CJSON}/cJSON.c)

# Where the link-graph nodes go. Pure integer geometry, no LVGL.
add_host_test(test_graph_layout test_graph_layout.c
              ${CORE}/ui_graph.c ${CORE}/vault_model.c)

# The built-in demo snapshot, tied to the reference producer's committed output.
add_host_test(test_vault_mock test_vault_mock.c
              ${CORE}/vault_mock.c ${CORE}/vault_parse.c ${CORE}/vault_model.c
              ${CJSON}/cJSON.c)

# The companion-app JSON. cJSON is linked only to validate the output — the
# serializer itself writes bytes directly.
add_host_test(test_api_json test_api_json.c ${CORE}/device_api_json.c ${CJSON}/cJSON.c)

enable_testing()
add_test(NAME vault_parse  COMMAND test_vault_parse)
add_test(NAME graph_layout COMMAND test_graph_layout)
add_test(NAME vault_mock   COMMAND test_vault_mock)
add_test(NAME api_json     COMMAND test_api_json)
