if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The WebAssembly backend requires the Emscripten toolchain.")
endif()

# The browser build intentionally uses SDL's software packet renderer only,
# overriding renderer options left in an existing build directory.
set(GEX_ENABLE_SOFTWARE_RENDERER ON CACHE BOOL "Build CPU packet rasterization" FORCE)
set(GEX_ENABLE_OPENGL_RENDERER OFF CACHE BOOL "Build OpenGL packet rasterization" FORCE)
set(GEX_ENABLE_PVR_RENDERER OFF CACHE BOOL "Build Dreamcast PVR packet rasterization" FORCE)
set(GEX_ENABLE_GLDC_RENDERER OFF CACHE BOOL "Build portable fixed-function GLdc packet rasterization" FORCE)
set(GEX_ENABLE_BGFX_RENDERER OFF CACHE BOOL "Build portable bgfx packet rasterization" FORCE)

set(GEX_PLATFORM_RUNTIME hle)
set(GEX_PLATFORM_DESKTOP_TESTS OFF)
set(GEX_PLATFORM_PASSWORD_SAVES ON)
set(GEX_PLATFORM_SDL ON)
set(GEX_BGFX_PLATFORM_FILE "${PROJECT_SOURCE_DIR}/cmake/platforms/wasm-bgfx.cmake")
set(GEX_OPTIONS_PLATFORM_SOURCE src/platform/wasm/options_platform.c)
find_package(OpenAL REQUIRED)
include_directories(SYSTEM "${OPENAL_INCLUDE_DIR}/AL")
set(GEX_PLATFORM_LIBRARIES ${OPENAL_LIBRARY} m)
set(GEX_HOST_SOURCES src/platform/desktop/services.c src/platform/wasm/settings.c
    src/platform/wasm/launcher.c
    src/platform/desktop/input.c src/platform/hle/memory.c)
set(GEX_LAUNCHER_SOURCES src/main.c src/platform_shims.c src/movie.c
    src/autoplay.c src/gameplay.c src/gamepad.c)
set(GEX_DEFAULT_RENDERER "${PROJECT_SOURCE_DIR}/src/platform/desktop/renderer.c")
set(GEX_DEFAULT_MOVIE_DECODER "${PROJECT_SOURCE_DIR}/src/platform/desktop/movie_decoder.c")
if(GEX_MOVIE_DECODER_SOURCE STREQUAL
   "${PROJECT_SOURCE_DIR}/src/platform/wasm/movie_decoder.c")
    set(GEX_MOVIE_DECODER_SOURCE "${GEX_DEFAULT_MOVIE_DECODER}" CACHE FILEPATH
        "C source implementing src/movie_decoder.h (defaults to desktop FFmpeg)" FORCE)
endif()
set(GEX_WASM_FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/.deps/wasm-ffmpeg" CACHE PATH
    "Prefix containing the minimal WebAssembly FFmpeg build")

function(gex_configure_movie_decoder target source)
    if("${source}" STREQUAL "${GEX_DEFAULT_MOVIE_DECODER}")
        set(ffmpeg_libraries
            "${GEX_WASM_FFMPEG_ROOT}/lib/libavformat.a"
            "${GEX_WASM_FFMPEG_ROOT}/lib/libavcodec.a"
            "${GEX_WASM_FFMPEG_ROOT}/lib/libswscale.a"
            "${GEX_WASM_FFMPEG_ROOT}/lib/libavutil.a")
        foreach(path IN LISTS ffmpeg_libraries)
            if(NOT EXISTS "${path}")
                message(FATAL_ERROR
                    "Missing ${path}; build WebAssembly FFmpeg with tools/build-wasm.sh.")
            endif()
        endforeach()
        if(NOT EXISTS "${GEX_WASM_FFMPEG_ROOT}/include/libavcodec/avcodec.h" OR
           NOT EXISTS "${GEX_WASM_FFMPEG_ROOT}/licenses/FFmpeg.txt")
            message(FATAL_ERROR
                "Incomplete WebAssembly FFmpeg install; run tools/build-wasm.sh.")
        endif()
        target_include_directories(${target} PRIVATE "${GEX_WASM_FFMPEG_ROOT}/include")
        target_link_libraries(${target} PUBLIC ${ffmpeg_libraries})
    endif()
endfunction()

function(gex_configure_platform_launcher target)
    set(logo "${PROJECT_SOURCE_DIR}/docs/GexRecompLogo.png")
    set(ffmpeg_license "${GEX_WASM_FFMPEG_ROOT}/licenses/FFmpeg.txt")
    set_target_properties(${target} PROPERTIES
        SUFFIX ".html" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${PROJECT_SOURCE_DIR}/web/shell.html" "${PROJECT_SOURCE_DIR}/web/gex-pre.js"
        "${PROJECT_SOURCE_DIR}/web/gex-coi-serviceworker.js"
        "${PROJECT_SOURCE_DIR}/web/gex-audio.js" "${logo}" "${ffmpeg_license}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PROJECT_SOURCE_DIR}/web/gex-coi-serviceworker.js"
            "$<TARGET_FILE_DIR:${target}>/gex-coi-serviceworker.js"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${logo}" "$<TARGET_FILE_DIR:${target}>/GexRecompLogo.png"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:${target}>/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ffmpeg_license}" "$<TARGET_FILE_DIR:${target}>/licenses/FFmpeg.txt"
        VERBATIM)
endfunction()

include("${PROJECT_SOURCE_DIR}/cmake/runtime-core.cmake")
set(GEX_RUNTIME_PLATFORM_SOURCES
    src/platform/linux/runtime.c src/platform/wasm/runtime_execution.c
    src/platform/linux/filesystem.c
    src/platform/linux/virtual_memory.c src/platform/wasm/file_mapping.c
    src/platform/wasm/font_path.c
    src/platform/wasm/fullscreen.c
    src/platform/wasm/playback.c
    src/platform/wasm/flat_memory.c src/platform/linux/audio.c src/platform/wasm/audio_limiter.c
    src/platform/desktop/font.c src/platform/desktop/screen.c src/platform/wasm/screen_policy.c
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
    -lidbfs.js
    -sENVIRONMENT=web,worker
    "--shell-file=${PROJECT_SOURCE_DIR}/web/shell.html"
    "--pre-js=${PROJECT_SOURCE_DIR}/web/gex-pre.js"
    "--js-library=${PROJECT_SOURCE_DIR}/web/gex-audio.js")
