#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${KOS_BASE:?Source the KallistiOS environ.sh first}"
: "${KOS_CC_BASE:?Source the KallistiOS environ.sh first}"
root="$PWD"
prefix="$root/.deps/dreamcast-ffmpeg"
source="$root/.deps/dreamcast-ffmpeg-src"
build="$root/.deps/dreamcast-ffmpeg-build"
archive="$root/.deps/dreamcast-downloads/ffmpeg-8.0.1.tar.xz"
hash=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
if [[ -f "$prefix/.installed-8.0.1-3" ]]; then
    exit 0
fi
mkdir -p "$(dirname "$archive")" "$build"
if [[ ! -f "$archive" ]]; then
    if [[ -f "$root/.deps/android-downloads/ffmpeg-8.0.1.tar.xz" ]]; then
        cp "$root/.deps/android-downloads/ffmpeg-8.0.1.tar.xz" "$archive.part"
    else
        curl --fail --location --retry 2 https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz -o "$archive.part"
    fi
    printf '%s  %s\n' "$hash" "$archive.part" | sha256sum --check
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$hash" "$archive" | sha256sum --check
if [[ ! -d "$source" ]]; then
    mkdir -p "$source"
    tar -xf "$archive" -C "$source" --strip-components=1
fi
(
    cd "$build"
    "$source/configure" --prefix="$prefix" --target-os=none --arch=sh4 \
        --cc="$KOS_BASE/utils/build_wrappers/kos-cc" \
        --ld="$KOS_BASE/utils/build_wrappers/kos-cc" \
        --ar="$KOS_CC_BASE/bin/sh-elf-ar" --ranlib="$KOS_CC_BASE/bin/sh-elf-ranlib" \
        --nm="$KOS_CC_BASE/bin/sh-elf-nm" --strip="$KOS_CC_BASE/bin/sh-elf-strip" \
        --enable-cross-compile --enable-static --disable-shared --enable-small \
        --disable-everything --disable-autodetect --disable-network --disable-programs \
        --disable-doc --disable-debug --disable-asm --disable-inline-asm \
        --disable-pthreads --disable-w32threads --disable-os2threads \
        --disable-avdevice --disable-avfilter --disable-swresample --disable-swscale \
        --enable-decoder=indeo3,pcm_u8 --enable-demuxer=avi \
        --disable-runtime-cpudetect --optflags=-O2 \
        --extra-cflags="-fno-lto -fno-fast-math -ffp-contract=off -ffunction-sections -fdata-sections" \
        --extra-ldflags="-fno-lto"
    make -j"${JOBS:-3}"
    make install
)
mkdir -p "$prefix/licenses"
cp "$source/COPYING.LGPLv2.1" "$prefix/licenses/FFmpeg.txt"
touch "$prefix/.installed-8.0.1-3"
