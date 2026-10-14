#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

# This test edits only a temporary project created by --smoke-test.
export GTK_A11Y=none
export GSETTINGS_BACKEND=memory
export GSK_RENDERER=cairo

if [ -n "${DISPLAY:-}" ]; then
    timeout 30s dbus-run-session -- ./build/startapp --smoke-test
else
    command -v xvfb-run >/dev/null 2>&1 || {
        printf '%s\n' 'Install xvfb and xauth to run the GUI test without a display.' >&2
        exit 1
    }
    export GDK_BACKEND=x11
    timeout 30s xvfb-run -a -s '-screen 0 1600x1000x24' \
        dbus-run-session -- ./build/startapp --smoke-test
fi
