#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
ndk="${ANDROID_NDK_HOME:-$sdk/ndk/27.2.12479018}"
abi="${GEX_ANDROID_ABI:-arm64-v8a}"
case "$abi" in
    arm64-v8a) triple=aarch64-linux-android; ffmpeg_arch=aarch64 ;;
    x86_64) triple=x86_64-linux-android; ffmpeg_arch=x86_64 ;;
    *) printf 'GEX_ANDROID_ABI must be arm64-v8a or x86_64 (64-bit guest address space required).\n' >&2; exit 1 ;;
esac
toolchain="$ndk/build/cmake/android.toolchain.cmake"
llvm="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [[ ! -f "$toolchain" || ! -x "$llvm/$triple""26-clang" ]]; then
    printf 'Android NDK 27+ is required; set ANDROID_HOME and/or ANDROID_NDK_HOME.\n' >&2
    exit 1
fi
prefix="$root/.deps/android-$abi"
sources="$root/.deps/android-sources"
downloads="$root/.deps/android-downloads"
builds="$root/.deps/android-build/$abi"
build="${1:-build/android/$abi}"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
mkdir -p "$prefix" "$sources" "$downloads" "$builds"

fetch() {
    local url="$1" name="$2" hash="$3"
    if [[ ! -f "$downloads/$name" ]]; then
        if [[ -f "$root/.deps/macos-downloads/$name" ]]; then
            cp "$root/.deps/macos-downloads/$name" "$downloads/$name.part"
        else
            curl --fail --location --retry 2 "$url" -o "$downloads/$name.part"
        fi
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
    "-DANDROID_ABI=$abi" -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "-DCMAKE_INSTALL_PREFIX=$prefix"
    "-DCMAKE_PREFIX_PATH=$prefix" "-DCMAKE_FIND_ROOT_PATH=$prefix")
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
    -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF -DALSOFT_BACKEND_OBOE=OFF \
    -DALSOFT_REQUIRE_OPENSL=ON
cmake --build "$builds/openal" --parallel "$jobs"
cmake --install "$builds/openal"
if [[ ! -f "$prefix/.ffmpeg-8.0.1-shared-installed" ]]; then
    mkdir -p "$builds/ffmpeg"
    (
        cd "$builds/ffmpeg"
        "$sources/ffmpeg-8.0.1/configure" --prefix="$prefix" --target-os=android --arch="$ffmpeg_arch" \
            --cc="$llvm/$triple""26-clang" --cxx="$llvm/$triple""26-clang++" \
            --ar="$llvm/llvm-ar" --ranlib="$llvm/llvm-ranlib" --nm="$llvm/llvm-nm" \
            --strip="$llvm/llvm-strip" --enable-cross-compile --enable-pic \
            --enable-shared --disable-static --disable-everything \
            --extra-ldflags="-Wl,-z,max-page-size=16384" \
            --enable-decoder=indeo3,pcm_u8 --enable-demuxer=avi --enable-protocol=file \
            --enable-swscale --disable-autodetect --disable-network --disable-programs \
            --disable-doc --disable-debug --disable-x86asm --disable-avdevice \
            --disable-avfilter --disable-swresample
        make -j"$jobs"
        make install
    )
    touch "$prefix/.ffmpeg-8.0.1-shared-installed"
fi
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/share/pkgconfig"
tools/build-bgfx.sh "$build" "${common[@]}" \
    -DGEX_BGFX_GL_FLAVOUR=es -DGEX_ENABLE_OPENGL_RENDERER=OFF \
    "-DOPENAL_LIBRARY=$prefix/lib/libopenal.so" "-DOPENAL_INCLUDE_DIR=$prefix/include/AL" \
    "${@:2}"
mkdir -p "$build/lib/$abi" "$build/licenses"
cp -L "$prefix"/lib/*.so "$build/lib/$abi/"
cp "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$triple/libc++_shared.so" "$build/lib/$abi/"
cp "$build/libgex_recomp.so" "$build/lib/$abi/"
"$llvm/llvm-strip" --strip-debug "$build/lib/$abi/"*.so
cp "$sources/SDL2-2.32.10/LICENSE.txt" "$build/licenses/SDL2.txt"
cp "$sources/SDL2_ttf-2.24.0/LICENSE.txt" "$build/licenses/SDL2_ttf.txt"
cp "$sources/SDL2_ttf-2.24.0/external/freetype/docs/FTL.TXT" "$build/licenses/FreeType.txt"
cp "$sources/openal-soft-1.24.3/COPYING" "$build/licenses/OpenAL-Soft.txt"
cp "$sources/ffmpeg-8.0.1/COPYING.LGPLv2.1" "$build/licenses/FFmpeg.txt"
cp "$ndk/toolchains/llvm/prebuilt/linux-x86_64/NOTICE" "$build/licenses/LLVM.txt"
python3 tools/package-android.py --sdk "$sdk" --sdl "$sources/SDL2-2.32.10" "$build"
