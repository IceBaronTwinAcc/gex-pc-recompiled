#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
prefix="$root/.deps/mingw64"
sources="$root/.deps/mingw-sources"
downloads="$root/.deps/mingw-downloads"
build="${1:-build/windows/mingw}"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
mkdir -p "$prefix" "$sources" "$downloads"

fetch() {
    local url="$1" name="$2" hash="$3"
    if [[ ! -f "$downloads/$name" ]]; then
        curl --fail --location --retry 2 "$url" -o "$downloads/$name"
    fi
    printf '%s  %s\n' "$hash" "$downloads/$name" | sha256sum --check --status
}

fetch https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz \
    SDL2-devel-2.32.10-mingw.tar.gz 83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2
fetch https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-devel-2.24.0-mingw.tar.gz \
    SDL2_ttf-devel-2.24.0-mingw.tar.gz 3a09e0a967ad53eca3ff2701de4d0df3369c0368739a1088a78d9d74908ef2c3
fetch https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz \
    ffmpeg-8.0.1.tar.xz 05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
for package in SDL2-2.32.10 SDL2_ttf-2.24.0; do
    if [[ ! -d "$sources/$package" ]]; then
        tar -xf "$downloads/${package/-/-devel-}-mingw.tar.gz" -C "$sources"
    fi
    cp -a "$sources/$package/x86_64-w64-mingw32/." "$prefix/"
done
if [[ ! -f "$prefix/lib/pkgconfig/libavcodec.pc" ]]; then
    if [[ ! -d "$sources/ffmpeg-8.0.1" ]]; then
        tar -xf "$downloads/ffmpeg-8.0.1.tar.xz" -C "$sources"
    fi
    mkdir -p "$sources/ffmpeg-build"
    (
        cd "$sources/ffmpeg-build"
        ../ffmpeg-8.0.1/configure --prefix="$prefix" --target-os=mingw32 --arch=x86_64 \
            --cross-prefix=x86_64-w64-mingw32- --enable-cross-compile \
            --enable-shared --disable-static --disable-everything \
            --enable-decoder=indeo3,pcm_u8 --enable-demuxer=avi --enable-protocol=file \
            --enable-swscale --disable-autodetect --disable-network --disable-programs \
            --disable-doc --disable-debug --disable-x86asm --disable-avdevice \
            --disable-avfilter --disable-swresample
        make -j"$jobs"
        make install
    )
fi
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH=
tools/build-bgfx.sh "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/mingw64.cmake" \
    -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
    -DPython3_EXECUTABLE="$root/.venv/bin/python"
cp "$prefix"/bin/*.dll "$build/"
pthread_dll="$(x86_64-w64-mingw32-gcc -print-file-name=libwinpthread-1.dll)"
if [[ ! -f "$pthread_dll" ]]; then
    pthread_library="$(x86_64-w64-mingw32-gcc -print-file-name=libwinpthread.dll.a)"
    pthread_dll="$(dirname "$pthread_library")/../bin/libwinpthread-1.dll"
fi
cp "$pthread_dll" "$build/"
mkdir -p "$build/licenses"
cp "$sources/SDL2-2.32.10/LICENSE.txt" "$build/licenses/SDL2.txt"
cp "$sources/SDL2_ttf-2.24.0/LICENSE.txt" "$build/licenses/SDL2_ttf.txt"
cp "$sources/ffmpeg-8.0.1/COPYING.LGPLv2.1" "$build/licenses/FFmpeg.txt"
printf 'Windows build: %s/gex_recomp.exe\n' "$build"
