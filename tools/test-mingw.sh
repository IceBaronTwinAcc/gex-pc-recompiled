#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build="${1:-build/windows/mingw}"
build="$(realpath "$build")"
prefix="$(mktemp -d /tmp/gex-wine-tests.XXXXXX)"
export WINEPREFIX="$prefix" WINEDEBUG=-all
export WINEDLLOVERRIDES='winemenubuilder.exe,mscoree,mshtml,winepulse.drv,winealsa.drv,wineoss.drv=d'
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XDG_DATA_HOME="$prefix/settings"
unset DISPLAY WAYLAND_DISPLAY
mkdir -p "$XDG_DATA_HOME"
cleanup() {
    if timeout 20s wineserver -w; then
        # This prefix was created by this invocation, never the user's Wine prefix.
        rm -rf -- "$prefix"
    else
        printf 'Wine is still running; leaving isolated prefix at %s\n' "$prefix" >&2
    fi
}
trap cleanup EXIT
timeout 10s wineserver -p5
timeout 60s wineboot --init
timeout 15s wine reg add 'HKCU\Software\Wine\Drivers' /v Audio /d '' /f
# Wine versions differ in whether they import Unix SDL variables.
timeout 15s wine reg add 'HKCU\Environment' /v SDL_VIDEODRIVER /d dummy /f
timeout 15s wine reg add 'HKCU\Environment' /v SDL_AUDIODRIVER /d dummy /f
# GPU tests require a private GL-capable display; this baseline has none.
timeout 240s ctest --test-dir "$build" --output-on-failure --parallel 3 -LE gpu
timeout 35s wine "$build/gex_recomp.exe" --headless --skip-movies --autoplay \
    --verify-gameplay --render-size 427x240 --smoke-seconds 24
timeout 110s wine "$build/gex_recomp.exe" --headless --smoke-seconds 95 2>&1 | tee "$prefix/movies.log"
[[ "$(grep -c 'playback completed' "$prefix/movies.log")" == 2 ]]
timeout 45s wine "$build/gex_recomp.exe" --headless --skip-movies --autoplay \
    --smoke-seconds 40 2>&1 | tee "$prefix/exit.log"
grep -q 'ExitProcess(0)' "$prefix/exit.log"
