#!/bin/sh -xeu

self=$(readlink -f "$0")
self_dir=$(dirname "$self")
base_dir=$(dirname "$self_dir")

cd "$base_dir"

if [ ! -f "configure.py" ]; then
    echo "$0 must be executed from the project root directory"
    exit 1
fi

local="false"
if [ "$#" -lt 1 ]; then
    local="false"
elif [ "$1" = "local" ]; then
    local="true"
fi

archs="arm aarch64 x86_64"
for arch in $archs; do
    python configure.py --buildtype "debug" --build-id --host Android --host-arch "$arch"
    make -f "Makefile.Android.$arch"
done

export NGL_ANDROID_ENV="$base_dir/Android/"
(
    cd android
    if [ "$local" = "true" ]; then
        ./gradlew publishToMavenLocal
    else
      ./gradlew publish
    fi
)
