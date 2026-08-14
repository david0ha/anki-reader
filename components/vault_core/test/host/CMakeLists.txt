cmake_minimum_required(VERSION 3.16)
project(kanji_core_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Paths relative to this file.
set(CORE   ${CMAKE_CURRENT_SOURCE_DIR}/../..)         # components/vault_core
set(CJSON  ${CORE}/../../third_party/cJSON)           # <root>/third_party/cJSON
set(ROOT   ${CORE}/../..)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(ZLIB REQUIRED)

# Reader fixtures are build artifacts produced by the authoritative exporter,
# not hand-packed C blobs. The second source fixture is derived only to put real
# records on both sides of the fixed 64-card block boundary.
set(CATALOG_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/offline_catalog.bin)
set(CATALOG_BOUNDARY_JSON ${CMAKE_CURRENT_BINARY_DIR}/offline_catalog_boundary.json)
set(CATALOG_BOUNDARY_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/offline_catalog_boundary.bin)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -c
            "import copy,json,sys; d=json.load(open(sys.argv[1],encoding='utf-8')); base=d['decks'][0]['cards'][0]; deck={k:v for k,v in d['decks'][0].items() if k != 'cards'}; deck['cards']=[]; [(lambda c,i: (c.__setitem__('id',f'boundary-{i:03d}'),c.__setitem__('front',f'字{i:02d}'),deck['cards'].append(c)))(copy.deepcopy(base),i) for i in range(66)]; json.dump({'decks':[deck]},open(sys.argv[2],'w',encoding='utf-8'),ensure_ascii=False)"
            ${ROOT}/tools/fixtures/offline_catalog.json ${CATALOG_BOUNDARY_JSON}
    RESULT_VARIABLE CATALOG_BOUNDARY_JSON_RESULT)
if(NOT CATALOG_BOUNDARY_JSON_RESULT EQUAL 0)
    message(FATAL_ERROR "could not create catalog boundary fixture")
endif()
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/gen_offline_catalog.py
            --fixture-json ${ROOT}/tools/fixtures/offline_catalog.json
            --output ${CATALOG_FIXTURE} --partition-size 0x770000 --seed 0 --verify
    RESULT_VARIABLE CATALOG_FIXTURE_RESULT)
if(NOT CATALOG_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "could not generate catalog reader fixture")
endif()
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/gen_offline_catalog.py
            --fixture-json ${CATALOG_BOUNDARY_JSON}
            --output ${CATALOG_BOUNDARY_FIXTURE} --partition-size 0x770000 --seed 0 --verify
    RESULT_VARIABLE CATALOG_BOUNDARY_RESULT)
if(NOT CATALOG_BOUNDARY_RESULT EQUAL 0)
    message(FATAL_ERROR "could not generate catalog boundary image")
endif()

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

# The companion-app JSON. cJSON is linked only to validate the output — the
# serializer itself writes bytes directly.
add_host_test(test_api_json test_api_json.c ${CORE}/device_api_json.c ${CJSON}/cJSON.c)

# The snapshot's own arithmetic: UTF-8-safe truncation and the fingerprint that
# decides whether the panel refreshes at all.
add_host_test(test_kanji_model test_kanji_model.c ${CORE}/kanji_model.c)

# What each button does. Three buttons, five screens, four ratings and no touch
# panel — every press costs a panel refresh, so the mapping is worth a test.
add_host_test(test_kanji_nav test_kanji_nav.c
              ${CORE}/kanji_nav.c ${CORE}/kanji_model.c)

# The wire format. Everything the producer can do wrong lands here.
add_host_test(test_kanji_parse test_kanji_parse.c
              ${CORE}/kanji_parse.c ${CORE}/kanji_model.c ${CJSON}/cJSON.c)

add_host_test(test_kanji_catalog test_kanji_catalog.c
              ${CORE}/kanji_catalog.c ${CORE}/kanji_parse.c
              ${CORE}/kanji_model.c ${CJSON}/cJSON.c)
target_compile_definitions(test_kanji_catalog PRIVATE
    CATALOG_FIXTURE="${CATALOG_FIXTURE}"
    CATALOG_BOUNDARY_FIXTURE="${CATALOG_BOUNDARY_FIXTURE}")
target_link_libraries(test_kanji_catalog ZLIB::ZLIB)

# The raw-NOR state journal. The test supplies the same read/write/erase seam
# used by the ESP partition adapter, including torn writes and reboot replay.
add_host_test(test_kanji_state test_kanji_state.c ${CORE}/kanji_state.c)

# Where everything goes on the 648x480 panel. Pure integer geometry, no LVGL —
# including the grade dock's rectangle, which is handed to the partial-refresh
# window verbatim.
add_host_test(test_kanji_layout test_kanji_layout.c
              ${CORE}/ui_kanji_layout.c ${CORE}/kanji_model.c)

# The fetch layer, with the test itself standing in for the HTTP port. Covers
# which failures are distinguished from each other, the product requirement that
# none of them disturbs the card on the glass, and the grading URL — which fails
# silently into somebody's review history if it is built wrong.
add_host_test(test_kanji_service test_kanji_service.c
              ${CORE}/kanji_service.c ${CORE}/kanji_parse.c ${CORE}/kanji_model.c
              ${CJSON}/cJSON.c)

# The built-in demo card, tied to the reference producer's committed output.
add_host_test(test_kanji_mock test_kanji_mock.c
              ${CORE}/kanji_mock.c ${CORE}/kanji_parse.c ${CORE}/kanji_model.c
              ${CJSON}/cJSON.c)

enable_testing()
add_test(NAME kanji_model   COMMAND test_kanji_model)
add_test(NAME kanji_nav     COMMAND test_kanji_nav)
add_test(NAME kanji_parse   COMMAND test_kanji_parse)
add_test(NAME kanji_catalog COMMAND test_kanji_catalog)
add_test(NAME kanji_state   COMMAND test_kanji_state)
add_test(NAME kanji_layout  COMMAND test_kanji_layout)
add_test(NAME kanji_service COMMAND test_kanji_service)
add_test(NAME kanji_mock    COMMAND test_kanji_mock)
add_test(NAME api_json      COMMAND test_api_json)
