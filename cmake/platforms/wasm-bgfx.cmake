set(GEX_BGFX_PACKET_POLICY src/platform/wasm/bgfx_packet_policy.c)
set(GEX_BGFX_PRESENT_POLICY src/platform/wasm/bgfx_present_policy.c)
set(gex_bgfx_default_gl es)
set(gex_bgfx_extra_profiles wgsl)

function(gex_bgfx_platform_before_dependencies)
    set(BGFX_CONFIG_RENDERER_WEBGPU ON CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_MULTITHREADED OFF CACHE BOOL "" FORCE)
endfunction()

function(gex_bgfx_platform_after_dependencies)
    find_package(Git REQUIRED)
    set(patch "${PROJECT_SOURCE_DIR}/cmake/platforms/wasm/bgfx-webgpu.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BGFX_DIR}" apply --reverse --check
        "${patch}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT applied EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BGFX_DIR}" apply --check
            "${patch}" RESULT_VARIABLE check ERROR_VARIABLE error)
        if(NOT check EQUAL 0)
            message(FATAL_ERROR "Cannot apply the WebGPU fixes to ${BGFX_DIR}: ${error}")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BGFX_DIR}" apply
            "${patch}" RESULT_VARIABLE result ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Failed to apply WebGPU fixes: ${error}")
        endif()
    endif()
    target_compile_definitions(bgfx PRIVATE
        BGFX_CONFIG_RENDERER_OPENGL=0
        BGFX_CONFIG_RENDERER_OPENGLES=0
        BGFX_CONFIG_RENDERER_VULKAN=0
        BGFX_CONFIG_RENDERER_DIRECT3D11=0
        BGFX_CONFIG_RENDERER_DIRECT3D12=0
        BGFX_CONFIG_RENDERER_WEBGPU=1)
endfunction()

function(gex_bgfx_platform_shaders)
    foreach(shader cs_copy fs_present_packet)
        if(shader STREQUAL cs_copy)
            set(type compute)
        else()
            set(type fragment)
        endif()
        add_custom_command(OUTPUT "${GEX_BGFX_SHADER_DIR}/${shader}_wgsl.h"
            COMMAND "${GEX_BGFX_SHADERC}"
                -f "${PROJECT_SOURCE_DIR}/src/platform/wasm/${shader}.sc"
                -o "${GEX_BGFX_SHADER_DIR}/${shader}_wgsl.h"
                --type "${type}" --platform linux --profile wgsl
                --bin2c "gex_bgfx_${shader}_wgsl" -i "${BGFX_DIR}/src"
                --varyingdef "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc"
            DEPENDS "${PROJECT_SOURCE_DIR}/src/platform/wasm/${shader}.sc"
                "${PROJECT_SOURCE_DIR}/src/platform/bgfx/varying.def.sc" "${GEX_BGFX_SHADERC}"
            VERBATIM)
        target_sources(gex_packet_gpu PRIVATE "${GEX_BGFX_SHADER_DIR}/${shader}_wgsl.h")
    endforeach()
endfunction()

function(gex_bgfx_platform_launcher target)
    target_link_options(${target} PRIVATE -sASYNCIFY=1)
endfunction()
