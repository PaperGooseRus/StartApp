#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/linux/.cmake/api/v1/query
touch build/linux/.cmake/api/v1/query/codemodel-v2
touch build/linux/.cmake/api/v1/query/toolchains-v1
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/linux --parallel 2
ctest --test-dir build/linux -R '^(core|tools|workflows|build-run|cmake-build|metadata-self)$' --output-on-failure
cmake --build build/linux --target package
printf '%s\n' 'Built build/linux/startapp and a Linux archive in build/linux/.'
