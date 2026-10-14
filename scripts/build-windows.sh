#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${MSYSTEM:-} != UCRT64 ]]; then
    printf '%s\n' 'Open the MSYS2 UCRT64 terminal, then run: bash scripts/build-windows.sh' >&2
    exit 1
fi
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows --parallel 2
ctest --test-dir build/windows -R '^core$' --output-on-failure
bash scripts/package-windows.sh
