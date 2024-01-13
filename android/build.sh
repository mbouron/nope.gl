#!/bin/sh -xeu

self=$(readlink -f "$0")
self_dir=$(dirname "$self")
base_dir=$(dirname "$self_dir")

opt="debug"
if [ $# -gt 0 ]; then
    opt=$1
fi

case $opt in
    "debug")
        buildtype="debug"
        graddle_rule="assembleDebug"
    ;;
    "release")
        buildtype="release"
        graddle_rule="assembleRelease"
        ;;
    *)
        echo "usage: ./build.sh [debug|release]"
        exit 1
    ;;
esac

cd "$base_dir"

if [ ! -f "configure.py" ]; then
    echo "$0 must be executed from the project root directory"
    exit 1
fi

archs="arm aarch64 x86_64"
for arch in $archs; do
    python configure.py --buildtype "$buildtype" --host Android --host-arch "$arch"
    make -f "Makefile.Android.$arch"
done

export NGL_ANDROID_ENV="$base_dir/Android/"
(
    cd android
    ./gradlew "$graddle_rule"
)

echo "AAR: $base_dir/android/nopegl/build/outputs/aar/nopegl-debug.aar"
