#!/usr/bin/env bash
set -uo pipefail
directory=${1:?Pass the CMake build directory}
status=0
ctest --test-dir "$directory" -R '^gui$' --output-on-failure || status=$?
if (( status != 0 )); then
    # Publish fixture-only diagnostics as annotations as well as private job logs.
    # Preserve the test's exit status; this never converts a failure to success.
    while IFS= read -r line; do
        if [[ $line == *'UI smoke FAIL'* || $line == file=* || $line == selection=* ]]; then
            line=${line//'%'/'%25'}
            line=${line//$'\r'/'%0D'}
            printf '::error title=GUI smoke test::%s\n' "$line"
        fi
    done < "$directory/Testing/Temporary/LastTest.log"
fi
exit "$status"
