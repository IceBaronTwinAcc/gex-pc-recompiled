set(GEX_PLATFORM_RUNTIME native32)
set(GEX_PLATFORM_DESKTOP_TESTS OFF)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Windows native32 host requires MSVC x86: configure with -A Win32.")
endif()
set(GEX_HOST_SOURCES src/platform/windows/memory.c src/platform/windows/file_io.c)
set(GEX_PLATFORM_LIBRARIES "")
set(GEX_LAUNCHER_SOURCES src/platform/windows/main.c
    "${PCRECOMP_DIR}/runtime/native32/native32.c"
    "${PCRECOMP_DIR}/runtime/native32/eh32.c"
    "${PCRECOMP_DIR}/runtime/native32/x87crt.c"
    "${PCRECOMP_DIR}/runtime/recomp32/image_loader.c")
set(GEX_DEFAULT_RENDERER "${PROJECT_SOURCE_DIR}/src/platform/windows/renderer.c")
