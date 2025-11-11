#!/bin/sh -xeu

self=$(readlink -f "$0")
self_dir=$(dirname "$self")
base_dir=$(dirname "$self_dir")

build_opt="debug"
publish_opt=""

while [ $# -gt 0 ]; do
    case $1 in
        --buildtype=*)
            build_opt="${1#--buildtype=}"
            ;;
        --publish=*)
            publish_opt="${1#--publish=}"
            ;;
        *)
            echo "usage: ./build.sh [--buildtype=debug|release|release-with-symbols] [--publish=local|remote]"
            exit 1
            ;;
    esac
    shift
done

debug_opts=""

case $build_opt in
    "debug")
        buildtype="debug"
        debug_opts="gl vk mem scene"
        gradle_rule="assembleDebug"
    ;;
    "release")
        buildtype="release"
        gradle_rule="assembleRelease"
    ;;
    "release-with-symbols")
        buildtype="debug"
        gradle_rule="assembleRelease"
    ;;
    *)
        echo "usage: ./build.sh [--buildtype=debug|release|release-with-symbols] [--publish=local|remote]"
        exit 1
    ;;
esac

case $publish_opt in
    "")
        # No publishing
        ;;
    "local")
        gradle_rule="$gradle_rule publishToMavenLocal"
        ;;
    "remote")
        gradle_rule="$gradle_rule publish"
        ;;
    *)
        echo "usage: ./build.sh [--buildtype=debug|release|release-with-symbols] [--publish=local|remote]"
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
    python configure.py --buildtype "$buildtype" -d $debug_opts --build-id --host Android --host-arch "$arch"
    make -f "Makefile.Android.$arch"
done

export NGL_ANDROID_ENV="$base_dir/build-android/"
(
    cd android
    ./gradlew $gradle_rule
)

echo "AAR: $base_dir/android/nopegl/build/outputs/aar/nopegl-$buildtype.aar"
