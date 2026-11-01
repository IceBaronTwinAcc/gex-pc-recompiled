#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${KOS_BASE:-}" ]]; then
    printf 'Source the KallistiOS environ.sh first.\n' >&2
    exit 1
fi
build=build/dreamcast
cmake -S . -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/dreamcast.cmake" \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-Os -g" \
    -DPython3_EXECUTABLE="$PWD/.venv/bin/python" -DBUILD_TESTING=OFF
cmake --build "$build" --parallel "${JOBS:-3}"
"${KOS_OBJCOPY}" -O binary "$build/gex_recomp.elf" "$build/gex_recomp.bin"
"${KOS_CC_BASE}/bin/sh-elf-size" "$build/gex_recomp.elf"
