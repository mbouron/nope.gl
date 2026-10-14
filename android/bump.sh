#!/bin/sh -e
#
# Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

self=$(readlink -f "$0")
self_dir=$(dirname "$self")
base_dir=$(dirname "$self_dir")
version_file="$base_dir/android/VERSION"

cd "$base_dir"

if [ ! -f "configure.py" ]; then
    echo "$0 must be executed from the project root directory"
    exit 1
fi

opt="minor"
if [ $# -gt 0 ]; then
    opt=$1
fi
field=""
case $opt in
    "patch")
        field="patch"
    ;;
    "minor")
        field="minor"
    ;;
    "major")
        field="major"
    ;;
    *)
        echo "usage: ./bump.sh [major|minor|patch]"
        exit 1
    ;;
esac

current_version=$(cat "$version_file")

# Split the version into its components
IFS='.' read -r major minor patch <<EOF
$current_version
EOF

# Increment the specified version field
case $field in
    "patch")
        patch=$((patch + 1))
    ;;
    "minor")
        minor=$((minor + 1))
        patch=0
    ;;
    "major")
        major=$((major + 1))
        minor=0
        patch=0
    ;;
esac

# Create the new version string
new_version="$major.$minor.$patch"

# Write the new version back to the VERSION file
echo "$new_version" > "$version_file"

# Print the new version
commit_message="android: bump bindings version to $new_version"
echo "$commit_message"

branch_name="android-bump-$new_version"

git checkout -b "$branch_name"

git add "$version_file"

git commit -m "$commit_message"
