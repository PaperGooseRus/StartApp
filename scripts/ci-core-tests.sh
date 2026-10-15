#!/usr/bin/env bash
set -uo pipefail
directory=${1:?Pass the CMake build directory}
status=0
ctest --test-dir "$directory" -R '^(core|tools|workflows|build-run|cmake-build|metadata-self)$' --output-on-failure || status=$?
if (( status != 0 )); then
    # Tests operate on owned fixtures. Keep their failures visible in public annotations.
    # GitHub retains only ten annotations and truncates messages around 4 KiB.
    while IFS= read -r -d $'\036' message; do
        if (( ${#message} > 3400 )); then message="${message:0:180}"$'\n...\n'"${message: -3100}"; fi
        message=${message//'%'/'%25'}
        message=${message//$'\r'/'%0D'}
        message=${message//$'\n'/'%0A'}
        printf '::error title=Native test diagnostics::%s\n' "$message"
    done < <(awk '
        /^[0-9]+\/[0-9]+ Testing:/ { block = "" }
        /GLib-.*DEBUG:/ { next }
        { block = block $0 "\n" }
        /Test Failed\./ { printf "%s%c", block, 30 }
    ' "$directory/Testing/Temporary/LastTest.log")
fi
exit "$status"
