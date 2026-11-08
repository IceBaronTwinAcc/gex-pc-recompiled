#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

build_dir="${1:-build/wasm}"
shift || true
if ! command -v emcmake >/dev/null 2>&1 || ! command -v emcc >/dev/null 2>&1; then
    printf 'Emscripten is required. Activate its SDK so emcmake and emcc are on PATH.\n' >&2
    exit 1
fi

root="$PWD"
prefix="$root/.deps/wasm-ffmpeg"
source="$root/.deps/wasm-sources/ffmpeg-8.0.1"
build="$root/.deps/wasm-build/ffmpeg"
downloads="$root/.deps/wasm-downloads"
archive="$downloads/ffmpeg-8.0.1.tar.xz"
hash=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
mkdir -p "$downloads" "$build"
if [[ ! -f "$archive" ]]; then
    if [[ -f "$root/.deps/macos-downloads/ffmpeg-8.0.1.tar.xz" ]]; then
        cp "$root/.deps/macos-downloads/ffmpeg-8.0.1.tar.xz" "$archive.part"
    else
        curl --fail --location --retry 2 \
            https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz -o "$archive.part"
    fi
    printf '%s  %s\n' "$hash" "$archive.part" | sha256sum --check
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$hash" "$archive" | sha256sum --check
if [[ ! -d "$source" ]]; then
    mkdir -p "$source"
    tar -xf "$archive" -C "$source" --strip-components=1
fi
if [[ ! -f "$prefix/.installed-wasm-ffmpeg-8.0.1-1" ||
      ! -f "$prefix/lib/libavformat.a" || ! -f "$prefix/lib/libavcodec.a" ||
      ! -f "$prefix/lib/libswscale.a" || ! -f "$prefix/lib/libavutil.a" ||
      ! -f "$prefix/include/libavcodec/avcodec.h" ||
      ! -f "$prefix/licenses/FFmpeg.txt" ]]; then
    (
        cd "$build"
        emconfigure "$source/configure" --prefix="$prefix" --target-os=none --arch=wasm32 \
            --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=llvm-nm --strip=llvm-strip \
            --enable-cross-compile --enable-pic --enable-static --disable-shared \
            --disable-everything --disable-autodetect --disable-network \
            --disable-programs --disable-doc --disable-debug --disable-x86asm --disable-asm \
            --disable-avdevice --disable-avfilter --disable-swresample \
            --enable-decoder=indeo3,cinepak,pcm_u8 --enable-demuxer=avi --enable-protocol=file \
            --enable-swscale --extra-cflags=-pthread --extra-ldflags=-pthread
        emmake make -j"$jobs"
        emmake make install
    )
    mkdir -p "$prefix/licenses"
    cp "$source/COPYING.LGPLv2.1" "$prefix/licenses/FFmpeg.txt"
    touch "$prefix/.installed-wasm-ffmpeg-8.0.1-1"
fi

args=(
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    -DGEX_PLATFORM_FILE="$PWD/cmake/platforms/wasm.cmake"
    -DGEX_WASM_FFMPEG_ROOT="$prefix"
    -DGEX_MOVIE_DECODER_SOURCE="$PWD/src/platform/desktop/movie_decoder.c"
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
