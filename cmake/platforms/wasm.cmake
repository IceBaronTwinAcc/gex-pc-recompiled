if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The WebAssembly backend requires the Emscripten toolchain.")
endif()

set(GEX_PLATFORM_RUNTIME hle)
set(GEX_PLATFORM_DESKTOP_TESTS OFF)
set(GEX_PLATFORM_SDL ON)
find_package(OpenAL REQUIRED)
include_directories(SYSTEM "${OPENAL_INCLUDE_DIR}/AL")
set(GEX_PLATFORM_LIBRARIES ${OPENAL_LIBRARY} m)
set(GEX_HOST_SOURCES src/platform/desktop/services.c
    src/platform/desktop/input.c src/platform/hle/memory.c)
set(GEX_LAUNCHER_SOURCES src/main.c src/platform_shims.c src/movie.c
    src/autoplay.c src/gameplay.c src/gamepad.c)
set(GEX_DEFAULT_RENDERER "${PROJECT_SOURCE_DIR}/src/platform/desktop/renderer.c")
set(GEX_DEFAULT_MOVIE_DECODER "${PROJECT_SOURCE_DIR}/src/platform/wasm/movie_decoder.c")

include("${PROJECT_SOURCE_DIR}/cmake/runtime-core.cmake")
set(GEX_RUNTIME_PLATFORM_SOURCES
    src/platform/linux/runtime.c src/platform/linux/filesystem.c
    src/platform/linux/virtual_memory.c src/platform/wasm/font_path.c
    src/platform/hle/flat_memory.c src/platform/linux/audio.c
    src/platform/desktop/font.c src/platform/desktop/screen.c
    src/platform/desktop/present.c)

add_compile_options(-pthread -sUSE_SDL=2 -sUSE_SDL_TTF=2)
add_link_options(
    -pthread
    -sUSE_SDL=2
    -sUSE_SDL_TTF=2
    -sPROXY_TO_PTHREAD=1
    -sPTHREAD_POOL_SIZE=8
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_RUNTIME_METHODS=FS,HEAP32
    -sINITIAL_MEMORY=536870912
    -sMAXIMUM_MEMORY=2147483648
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=33554432
    -Wl,--global-base=16777216
    -sENVIRONMENT=web,worker
    "--shell-file=${PROJECT_SOURCE_DIR}/web/shell.html"
    "--pre-js=${PROJECT_SOURCE_DIR}/web/gex-pre.js"
    "--js-library=${PROJECT_SOURCE_DIR}/web/gex-audio.js")

if(GEX_ENABLE_BGFX_RENDERER)
    add_link_options(
        -sOFFSCREENCANVAS_SUPPORT=1
        # Transfer only when bgfx starts; SDL software presentation needs the page canvas.
        "-sOFFSCREENCANVASES_TO_PTHREAD=\"\""
        "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$GL")
endif()

set(GEX_PLATFORM_WEB ON)
