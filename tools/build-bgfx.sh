#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
revision=1af3832b87ad0588c05f1f0bd7ee17a7b14d2315
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
if [[ ! -d .deps/bgfx.cmake ]]; then
    git clone https://github.com/bkaradzic/bgfx.cmake.git .deps/bgfx.cmake
    git -C .deps/bgfx.cmake checkout --detach "$revision"
fi
if [[ "$(git -C .deps/bgfx.cmake rev-parse HEAD)" != "$revision" ]]; then
    printf 'Expected bgfx.cmake revision %s; existing checkout was not modified.\n' "$revision" >&2
    exit 1
fi
cmake -S .deps/bgfx.cmake -B .deps/bgfx-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBGFX_CMAKE_USER_SCRIPT="$root/cmake/bgfx-host.cmake" \
    -DBGFX_BUILD_EXAMPLES=OFF -DBGFX_BUILD_EXAMPLE_COMMON=OFF \
    -DBGFX_BUILD_TOOLS_GEOMETRY=OFF -DBGFX_BUILD_TOOLS_TEXTURE=OFF \
    -DBGFX_WITH_WAYLAND=OFF -DBGFX_CONFIG_VIDEO=OFF -DBGFX_INSTALL=OFF
cmake --build .deps/bgfx-build --parallel "$jobs" --target shaderc
cmake -S . -B "${1:-build/linux/bgfx}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DGEX_ENABLE_BGFX_RENDERER=ON \
    -DGEX_BGFX_SHADERC="$root/.deps/bgfx-build/bin/host/shaderc" \
    -DBX_DIR="$root/.deps/bgfx-build/_deps/bx-src" \
    -DBIMG_DIR="$root/.deps/bgfx-build/_deps/bimg-src" \
    -DBGFX_DIR="$root/.deps/bgfx-build/_deps/bgfx-src" \
    "${@:2}"
cmake --build "${1:-build/linux/bgfx}" --parallel "$jobs"
