#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
arch="${GEX_MACOS_ARCH:-arm64}"
case "$arch" in
    arm64) ffmpeg_arch=aarch64 ;;
    x86_64) ffmpeg_arch=x86_64 ;;
    *) printf 'GEX_MACOS_ARCH must be arm64 or x86_64.\n' >&2; exit 1 ;;
esac
prefix="$root/.deps/macos-$arch"
sources="$root/.deps/macos-sources"
builds="$root/.deps/macos-build/$arch"
downloads="$root/.deps/macos-downloads"
toolchain="$root/cmake/toolchains/macos.cmake"
build="${1:-build/macos/$arch-bgfx}"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
export PATH="$root/.deps/osxcross/target/bin:$PATH"
mkdir -p "$prefix" "$sources" "$downloads" "$builds"
compilers=()
for candidate in "$root"/.deps/osxcross/target/bin/"$arch"-apple-darwin*-clang; do
    [[ "$candidate" == *-cmake-clang ]] || compilers+=("$candidate")
done
if [[ ${#compilers[@]} != 1 || ! -x "${compilers[0]}" ]]; then
    printf 'A configured %s OSXCross toolchain is required in .deps/osxcross.\n' "$arch" >&2
    exit 1
fi
compiler="${compilers[0]}"
cross="${compiler%-clang}"

fetch() {
    local url="$1" name="$2" hash="$3"
    if [[ ! -f "$downloads/$name" ]]; then
        curl --fail --location --retry 2 "$url" -o "$downloads/$name.part"
        printf '%s  %s\n' "$hash" "$downloads/$name.part" | sha256sum --check
        mv "$downloads/$name.part" "$downloads/$name"
    fi
    printf '%s  %s\n' "$hash" "$downloads/$name" | sha256sum --check
}
fetch https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz \
    SDL2-2.32.10.tar.gz 5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
fetch https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.24.0/SDL2_ttf-2.24.0.tar.gz \
    SDL2_ttf-2.24.0.tar.gz 0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd
fetch https://github.com/kcat/openal-soft/releases/download/1.24.3/openal-soft-1.24.3.tar.bz2 \
    openal-soft-1.24.3.tar.bz2 cb5e6197a1c0da0edcf2a81024953cc8fa8545c3b9474e48c852af709d587892
fetch https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz \
    ffmpeg-8.0.1.tar.xz 05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
for package in SDL2-2.32.10 SDL2_ttf-2.24.0 openal-soft-1.24.3 ffmpeg-8.0.1; do
    if [[ ! -d "$sources/$package" ]]; then
        case "$package" in
            openal-*) extension=tar.bz2 ;;
            ffmpeg-*) extension=tar.xz ;;
            *) extension=tar.gz ;;
        esac
        tar -xf "$downloads/$package.$extension" -C "$sources"
    fi
done
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    "-DGEX_MACOS_ARCH=$arch"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_PREFIX_PATH=$prefix"
    "-DCMAKE_C_FLAGS=-resource-dir=$root/.deps/osxcross/target/lib/clang/runtime"
    "-DCMAKE_OBJC_FLAGS=-resource-dir=$root/.deps/osxcross/target/lib/clang/runtime"
    -DCMAKE_INSTALL_NAME_DIR=@rpath -DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON)
cmake -S "$sources/SDL2-2.32.10" -B "$builds/sdl" "${common[@]}" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$builds/sdl" --parallel "$jobs"
cmake --install "$builds/sdl"
cmake -S "$sources/SDL2_ttf-2.24.0" -B "$builds/ttf" "${common[@]}" \
    -DSDL2TTF_VENDORED=ON -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_HARFBUZZ=OFF \
    -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_BROTLI=ON
cmake --build "$builds/ttf" --parallel "$jobs"
cmake --install "$builds/ttf"
cmake -S "$sources/openal-soft-1.24.3" -B "$builds/openal" "${common[@]}" \
    -DALSOFT_EXAMPLES=OFF -DALSOFT_UTILS=OFF -DALSOFT_TESTS=OFF \
    -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF \
    -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF -DALSOFT_REQUIRE_COREAUDIO=ON
cmake --build "$builds/openal" --parallel "$jobs"
cmake --install "$builds/openal"
if [[ ! -f "$prefix/.ffmpeg-8.0.1-installed" ]]; then
    mkdir -p "$builds/ffmpeg"
    (
        cd "$builds/ffmpeg"
        "$sources/ffmpeg-8.0.1/configure" --prefix="$prefix" --target-os=darwin --arch="$ffmpeg_arch" \
            --cc="$compiler" --cxx="${cross}-clang++" --ar="${cross}-ar" \
            --ranlib="${cross}-ranlib" --nm="${cross}-nm" --strip="${cross}-strip" \
            --enable-cross-compile \
            "--extra-cflags=-mmacosx-version-min=13.0 -resource-dir=$root/.deps/osxcross/target/lib/clang/runtime" \
            "--extra-ldflags=-mmacosx-version-min=13.0 -resource-dir=$root/.deps/osxcross/target/lib/clang/runtime" \
            --install-name-dir=@rpath --enable-shared --disable-static --disable-everything \
            --enable-decoder=indeo3,pcm_u8 --enable-demuxer=avi --enable-protocol=file \
            --enable-swscale --disable-autodetect --disable-network --disable-programs \
            --disable-doc --disable-debug --disable-x86asm --disable-avdevice --disable-avfilter \
            --disable-swresample
        make -j"$jobs"
        make install
    )
    touch "$prefix/.ffmpeg-8.0.1-installed"
fi
tools/build-bgfx.sh "$build" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" \
    "-DGEX_MACOS_ARCH=$arch" \
    "-DCMAKE_PREFIX_PATH=$prefix" -DGEX_ENABLE_OPENGL_RENDERER=OFF \
    "-DOPENAL_LIBRARY=$prefix/lib/libopenal.dylib" "-DOPENAL_INCLUDE_DIR=$prefix/include/AL" \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=@executable_path/../Frameworks;@executable_path/lib' \
    "${@:2}"
mkdir -p "$build/lib" "$build/licenses"
cp -L "$prefix"/lib/*.dylib "$build/lib/"
cp "$sources/SDL2-2.32.10/LICENSE.txt" "$build/licenses/SDL2.txt"
cp "$sources/SDL2_ttf-2.24.0/LICENSE.txt" "$build/licenses/SDL2_ttf.txt"
cp "$sources/SDL2_ttf-2.24.0/external/freetype/docs/FTL.TXT" "$build/licenses/FreeType.txt"
cp "$sources/openal-soft-1.24.3/COPYING" "$build/licenses/OpenAL-Soft.txt"
cp "$sources/ffmpeg-8.0.1/COPYING.LGPLv2.1" "$build/licenses/FFmpeg.txt"
python3 tools/package-macos.py --prefix "$prefix" "$build"
