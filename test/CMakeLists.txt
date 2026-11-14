cmake_minimum_required(VERSION 3.20)
project(brain_display_tests LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(raster_test
    raster_test.c
    raster_ref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/raster.c)
target_include_directories(raster_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(raster_test PRIVATE -Wall -Wextra -O2)

add_executable(compose_test
    compose_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/compose.c)
target_include_directories(compose_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(compose_test PRIVATE -Wall -Wextra -O2)

add_executable(raster2_test
    raster2_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/raster.c)
target_include_directories(raster2_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(raster2_test PRIVATE -Wall -Wextra -O2)

add_executable(nightlight_test
    nightlight_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/nightlight.c)
target_include_directories(nightlight_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(nightlight_test PRIVATE -Wall -Wextra -O2)

enable_testing()
add_test(NAME raster_random COMMAND raster_test 300000)
add_test(NAME raster_random_seed2 COMMAND raster_test 300000 2)
add_test(NAME raster_random_seed3 COMMAND raster_test 300000 12345)
add_test(NAME compose COMMAND compose_test)
add_test(NAME raster2 COMMAND raster2_test)
add_test(NAME nightlight COMMAND nightlight_test)
