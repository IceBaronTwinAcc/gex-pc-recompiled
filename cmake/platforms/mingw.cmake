if(NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The MinGW HLE host requires a 64-bit MinGW-w64 compiler.")
endif()
set(GEX_PLATFORM_RUNTIME hle)
set(GEX_PLATFORM_DESKTOP_TESTS ON)
set(GEX_PLATFORM_SDL ON)
set(GEX_OPTIONS_PLATFORM_SOURCE src/platform/desktop/options_platform.c)
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_path(GEX_SDL_INCLUDE_ROOT SDL2/SDL.h REQUIRED)
set(GEX_PLATFORM_LIBRARIES SDL2::SDL2 SDL2_ttf::SDL2_ttf Threads::Threads m advapi32 dsound dxguid)
set(GEX_HOST_LIBRARIES ${GEX_PLATFORM_LIBRARIES})
set(GEX_HOST_SOURCES src/platform/desktop/services.c src/platform/desktop/settings.c
    src/platform/desktop/launcher.c src/platform/desktop/input.c
    src/platform/hle/memory.c)
set(GEX_LAUNCHER_SOURCES src/main.c src/platform_shims.c src/movie.c
    src/autoplay.c src/gameplay.c src/gamepad.c)
set(GEX_DEFAULT_RENDERER "${PROJECT_SOURCE_DIR}/src/platform/desktop/renderer.c")
set(GEX_PACKET_GPU_SOURCE "${PROJECT_SOURCE_DIR}/src/platform/desktop/packet_gpu.c")
set(GEX_DEFAULT_MOVIE_DECODER "${PROJECT_SOURCE_DIR}/src/platform/desktop/movie_decoder.c")
include("${PROJECT_SOURCE_DIR}/cmake/runtime-core.cmake")
set(GEX_RUNTIME_PLATFORM_SOURCES
    src/platform/windows/runtime.c src/platform/windows/filesystem.c src/platform/windows/file_io.c
    src/platform/windows/virtual_memory.c src/platform/windows/font_path.c
    src/platform/hle/flat_memory.c
    src/platform/windows/audio.c src/platform/desktop/font.c
    src/platform/desktop/screen.c src/platform/desktop/screen_policy.c
    src/platform/desktop/present.c)
add_compile_definitions(SDL_MAIN_HANDLED __USE_MINGW_ANSI_STDIO=1)
include_directories("${GEX_SDL_INCLUDE_ROOT}")

function(gex_configure_movie_decoder target source)
    if(source STREQUAL GEX_DEFAULT_MOVIE_DECODER)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
        target_link_libraries(${target} PRIVATE PkgConfig::FFMPEG)
    endif()
endfunction()
