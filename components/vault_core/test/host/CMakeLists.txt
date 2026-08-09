cmake_minimum_required(VERSION 3.16)
project(fortune_core_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Paths relative to this file.
set(CORE   ${CMAKE_CURRENT_SOURCE_DIR}/../..)         # components/fortune_core
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

# The fortune itself: pure, no cJSON, no network.
add_host_test(test_omikuji test_omikuji.c ${CORE}/omikuji.c)
add_host_test(test_saju    test_saju.c    ${CORE}/saju.c)

# Weather parse (Open-Meteo geocoding + forecast), fixtures in ./fixtures.
add_host_test(test_weather test_weather.c ${CORE}/weather_parse.c ${CJSON}/cJSON.c)

# The companion-app JSON. cJSON is linked only to validate the output — the
# serializer itself writes bytes directly.
add_host_test(test_api_json test_api_json.c ${CORE}/device_api_json.c ${CJSON}/cJSON.c)

enable_testing()
add_test(NAME omikuji  COMMAND test_omikuji)
add_test(NAME saju     COMMAND test_saju)
add_test(NAME weather  COMMAND test_weather)
add_test(NAME api_json COMMAND test_api_json)
