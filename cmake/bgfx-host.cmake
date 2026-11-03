function(gex_configure_bgfx_shaderc)
    set_target_properties(shaderc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/host")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        target_include_directories(shaderc PRIVATE "${BGFX_DIR}/3rdparty/d3d4linux/include")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin/windows")
        file(COPY "${BGFX_DIR}/tools/bin/windows/d3d4linux.exe"
            "${BGFX_DIR}/tools/bin/windows/d3dcompiler_47.dll"
            DESTINATION "${CMAKE_BINARY_DIR}/bin/windows")
        file(COPY "${BGFX_DIR}/tools/bin/linux/libdxil.so"
            DESTINATION "${CMAKE_BINARY_DIR}/bin/host")
    endif()
endfunction()

cmake_language(DEFER CALL gex_configure_bgfx_shaderc)
