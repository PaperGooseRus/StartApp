#!/usr/bin/env bash
set -uo pipefail
directory=${1:?Pass the CMake build directory}
status=0
ctest --test-dir "$directory" -R '^(core|tools|workflows|build-run|cmake-build|metadata-self)$' --output-on-failure || status=$?
if (( status != 0 )); then
    # Tests operate on owned fixtures. Keep their failures visible in public annotations.
    while IFS= read -r line; do
        line=${line//'%'/'%25'}
        line=${line//$'\r'/'%0D'}
        printf '::error title=Native test diagnostics::%s\n' "$line"
    done < <(tail -180 "$directory/Testing/Temporary/LastTest.log")
fi
exit "$status"
