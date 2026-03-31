#!/bin/sh -eu

self=$(readlink -f "$0")
self_dir=$(dirname "$self")
base_dir=$(dirname "$self_dir")

cd "$base_dir"

if [ ! -f "configure.py" ]; then
    echo "$0 must be executed from the project root directory"
    exit 1
fi

nopegl_version=$(cat VERSION)
bindings_version=$(cat android/VERSION)
tag="v${nopegl_version}-${bindings_version}-android"

release="${nopegl_version}-${bindings_version}-android"

git commit --allow-empty -m "Release $release"
git tag -a "$tag" -m "$tag"

echo "Tag $tag created. You can now push it with:"
echo "  git push origin $tag"
