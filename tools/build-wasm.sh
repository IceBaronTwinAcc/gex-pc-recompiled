#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

build_dir="${1:-build/wasm}"
shift || true
if ! command -v emcmake >/dev/null 2>&1 || ! command -v emcc >/dev/null 2>&1; then
    printf 'Emscripten is required. Activate its SDK so emcmake and emcc are on PATH.\n' >&2
    exit 1
fi

args=(
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    -DGEX_PLATFORM_FILE="$PWD/cmake/platforms/wasm.cmake"
    -DGEX_RENDERER_BACKEND_SOURCE="$PWD/src/platform/desktop/renderer.c"
    -DGEX_ENABLE_SOFTWARE_RENDERER=ON
    -DGEX_ENABLE_OPENGL_RENDERER=OFF
    -DGEX_ENABLE_PVR_RENDERER=OFF
    -DGEX_ENABLE_GLDC_RENDERER=OFF
    -DGEX_ENABLE_BGFX_RENDERER=OFF
)
if command -v ninja >/dev/null 2>&1; then
    args=(-G Ninja "${args[@]}")
fi

emcmake cmake -S . -B "$build_dir" "${args[@]}" "$@"
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
