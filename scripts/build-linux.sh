#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel 2
ctest --test-dir build/linux -R '^core$' --output-on-failure
cmake --build build/linux --target package
printf '%s\n' 'Built build/linux/startapp and a Linux archive in build/linux/.'
