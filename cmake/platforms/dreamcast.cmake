if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Dreamcast backend requires the KallistiOS SH4 toolchain.")
endif()
set(GEX_PLATFORM_RUNTIME hle)
set(GEX_PLATFORM_DESKTOP_TESTS OFF)
set(GEX_PVR_AVAILABLE ON)
target_compile_definitions(gex_renderer PUBLIC GEX_NATIVE_GAMEPLAY_WIDTH=320 GEX_NATIVE_GAMEPLAY_HEIGHT=240)
target_compile_definitions(gex_renderer PRIVATE GEX_PACKET_SPAN_LIMIT=256)
set(GEX_PLATFORM_LIBRARIES m)
set(GEX_HOST_SOURCES src/platform/dreamcast/services.c
    src/platform/dreamcast/input.c src/platform/hle/memory.c)
set(GEX_LAUNCHER_SOURCES src/main.c src/platform/dreamcast/main.c
    src/platform_shims.c src/movie.c src/autoplay.c src/gameplay.c src/gamepad.c)
set_source_files_properties(src/main.c PROPERTIES COMPILE_DEFINITIONS main=gex_portable_main)
set(GEX_DEFAULT_RENDERER "${PROJECT_SOURCE_DIR}/src/platform/dreamcast/renderer.c")
set(GEX_DEFAULT_MOVIE_DECODER "${PROJECT_SOURCE_DIR}/src/platform/dreamcast/movie_decoder.c")
include("${PROJECT_SOURCE_DIR}/cmake/runtime-core.cmake")
set(GEX_RUNTIME_PLATFORM_SOURCES src/platform/dreamcast/runtime.c
    src/platform/dreamcast/filesystem.c src/platform/dreamcast/memory.c
    src/platform/dreamcast/audio.c src/platform/dreamcast/font.c
    src/platform/dreamcast/screen.c)
add_compile_definitions(RECOMP_FLAT_MEMORY RECOMP_UNALIGNED_MEMORY HLE_GUEST_STACK_SIZE=131072
    HLE_MAX_WINDOWS=32 HLE_MAX_MENUS=8 HLE_MAX_DC=64
    HLE_MAX_FONTS=16 HLE_MAX_GOBJ=128 HLE_MAX_DIBS=64)
add_compile_options(-Os -g -fno-lto -fno-fast-math -ffp-contract=off
    -ffunction-sections -fdata-sections)
foreach(target gex_game gex_renderer)
    target_compile_options(${target} PRIVATE -O2 -g -fomit-frame-pointer -fno-lto -fno-fast-math
        -ffp-contract=off -ffunction-sections -fdata-sections)
endforeach()
set_source_files_properties(src/platform/dreamcast/renderer.c
    src/platform/dreamcast/packet_gpu.c src/platform/gl/packet_gpu.c src/sha256.c
    "${PCRECOMP_DIR}/runtime/win32hle/dsound.c"
    PROPERTIES COMPILE_OPTIONS -O2)
add_link_options(-fno-lto "-Wl,--gc-sections" "-Wl,-Map=${CMAKE_BINARY_DIR}/gex_recomp.map")
if(BUILD_TESTING)
    add_executable(gex_fs_test tests/dreamcast_filesystem.c
        src/platform/dreamcast/filesystem.c)
    target_include_directories(gex_fs_test PRIVATE src
        "${PCRECOMP_DIR}/runtime/win32hle" "${PCRECOMP_DIR}/runtime/recomp32")
    add_executable(gex_video_test tests/dreamcast_video.c src/renderer.c
        src/platform/dreamcast/renderer.c)
    target_compile_options(gex_video_test PRIVATE -O2)
    target_include_directories(gex_video_test PRIVATE src
        "${PCRECOMP_DIR}/runtime/win32hle" "${PCRECOMP_DIR}/runtime/recomp32")
    add_executable(gex_aica_test tests/dreamcast_audio.c)
    target_include_directories(gex_aica_test PRIVATE src
        "${PCRECOMP_DIR}/runtime/win32hle" "${PCRECOMP_DIR}/runtime/recomp32")
endif()
