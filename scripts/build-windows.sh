#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${MSYSTEM:-} != UCRT64 ]]; then
    printf '%s\n' 'Open the MSYS2 UCRT64 terminal, then run: bash scripts/build-windows.sh' >&2
    exit 1
fi
mkdir -p build/windows/.cmake/api/v1/query
touch build/windows/.cmake/api/v1/query/codemodel-v2
touch build/windows/.cmake/api/v1/query/toolchains-v1
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/windows --parallel 2
ctest --test-dir build/windows -R '^(core|metadata-self)$' --output-on-failure
bash scripts/package-windows.sh
