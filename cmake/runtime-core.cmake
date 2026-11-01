set(hle "${PCRECOMP_DIR}/runtime/win32hle")
set(GEX_RUNTIME_MACHINE_SOURCES "${hle}/host_lite.c" src/platform/runtime_core.c)
set(GEX_RUNTIME_MEMORY_SOURCES "${hle}/guest_mem.c")
set(GEX_RUNTIME_SYSTEM_SOURCES
    "${hle}/kernel32.c" "${hle}/kernel32_ext.c" "${hle}/kernel32_crt.c")
set(GEX_RUNTIME_STORAGE_SOURCES
    "${hle}/hle_path.c" "${hle}/module.c" "${hle}/pe_loader.c" "${hle}/advapi32.c")
set(GEX_RUNTIME_DISPLAY_SOURCES
    "${hle}/user32.c" "${hle}/gdidc.c" "${hle}/gdi32.c")
set(GEX_RUNTIME_AUDIO_SOURCES "${hle}/dsound.c" "${hle}/winmm.c" src/platform/audio.c)
set(GEX_RUNTIME_COMPAT_SOURCES "${hle}/host.c" "${hle}/ole32.c"
    "${hle}/storage.c" "${hle}/lz32.c")
