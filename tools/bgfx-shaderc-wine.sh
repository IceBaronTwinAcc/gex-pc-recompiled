#!/usr/bin/env bash
set -euo pipefail
: "${GEX_BGFX_SHADERC:?Set GEX_BGFX_SHADERC to the host-native bgfx shader compiler}"
prefix="$(mktemp -d "${TMPDIR:-/tmp}/gex-bgfx-shader.XXXXXX")"
export WINEPREFIX="$prefix" WINEDEBUG=-all
export WINEDLLOVERRIDES='d3dcompiler_47=n;winemenubuilder.exe,mscoree,mshtml,winepulse.drv,winealsa.drv,wineoss.drv=d'
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY
cleanup() {
    local status=$?
    if timeout 30s wineserver -w; then
        rm -rf -- "$prefix"
    else
        printf 'Shader compiler Wine is still running; preserving %s\n' "$prefix" >&2
        status=1
    fi
    exit "$status"
}
trap cleanup EXIT
if ! {
    timeout 15s wineserver -p5 &&
    timeout 60s wineboot --init &&
    timeout 20s wine reg add 'HKCU\Software\Wine\Drivers' /v Audio /d '' /f
} > "$prefix/bootstrap.log" 2>&1; then
    cat "$prefix/bootstrap.log" >&2
    exit 1
fi
timeout 60s "$GEX_BGFX_SHADERC" "$@"
