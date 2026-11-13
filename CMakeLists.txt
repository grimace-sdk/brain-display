cmake_minimum_required(VERSION 3.20)
project(brain_display LANGUAGES C)

include(Grimace)

option(BRAIN_NO_PXP "Use the CPU transpose instead of the PXP" OFF)

set(BRAIN_DISPLAY_SOURCES
    src/driver.c
    src/raster.c
    src/compose.c
    src/board.c
    src/log.c
    src/panel.c
    src/refresh.c
    src/te.c
    src/pxp.c
    src/irq.c
    src/timebase.c
    src/present.c
)

foreach(board pwsh2 pwa7300)
    grimace_add_library(brain_display_${board} ${BRAIN_DISPLAY_SOURCES}
        OUTPUT brain-display-${board}.dll EXPORTS DrvEnableDriver HALInit)
    target_link_libraries(brain_display_${board} PRIVATE grimace::ceddk)
    target_compile_options(brain_display_${board} PRIVATE -Wall -Werror -Wno-unused-parameter -O2 -g)
    target_compile_definitions(brain_display_${board} PRIVATE UNICODE _UNICODE BRAIN_BOARD_${board})
    if(BRAIN_NO_PXP)
        target_compile_definitions(brain_display_${board} PRIVATE BRAIN_NO_PXP)
    endif()
endforeach()
