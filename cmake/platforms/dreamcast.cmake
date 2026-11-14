if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Dreamcast backend requires the KallistiOS SH4 toolchain.")
endif()
set(GEX_PLATFORM_RUNTIME hle)
set(GEX_PLATFORM_DESKTOP_TESTS OFF)
set(GEX_PVR_AVAILABLE ON)
option(GEX_DREAMCAST_NATIVE_MAP "Enable experimental native map draw generation" OFF)
option(GEX_DREAMCAST_PACKETLESS_TITLE "Enable experimental packetless title draws" OFF)
option(GEX_DREAMCAST_PACKETLESS_GAME "Enable experimental packetless map and gameplay draws" OFF)
if(GEX_DREAMCAST_NATIVE_MAP)
    list(APPEND GEX_PLATFORM_PACKET_GPU_DEFINITIONS GEX_DREAMCAST_NATIVE_MAP)
endif()
if(GEX_DREAMCAST_PACKETLESS_TITLE)
    list(APPEND GEX_PLATFORM_PACKET_GPU_DEFINITIONS GEX_DREAMCAST_PACKETLESS_TITLE)
endif()
if(GEX_DREAMCAST_PACKETLESS_GAME)
    list(APPEND GEX_PLATFORM_PACKET_GPU_DEFINITIONS GEX_DREAMCAST_PACKETLESS_GAME)
endif()
target_compile_definitions(gex_renderer PUBLIC GEX_NATIVE_GAMEPLAY_WIDTH=320 GEX_NATIVE_GAMEPLAY_HEIGHT=240)
target_compile_definitions(gex_renderer PRIVATE GEX_PACKET_SPAN_LIMIT=256)
set(GEX_PLATFORM_LIBRARIES m)
set(GEX_HOST_SOURCES src/platform/dreamcast/services.c
    src/platform/dreamcast/input.c src/platform/hle/memory.c)
set(GEX_LAUNCHER_SOURCES src/main.c src/platform/dreamcast/main.c
    src/platform_shims.c src/movie.c src/autoplay.c src/gameplay.c src/gamepad.c)
set_source_files_properties(src/main.c PROPERTIES COMPILE_DEFINITIONS main=gex_portable_main)
set(GEX_DEFAULT_RENDERER "${PROJECT_SOURCE_DIR}/src/platform/dreamcast/renderer.c")
if(GEX_MOVIE_DECODER_SOURCE STREQUAL "${PROJECT_SOURCE_DIR}/src/platform/dreamcast/movie_decoder.c")
    unset(GEX_MOVIE_DECODER_SOURCE CACHE)
endif()
set(GEX_DEFAULT_MOVIE_DECODER "${PROJECT_SOURCE_DIR}/src/platform/dreamcast/movie_mpeg.c")
set(GEX_DREAMCAST_MPEG_ROOT "${PROJECT_SOURCE_DIR}/.deps/dreamcast-pl-mpeg"
    CACHE PATH "Pinned Dreamcast pl_mpeg source")
set(GEX_DREAMCAST_FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/.deps/dreamcast-ffmpeg"
    CACHE PATH "Minimal KallistiOS FFmpeg installation")
function(gex_configure_movie_decoder target source)
    if(source STREQUAL GEX_DEFAULT_MOVIE_DECODER)
        set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS GEX_DREAMCAST_PVR_MOVIES)
        if(NOT EXISTS "${GEX_DREAMCAST_MPEG_ROOT}/pl_mpeg.h")
            message(FATAL_ERROR "Dreamcast pl_mpeg is missing; run tools/prepare-dreamcast-mpeg.sh")
        endif()
        target_include_directories(${target} PRIVATE "${GEX_DREAMCAST_MPEG_ROOT}")
        target_compile_options(${target} PRIVATE -O2)
    elseif(source STREQUAL "${PROJECT_SOURCE_DIR}/src/platform/desktop/movie_decoder.c")
        set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS GEX_DREAMCAST_PVR_MOVIES)
        target_sources(${target} PRIVATE src/platform/dreamcast/movie_io.c
            src/platform/dreamcast/movie_pixels.c)
        target_compile_definitions(${target} PRIVATE GEX_MOVIE_CUSTOM_IO GEX_MOVIE_CUSTOM_PIXELS)
        target_compile_options(${target} PRIVATE -O2)
        target_include_directories(${target} PRIVATE "${GEX_DREAMCAST_FFMPEG_ROOT}/include")
        foreach(component avformat avcodec avutil)
            find_library(GEX_DC_${component}_LIBRARY NAMES ${component}
                PATHS "${GEX_DREAMCAST_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
            target_link_libraries(${target} PRIVATE "${GEX_DC_${component}_LIBRARY}")
        endforeach()
        target_link_libraries(${target} PRIVATE m)
    elseif(source STREQUAL "${PROJECT_SOURCE_DIR}/src/platform/dreamcast/movie_metadata.c")
        set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS GEX_DREAMCAST_SKIP_MOVIES)
        if(NOT GEX_ENABLE_GLDC_RENDERER)
            target_compile_options(gex_game PRIVATE -O2)
        endif()
    endif()
endfunction()
include("${PROJECT_SOURCE_DIR}/cmake/runtime-core.cmake")
set(GEX_RUNTIME_PLATFORM_SOURCES src/platform/dreamcast/runtime.c
    src/platform/dreamcast/filesystem.c src/platform/dreamcast/memory.c
    src/platform/dreamcast/audio.c src/platform/dreamcast/font.c
    src/platform/dreamcast/screen.c)
set_property(SOURCE src/platform/dreamcast/audio.c tests/dreamcast_audio.c
    APPEND PROPERTY INCLUDE_DIRECTORIES "$ENV{KOS_BASE}/kernel/arch/dreamcast/sound"
    "$ENV{KOS_BASE}/kernel/arch/dreamcast/include/dc/sound")
add_compile_definitions(RECOMP_FLAT_MEMORY RECOMP_UNALIGNED_MEMORY HLE_GUEST_STACK_SIZE=131072
    HLE_MAX_WINDOWS=32 HLE_MAX_MENUS=8 HLE_MAX_DC=64
    HLE_MAX_FONTS=16 HLE_MAX_GOBJ=128 HLE_MAX_DIBS=64)
add_compile_options(-Os -g -fno-lto -fno-fast-math -ffp-contract=off
    -ffunction-sections -fdata-sections)
target_compile_options(gex_game PRIVATE -Os -g -fomit-frame-pointer -fno-lto
    -fno-fast-math -ffp-contract=off -ffunction-sections -fdata-sections)
target_compile_options(gex_renderer PRIVATE -O2 -g -fomit-frame-pointer -fno-lto -fno-fast-math
    -ffp-contract=off -ffunction-sections -fdata-sections)
set_source_files_properties(src/main.c src/platform/dreamcast/renderer.c
    src/platform/dreamcast/packet_gpu.c src/platform/dreamcast/title_cel.c
    src/platform/dreamcast/title_draw.c src/platform/dreamcast/title_layout.c
    src/platform/dreamcast/title_plan.c src/platform/dreamcast/map_plan.c
    src/platform/dreamcast/map_draw.c src/platform/gl/packet_gpu.c src/sha256.c
    "${PCRECOMP_DIR}/runtime/win32hle/dsound.c"
    PROPERTIES COMPILE_OPTIONS -O2)
set_source_files_properties(src/graphics_guest.c
    PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer")
add_link_options(-fno-lto "-Wl,--gc-sections" "-Wl,-Map=${CMAKE_BINARY_DIR}/gex_recomp.map")
if(BUILD_TESTING)
    if(GEX_MOVIE_DECODER_SOURCE STREQUAL GEX_DEFAULT_MOVIE_DECODER)
        add_executable(gex_dc_movie_test tests/dreamcast_mpeg.c)
    else()
        add_executable(gex_dc_movie_test tests/dreamcast_movie.c)
    endif()
    target_include_directories(gex_dc_movie_test PRIVATE src)
    target_link_libraries(gex_dc_movie_test PRIVATE gex_movie_decoder)
    if(GEX_MOVIE_DECODER_SOURCE STREQUAL "${PROJECT_SOURCE_DIR}/src/platform/desktop/movie_decoder.c")
        target_compile_definitions(gex_dc_movie_test PRIVATE GEX_MOVIE_CUSTOM_IO)
        target_include_directories(gex_dc_movie_test PRIVATE "${GEX_DREAMCAST_FFMPEG_ROOT}/include")
    endif()
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
    add_executable(gex_vm_test tests/dreamcast_memory.c)
    target_include_directories(gex_vm_test PRIVATE src
        "${PCRECOMP_DIR}/runtime/win32hle" "${PCRECOMP_DIR}/runtime/recomp32")
endif()
