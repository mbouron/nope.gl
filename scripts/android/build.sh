#!/bin/sh -xeu

glslang_version=11.9.0
nopemd_version=11.1.1
ffmpeg_version=6.1
ndk_path=$HOME/src/android-sdk/ndk/26.1.10909125
ndk_bin_path="$ndk_path/toolchains/llvm/prebuilt/linux-x86_64/bin"
ndk_cmake_toolchain="$ndk_path/build/cmake/android.toolchain.cmake"

arch="aarch64"

android_suffix="android"
compiler=$arch

if [ $arch = "arm" ]; then
    cpu_family="arm"
    android_abi="armeabi-v7a"
    android_suffix="androideabi"
    compiler="armv7a"
elif [ $arch = "aarch64" ]; then
    android_abi="arm64-v8a"
    cpu_family="arm"
elif [ $arch = "x86_64" ]; then
    android_abi="x86_64"
    cpu_family="x86"
fi

prefix="$HOME/src/ngl-android-env/$android_abi"

mkdir -p "$prefix"
cat >"$prefix/meson-android-$arch.ini" <<EOF
[constants]
ndk_path = '$ndk_path'
toolchain = ndk_path / 'toolchains/llvm/prebuilt/linux-x86_64'

[binaries]
c = toolchain / 'bin/aarch64-linux-android28-clang'
cpp = toolchain / 'bin/aarch64-linux-android28-clang++'
strip = toolchain / 'bin/llvm-strip'
pkgconfig = '/usr/bin/pkg-config'

[host_machine]
system = 'android'
cpu_family = '$cpu_family'
cpu = '$arch'
endian = 'little'
EOF

curl -sL https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz -o ffmpeg.tar.xz
tar -xf ffmpeg.tar.xz
cd ffmpeg-$ffmpeg_version

./configure \
--disable-everything --disable-doc --disable-static --disable-autodetect --disable-programs \
--enable-shared --enable-cross-compile --enable-jni --enable-mediacodec --enable-hwaccels  \
--enable-avdevice --enable-swresample \
--enable-filter='aformat,aresample,asetnsamples,asettb,fillborders,copy,format,fps,hflip,palettegen,paletteuse,pad,settb,scale,transpose,vflip' \
--enable-bsf='aac_adtstoasc,extract_extradata,h264_mp4toannexb,hevc_mp4toannexb,mpeg4_unpack_bframes,vp9_superframe' \
--enable-encoder='gif,png,mjpeg' \
--enable-demuxer='aac,aiff,concat,gif,image_jpeg_pipe,image_pgm_pipe,image_png_pipe,rawvideo,mp3,mp4,mov,wav,mpegts,flac,ogg,asf,avi,image_webp_pipe,matroska' \
--enable-decoder='aac,alac,amrnb,gif,mjpeg,mp2,mp3,mpeg4,pcm_s16be,pcm_s16le,pcm_s24be,pcm_s24le,pgm,png,rawvideo,flac,vorbis,opus,wmav1,wmav2,wmav1lossless,h264_mediacodec,hevc_mediacodec,vp8,vp8_mediacodec,vp9_mediacodec,webp' \
--enable-parser='mjpeg,png,h264,mpeg4video,aac,flac,hevc,vp8,vp9' \
--enable-muxer='gif,image2,mp4,mov,ipod' \
--enable-protocol='file,http,https,pipe' \
--arch=$arch --target-os=android \
--cross-prefix="$ndk_bin_path/llvm-" \
--cc="$ndk_bin_path/${compiler}-linux-${android_suffix}28-clang" \
--prefix="$prefix"
make install -j$(($(nproc)+1))
cd ..

curl -sL https://github.com/NopeForge/nope.media/archive/refs/tags/v$nopemd_version.tar.gz -o nope.media.tgz
tar xf nope.media.tgz
cd nope.media-$nopemd_version
PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig/" \
meson setup --prefix "$prefix" -Ddefault_library=static -Dvaapi=disabled --cross-file "$prefix/meson-android-$arch.ini" builddir
meson compile -C builddir
meson install -C builddir
cd ..

if [ $arch = "arm" ]; then
    abi="armeabi-v7a"
elif [ $arch = "aarch64" ]; then
    abi="arm64-v8a"
elif [ $arch = "x86_64" ]; then
    abi="x86_64"
fi

curl -sL https://github.com/KhronosGroup/glslang/archive/refs/tags/$glslang_version.tar.gz -o glslang.tar.gz
tar xf glslang.tar.gz
cd glslang-$glslang_version
cmake \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk_cmake_toolchain" \
    -DANDROID_STL=c++_shared \
    -DANDROID_TOOLCHAIN=clang \
    -DANDROID_PLATFORM=android-28 \
    -DANDROID_ABI="$abi" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXTERNAL=OFF
cmake --build .
cmake --install .
cd ..

if ! [ -d nope.gl ]; then
    git clone https://github.com/mbouron/nope.gl.git
fi
cd nope.gl/libnopegl
git checkout blur
PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig/" \
meson setup builddir \
    --cross-file "$prefix/meson-android-$arch.ini" \
    --prefix="$prefix" \
    --buildtype release \
    -Ddefault_library=static \
    -Dextra_library_dirs="$prefix/lib" \
    -Dextra_include_dirs="$prefix/include"
meson compile -C builddir
meson install -C builddir
cd ../..
