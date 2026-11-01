# SH GCC 13 can crash when expanding GLdc's debug LTO objects.
function(gex_configure_gldc)
    set_property(TARGET GL PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    target_compile_options(GL PRIVATE -fno-fast-math -ffp-contract=off
        -ffunction-sections -fdata-sections)
    execute_process(COMMAND git describe --abbrev=4 --always --tags
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GLDC_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    configure_file(GL/version.c.in "${CMAKE_CURRENT_BINARY_DIR}/version.c")
    if(NOT GLDC_HAVE_PVR_SCENE_BEGIN_RTT)
        target_sources(GL PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/gldc-rtt.c")
        target_compile_definitions(GL PRIVATE GLDC_HAVE_PVR_SCENE_BEGIN_RTT=1)
        target_compile_options(GL PRIVATE -include "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/gldc-rtt.h")
    endif()
endfunction()
cmake_language(DEFER CALL gex_configure_gldc)
