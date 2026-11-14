#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${KOS_BASE:-}" ]]; then
    printf 'Source the KallistiOS environ.sh first.\n' >&2
    exit 1
fi
build=build/dreamcast
decoder="${GEX_DREAMCAST_MOVIE_DECODER:-$PWD/src/platform/dreamcast/movie_mpeg.c}"
if [[ "$decoder" == "$PWD/src/platform/dreamcast/movie_mpeg.c" ]]; then
    bash tools/prepare-dreamcast-mpeg.sh
elif [[ "$decoder" == "$PWD/src/platform/desktop/movie_decoder.c" ]]; then
    bash tools/build-dreamcast-ffmpeg.sh
fi
generator=()
if [[ ! -f "$build/CMakeCache.txt" ]]; then generator=(-G Ninja); fi
cmake -S . -B "$build" "${generator[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/dreamcast.cmake" \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-Os -g" \
    -DPython3_EXECUTABLE="$PWD/.venv/bin/python" -DBUILD_TESTING=OFF \
    -DGEX_MOVIE_DECODER_SOURCE="$decoder"
cmake --build "$build" --parallel "${JOBS:-3}"
"${KOS_OBJCOPY}" -O binary "$build/gex_recomp.elf" "$build/gex_recomp.bin"
"${KOS_CC_BASE}/bin/sh-elf-size" "$build/gex_recomp.elf"
mkdir -p "$build/licenses"
if [[ "$decoder" == "$PWD/src/platform/dreamcast/movie_mpeg.c" ]]; then
    cp .deps/dreamcast-pl-mpeg/LICENSE "$build/licenses/pl_mpeg.txt"
elif [[ "$decoder" == "$PWD/src/platform/desktop/movie_decoder.c" ]]; then
    cp .deps/dreamcast-ffmpeg/licenses/FFmpeg.txt "$build/licenses/FFmpeg.txt"
fi
