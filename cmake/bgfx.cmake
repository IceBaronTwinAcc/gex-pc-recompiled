enable_language(CXX)
set(GEX_BGFX_CMAKE_DIR "${PROJECT_SOURCE_DIR}/.deps/bgfx.cmake" CACHE PATH
    "bgfx.cmake checkout (revision 1af3832b87ad0588c05f1f0bd7ee17a7b14d2315)")
set(GEX_BGFX_SHADERC "${PROJECT_SOURCE_DIR}/.deps/bgfx-build/bin/host/shaderc" CACHE FILEPATH
    "Host-native bgfx shader compiler")
if(NOT EXISTS "${GEX_BGFX_CMAKE_DIR}/CMakeLists.txt" OR NOT EXISTS "${GEX_BGFX_SHADERC}")
    message(FATAL_ERROR "Prepare bgfx and its host shader compiler with tools/build-bgfx.sh")
endif()
if(EMSCRIPTEN)
    set(BGFX_CONFIG_RENDERER_WEBGPU ON CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_MULTITHREADED OFF CACHE BOOL "" FORCE)
endif()
set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLE_COMMON OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CONFIG_VIDEO OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BGFX_WITH_WAYLAND ON CACHE BOOL "" FORCE)
else()
    set(BGFX_WITH_WAYLAND OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${GEX_BGFX_CMAKE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/bgfx" EXCLUDE_FROM_ALL)
if(EMSCRIPTEN)
    find_package(Git REQUIRED)
    set(gex_bgfx_webgpu_patch "${PROJECT_SOURCE_DIR}/cmake/patches/bgfx-webgpu.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gex_bgfx_webgpu_patch}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BGFX_DIR}" apply --reverse --check
        "${gex_bgfx_webgpu_patch}" RESULT_VARIABLE gex_bgfx_patch_applied
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT gex_bgfx_patch_applied EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BGFX_DIR}" apply --check
            "${gex_bgfx_webgpu_patch}" RESULT_VARIABLE gex_bgfx_patch_check
            ERROR_VARIABLE gex_bgfx_patch_error)
        if(NOT gex_bgfx_patch_check EQUAL 0)
            message(FATAL_ERROR "Cannot apply the WebGPU fixes to ${BGFX_DIR}: ${gex_bgfx_patch_error}")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BGFX_DIR}" apply
            "${gex_bgfx_webgpu_patch}" RESULT_VARIABLE gex_bgfx_patch_result
            ERROR_VARIABLE gex_bgfx_patch_error)
        if(NOT gex_bgfx_patch_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply WebGPU fixes: ${gex_bgfx_patch_error}")
        endif()
    endif()
endif()
if(ANDROID)
    target_link_libraries(bgfx PUBLIC EGL GLESv3 android log)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/licenses")
foreach(dependency bgfx bx bimg)
    string(TOUPPER "${dependency}" dependency_upper)
    configure_file("${${dependency_upper}_DIR}/LICENSE"
        "${CMAKE_CURRENT_BINARY_DIR}/licenses/${dependency}.txt" COPYONLY)
endforeach()
if(MINGW)
    target_link_options(bgfx INTERFACE -static-libgcc -static-libstdc++)
endif()

if(ANDROID OR EMSCRIPTEN)
    set(gex_bgfx_default_gl es)
else()
    set(gex_bgfx_default_gl desktop)
endif()
set(GEX_BGFX_GL_FLAVOUR "${gex_bgfx_default_gl}" CACHE STRING "bgfx GL implementation: desktop or es")
set_property(CACHE GEX_BGFX_GL_FLAVOUR PROPERTY STRINGS desktop es)
if(NOT GEX_BGFX_GL_FLAVOUR MATCHES "^(desktop|es)$")
    message(FATAL_ERROR "GEX_BGFX_GL_FLAVOUR must be desktop or es")
endif()
if(ANDROID AND NOT GEX_BGFX_GL_FLAVOUR STREQUAL "es")
    message(FATAL_ERROR "Android requires GEX_BGFX_GL_FLAVOUR=es; Vulkan is bundled alongside it.")
endif()
if(EMSCRIPTEN)
    target_compile_definitions(bgfx PRIVATE
        BGFX_CONFIG_RENDERER_OPENGL=0
        BGFX_CONFIG_RENDERER_OPENGLES=0
        BGFX_CONFIG_RENDERER_VULKAN=0
        BGFX_CONFIG_RENDERER_DIRECT3D11=0
        BGFX_CONFIG_RENDERER_DIRECT3D12=0
        BGFX_CONFIG_RENDERER_WEBGPU=1)
elseif(NOT APPLE)
    if(GEX_BGFX_GL_FLAVOUR STREQUAL "es")
        set(gex_bgfx_gl 0)
        set(gex_bgfx_es 31)
    else()
        set(gex_bgfx_gl 43)
        set(gex_bgfx_es 0)
    endif()
    target_compile_definitions(bgfx PRIVATE
        BGFX_CONFIG_RENDERER_OPENGL=${gex_bgfx_gl}
        BGFX_CONFIG_RENDERER_OPENGLES=${gex_bgfx_es}
        BGFX_CONFIG_RENDERER_VULKAN=1
        BGFX_CONFIG_RENDERER_DIRECT3D11=$<BOOL:${WIN32}>
        BGFX_CONFIG_RENDERER_DIRECT3D12=$<BOOL:${WIN32}>)
endif()

set(GEX_BGFX_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/bgfx-shaders")
file(MAKE_DIRECTORY "${GEX_BGFX_SHADER_DIR}")
set(gex_bgfx_profiles spirv metal glsl essl)
if(EMSCRIPTEN)
    list(APPEND gex_bgfx_profiles wgsl)
endif()
if(WIN32)
    list(APPEND gex_bgfx_profiles dxbc dxil)
endif()
foreach(profile IN LISTS gex_bgfx_profiles)
    set(shader_profile "${profile}")
    set(shader_command "${GEX_BGFX_SHADERC}")
    if(profile STREQUAL metal)
        set(platform osx)
    elseif(profile STREQUAL dxbc)
        set(platform windows)
        set(shader_profile s_5_0)
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            find_program(GEX_BGFX_WINE wine REQUIRED)
            set(shader_command "${CMAKE_COMMAND}" -E env
                "GEX_BGFX_SHADERC=${GEX_BGFX_SHADERC}"
                bash "${PROJECT_SOURCE_DIR}/tools/bgfx-shaderc-wine.sh")
        endif()
    elseif(profile STREQUAL dxil)
        set(platform windows)
        set(shader_profile s_6_0)
    elseif(profile STREQUAL glsl)
        set(platform linux)
        set(shader_profile 430)
    elseif(profile STREQUAL essl)
        set(platform android)
        set(shader_profile 310_es)
    else()
        set(platform linux)
    endif()
    foreach(shader cs_packet vs_present fs_present)
        if(shader MATCHES "^cs_")
            set(type compute)
        elseif(shader MATCHES "^vs_")
            set(type vertex)
        else()
            set(type fragment)
        endif()
        add_custom_command(OUTPUT "${GEX_BGFX_SHADER_DIR}/${shader}_${profile}.h"
            COMMAND ${shader_command}
                -f "${PROJECT_SOURCE_DIR}/src/platform/bgfx/${shader}.sc"
                -o "${GEX_BGFX_SHADER_DIR}/${shader}_${profile}.h"
                --type "${type}" --platform "${platform}" --profile "${shader_profile}"
                --bin2c "gex_bgfx_${shader}_${profile}" -i "${BGFX_DIR}/src"
                --varyingdef "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc"
            DEPENDS "${PROJECT_SOURCE_DIR}/src/platform/bgfx/${shader}.sc"
                "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc"
                "${GEX_BGFX_SHADERC}"
                "${PROJECT_SOURCE_DIR}/tools/bgfx-shaderc-wine.sh"
            VERBATIM)
        target_sources(gex_packet_gpu PRIVATE "${GEX_BGFX_SHADER_DIR}/${shader}_${profile}.h")
    endforeach()
endforeach()
target_sources(gex_packet_gpu PRIVATE src/platform/bgfx/packet_gpu.c)
if(EMSCRIPTEN)
    add_custom_command(OUTPUT "${GEX_BGFX_SHADER_DIR}/cs_copy_wgsl.h"
        COMMAND "${GEX_BGFX_SHADERC}"
            -f "${PROJECT_SOURCE_DIR}/src/platform/bgfx/cs_copy.sc"
            -o "${GEX_BGFX_SHADER_DIR}/cs_copy_wgsl.h"
            --type compute --platform linux --profile wgsl
            --bin2c gex_bgfx_cs_copy_wgsl -i "${BGFX_DIR}/src"
            --varyingdef "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc"
        DEPENDS "${PROJECT_SOURCE_DIR}/src/platform/bgfx/cs_copy.sc"
            "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc" "${GEX_BGFX_SHADERC}"
        VERBATIM)
    target_sources(gex_packet_gpu PRIVATE "${GEX_BGFX_SHADER_DIR}/cs_copy_wgsl.h")
    add_custom_command(OUTPUT "${GEX_BGFX_SHADER_DIR}/fs_present_packet_wgsl.h"
        COMMAND "${GEX_BGFX_SHADERC}"
            -f "${PROJECT_SOURCE_DIR}/src/platform/bgfx/fs_present_packet.sc"
            -o "${GEX_BGFX_SHADER_DIR}/fs_present_packet_wgsl.h"
            --type fragment --platform linux --profile wgsl
            --bin2c gex_bgfx_fs_present_packet_wgsl -i "${BGFX_DIR}/src"
            --varyingdef "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc"
        DEPENDS "${PROJECT_SOURCE_DIR}/src/platform/bgfx/fs_present_packet.sc"
            "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc" "${GEX_BGFX_SHADERC}"
        VERBATIM)
    target_sources(gex_packet_gpu PRIVATE "${GEX_BGFX_SHADER_DIR}/fs_present_packet_wgsl.h")
endif()
target_include_directories(gex_packet_gpu PRIVATE "${GEX_BGFX_SHADER_DIR}")
target_link_libraries(gex_packet_gpu PRIVATE bgfx bx gex_renderer)

if(BUILD_TESTING AND GEX_ENABLE_SOFTWARE_RENDERER)
    add_executable(gex_bgfx_test tests/packet_gpu.c)
    target_compile_definitions(gex_bgfx_test PRIVATE GEX_TEST_BGFX=1)
    target_link_libraries(gex_bgfx_test PRIVATE gex_renderer gex_packet_gpu ${GEX_PLATFORM_LIBRARIES})
    add_test(NAME bgfx-packet-differential COMMAND gex_bgfx_test)
    set_tests_properties(bgfx-packet-differential PROPERTIES TIMEOUT 180 LABELS gpu
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
if(BUILD_TESTING)
    add_executable(gex_bgfx_present_test tests/bgfx_present.c)
    target_link_libraries(gex_bgfx_present_test PRIVATE gex_renderer gex_packet_gpu ${GEX_PLATFORM_LIBRARIES})
    add_test(NAME bgfx-presentation COMMAND gex_bgfx_present_test)
    set_tests_properties(bgfx-presentation PROPERTIES TIMEOUT 30 LABELS gpu
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
if(BUILD_TESTING)
    if(ANDROID)
        set(gex_bgfx_test_apis vulkan opengles)
    elseif(APPLE)
        set(gex_bgfx_test_apis metal)
    else()
        if(GEX_BGFX_GL_FLAVOUR STREQUAL "es")
            set(gex_bgfx_test_apis opengles)
        else()
            set(gex_bgfx_test_apis opengl)
        endif()
        if(WIN32)
            list(APPEND gex_bgfx_test_apis direct3d11 direct3d12)
        endif()
    endif()
    foreach(api IN LISTS gex_bgfx_test_apis)
        add_test(NAME bgfx-presentation-${api} COMMAND gex_bgfx_present_test)
        set_tests_properties(bgfx-presentation-${api} PROPERTIES TIMEOUT 30 LABELS gpu
            ENVIRONMENT "GEX_BGFX_API=${api};EGL_PLATFORM=surfaceless;SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
        if(GEX_ENABLE_SOFTWARE_RENDERER)
            add_test(NAME bgfx-packet-${api} COMMAND gex_bgfx_test)
            set_tests_properties(bgfx-packet-${api} PROPERTIES TIMEOUT 180 LABELS gpu
                ENVIRONMENT "GEX_BGFX_API=${api};EGL_PLATFORM=surfaceless;SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
        endif()
    endforeach()
endif()
