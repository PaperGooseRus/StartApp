#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ ${MSYSTEM:-} == UCRT64 ]] || { printf '%s\n' 'Packaging requires MSYS2 UCRT64.' >&2; exit 1; }
prefix=${MINGW_PREFIX:?}
package=dist/StartApp-Windows-x64
[[ ! -e $package ]] || { printf '%s\n' "Move or remove the previous $package before packaging." >&2; exit 1; }
[[ -f build/windows/startapp.exe && -f build/windows/startapp-cli.exe ]] || {
    printf '%s\n' 'Build the Windows executables first.' >&2; exit 1;
}
mkdir -p "$package"
cp build/windows/startapp.exe build/windows/startapp-cli.exe "$package/"
if [[ -d $prefix/lib/gdk-pixbuf-2.0 ]]; then
    mkdir -p "$package/lib"
    cp -R "$prefix/lib/gdk-pixbuf-2.0" "$package/lib/"
fi

# Resolve the complete PE import closure, including GTK's transitive DLLs.
# All application DLLs come from this UCRT64 prefix, never another toolchain.
queue=("$package/startapp.exe" "$package/startapp-cli.exe")
shopt -s nullglob
for module in "$package"/lib/gdk-pixbuf-2.0/*/loaders/*.dll; do queue+=("$module"); done
declare -A copied
system=$(cygpath -u "${WINDIR:?}")/System32
for ((index=0; index<${#queue[@]}; index++)); do
    imports=$("$prefix/bin/objdump.exe" -p "${queue[index]}")
    while IFS= read -r dll; do
        [[ -n $dll ]] || continue
        key=${dll,,}
        [[ ${copied[$key]:-} ]] && continue
        copied[$key]=1
        if [[ -f $prefix/bin/$dll ]]; then
            cp "$prefix/bin/$dll" "$package/"
            queue+=("$package/$dll")
        elif [[ $key == api-ms-win-* || $key == ext-ms-win-* || -f $system/$dll ]]; then
            continue
        else
            printf '%s\n' "Missing runtime DLL: $dll (imported by ${queue[index]})" >&2
            exit 1
        fi
    done < <(printf '%s\n' "$imports" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
done

mkdir -p "$package/share/glib-2.0" "$package/share/icons" "$package/etc"
cp -R "$prefix/share/glib-2.0/schemas" "$package/share/glib-2.0/"
glib-compile-schemas "$package/share/glib-2.0/schemas"
for theme in Adwaita hicolor; do
    [[ ! -d $prefix/share/icons/$theme ]] || cp -R "$prefix/share/icons/$theme" "$package/share/icons/"
done
if [[ -d $prefix/etc/fonts ]]; then cp -R "$prefix/etc/fonts" "$package/etc/"; fi
if [[ -d $prefix/share/fontconfig ]]; then cp -R "$prefix/share/fontconfig" "$package/share/"; fi
# Windows GdkPixbuf resolves relative cache paths against its DLL prefix.
# Generate with a relative module directory to keep the whole folder relocatable.
for directory in "$package"/lib/gdk-pixbuf-2.0/*/loaders; do
    relative=${directory#"$package/"}
    (cd "$package"; MSYS2_ENV_CONV_EXCL=GDK_PIXBUF_MODULEDIR GDK_PIXBUF_MODULEDIR="$relative" \
        "$prefix/bin/gdk-pixbuf-query-loaders.exe" > "${relative%/loaders}/loaders.cache")
done
cp README.md "$package/README.md"
cp docs/PLATFORMS.md "$package/PLATFORMS.md"
mkdir -p "$package/licenses"
for license in "$prefix"/share/licenses/*; do
    [[ ! -d $license ]] || cp -R "$license" "$package/licenses/"
done
printf '%s\n' 'StartApp Windows x64 portable package' \
    'Unzip the entire folder, then double-click startapp.exe.' \
    'For command-line JSON analysis, use startapp-cli.exe --inspect "C:\path\to\project".' \
    'Windows 10/11 x64; no MSYS2 installation is required to run this folder.' > "$package/START-HERE.txt"
(cd dist; cmake -E tar cf StartApp-Windows-x64.zip --format=zip -- StartApp-Windows-x64)
printf '%s\n' 'Created dist/StartApp-Windows-x64.zip'
