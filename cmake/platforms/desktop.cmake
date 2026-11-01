# SDL/POSIX host. Guest-facing declarations remain in win32hle.h/host.h.
set(GEX_PLATFORM_RUNTIME hle)
set(GEX_PLATFORM_DESKTOP_TESTS ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
find_package(OpenAL REQUIRED)
find_package(Threads REQUIRED)

set(GEX_PLATFORM_LIBRARIES PkgConfig::SDL2 PkgConfig::SDL2_TTF Threads::Threads OpenAL::OpenAL m)
set(GEX_HOST_LIBRARIES ${GEX_PLATFORM_LIBRARIES})
set(GEX_HOST_SOURCES
    src/platform/desktop/services.c
    src/platform/desktop/input.c src/platform/hle/memory.c)
set(GEX_LAUNCHER_SOURCES src/main.c src/platform_shims.c src/movie.c
    src/autoplay.c src/gameplay.c src/gamepad.c)
set(GEX_DEFAULT_RENDERER "${PROJECT_SOURCE_DIR}/src/platform/desktop/renderer.c")
set(GEX_PACKET_GPU_SOURCE "${PROJECT_SOURCE_DIR}/src/platform/desktop/packet_gpu.c")
set(GEX_DEFAULT_MOVIE_DECODER "${PROJECT_SOURCE_DIR}/src/platform/desktop/movie_decoder.c")

include("${PROJECT_SOURCE_DIR}/cmake/runtime-core.cmake")
set(GEX_RUNTIME_PLATFORM_SOURCES
    src/platform/linux/runtime.c src/platform/linux/filesystem.c
    src/platform/linux/virtual_memory.c src/platform/linux/font_path.c
    src/platform/hle/flat_memory.c
    src/platform/linux/audio.c src/platform/desktop/font.c
    src/platform/desktop/screen.c src/platform/desktop/present.c)

function(gex_configure_movie_decoder target source)
    if(source STREQUAL GEX_DEFAULT_MOVIE_DECODER)
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
        target_link_libraries(${target} PRIVATE PkgConfig::FFMPEG)
    endif()
endfunction()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(gex_native_audio_test tests/native_audio_loopback.c)
    target_include_directories(gex_native_audio_test PRIVATE src
        "${PCRECOMP_DIR}/runtime/win32hle" "${PCRECOMP_DIR}/runtime/recomp32")
    target_link_libraries(gex_native_audio_test PRIVATE gex_hle ${GEX_PLATFORM_LIBRARIES})
    add_test(NAME native-audio-loopback COMMAND gex_native_audio_test)
    set_tests_properties(native-audio-loopback PROPERTIES TIMEOUT 10
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
