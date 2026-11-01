if(NOT DEFINED ENV{KOS_BASE})
    message(FATAL_ERROR "Source the KallistiOS environ.sh before configuring.")
endif()
include("$ENV{KOS_BASE}/utils/cmake/kallistios.toolchain.cmake")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(GEX_PLATFORM_FILE "${CMAKE_CURRENT_LIST_DIR}/../platforms/dreamcast.cmake"
    CACHE FILEPATH "Dreamcast platform manifest")
