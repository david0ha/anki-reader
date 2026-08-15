cmake_minimum_required(VERSION 3.16)
project(catalog_store_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(STORE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ROOT ${STORE}/../..)
set(CORE ${ROOT}/components/vault_core)
set(CJSON ${ROOT}/third_party/cJSON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(ZLIB REQUIRED)

set(CATALOG_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/offline_catalog.bin)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/gen_offline_catalog.py
            --fixture-json ${ROOT}/tools/fixtures/offline_catalog.json
            --output ${CATALOG_FIXTURE} --partition-size 0x770000 --seed 0 --verify
    RESULT_VARIABLE CATALOG_FIXTURE_RESULT)
if(NOT CATALOG_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "could not generate catalog-store fixture")
endif()

add_library(catalog_store_core STATIC ${STORE}/catalog_store_core.c)
target_include_directories(catalog_store_core PRIVATE ${STORE} ${CORE}/include)
target_compile_options(catalog_store_core PRIVATE
    -Wall -Wextra -Werror -Wframe-larger-than=2048 -fstack-usage)

add_executable(test_catalog_store_core
    test_catalog_store_core.c
    ${CORE}/kanji_catalog.c
    ${CORE}/kanji_state.c
    ${CORE}/kanji_parse.c
    ${CORE}/kanji_model.c
    ${CORE}/kanji_clock.c
    ${CORE}/kanji_fsrs.c
    ${CJSON}/cJSON.c)
target_include_directories(test_catalog_store_core PRIVATE
    ${STORE} ${CORE}/include ${CJSON})
target_compile_definitions(test_catalog_store_core PRIVATE
    CATALOG_FIXTURE="${CATALOG_FIXTURE}")
target_compile_options(test_catalog_store_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_catalog_store_core PRIVATE catalog_store_core ZLIB::ZLIB m)

add_executable(test_catalog_store_adapter
    test_catalog_store_adapter.c
    ${STORE}/catalog_store.c
    ${CORE}/kanji_clock.c)
target_include_directories(test_catalog_store_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${STORE} ${CORE}/include)
target_compile_options(test_catalog_store_adapter PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_catalog_store_adapter PRIVATE ZLIB::ZLIB m)

enable_testing()
add_test(NAME catalog_store_core COMMAND test_catalog_store_core)
add_test(NAME catalog_store_adapter COMMAND test_catalog_store_adapter)
