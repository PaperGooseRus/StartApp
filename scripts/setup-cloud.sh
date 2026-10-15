#!/bin/sh
# Rootless GTK4 setup for the Debian 13 cloud workspace.
# APT validates signed metadata and artifact checksums before extraction.
set -eu
. /etc/os-release
if [ "$ID" != debian ] || [ "$VERSION_ID" != 13 ]; then
    printf '%s\n' 'This helper supports Debian 13. See README.md for normal installation.' >&2
    exit 1
fi

STARTAPP_ENV_DIR=/workspace/.startapp-env
STARTAPP_MULTIARCH=$(cc -print-multiarch)
mkdir -p "$STARTAPP_ENV_DIR/apt/lists/partial" \
    "$STARTAPP_ENV_DIR/apt/archives/partial" "$STARTAPP_ENV_DIR/apt/empty" \
    "$STARTAPP_ENV_DIR/root" "$STARTAPP_ENV_DIR/cache"

cat > "$STARTAPP_ENV_DIR/apt/sources.list" <<'EOF'
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie-updates main
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://security.debian.org/debian-security trixie-security main
EOF
cat > "$STARTAPP_ENV_DIR/apt/apt.conf" <<EOF
Dir::Etc::main "/dev/null";
Dir::Etc::parts "$STARTAPP_ENV_DIR/apt/empty";
Dir::Etc::sourcelist "$STARTAPP_ENV_DIR/apt/sources.list";
Dir::Etc::sourceparts "$STARTAPP_ENV_DIR/apt/empty";
Dir::State::lists "$STARTAPP_ENV_DIR/apt/lists";
Dir::Cache::archives "$STARTAPP_ENV_DIR/apt/archives";
Dir::Cache::pkgcache "$STARTAPP_ENV_DIR/apt/pkgcache.bin";
Dir::Cache::srcpkgcache "$STARTAPP_ENV_DIR/apt/srcpkgcache.bin";
Dir::Log "$STARTAPP_ENV_DIR/apt";
APT::Sandbox::User "$(id -un)";
EOF

APT_CONFIG="$STARTAPP_ENV_DIR/apt/apt.conf" /usr/bin/apt-get update
# No dpkg installation occurs: only authenticated package downloads.
APT_CONFIG="$STARTAPP_ENV_DIR/apt/apt.conf" /usr/bin/apt-get \
    -o Debug::NoLocking=true --download-only --reinstall -y --no-install-recommends install \
    libgtk-4-dev libjson-glib-dev libjson-glib-1.0-0 libgtksourceview-5-dev libgtksourceview-5-0 libgtksourceview-5-common xvfb xauth dbus-daemon cmake ninja-build clangd gdb meson \
    libpango-1.0-0 libpangocairo-1.0-0 libpangoft2-1.0-0 libharfbuzz0b \
    libgdk-pixbuf-2.0-0 libcairo2 libcairo-gobject2 libvulkan1 libglib2.0-0t64

for package in "$STARTAPP_ENV_DIR"/apt/archives/*.deb; do
    dpkg-deb -x "$package" "$STARTAPP_ENV_DIR/root"
done

cat > "$STARTAPP_ENV_DIR/activate.sh" <<EOF
export PKG_CONFIG_PATH=$STARTAPP_ENV_DIR/root/usr/lib/$STARTAPP_MULTIARCH/pkgconfig:$STARTAPP_ENV_DIR/root/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$STARTAPP_ENV_DIR/root
export LD_LIBRARY_PATH=$STARTAPP_ENV_DIR/root/usr/lib/$STARTAPP_MULTIARCH\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}
export GSETTINGS_SCHEMA_DIR=$STARTAPP_ENV_DIR/root/usr/share/glib-2.0/schemas
export XDG_CACHE_HOME=$STARTAPP_ENV_DIR/cache
export XDG_DATA_DIRS=$STARTAPP_ENV_DIR/root/usr/share\${XDG_DATA_DIRS:+:\$XDG_DATA_DIRS}:/usr/local/share:/usr/share
export PYTHONPATH=$STARTAPP_ENV_DIR/root/usr/lib/python3/dist-packages\${PYTHONPATH:+:\$PYTHONPATH}
export PATH=$STARTAPP_ENV_DIR/root/usr/bin:\$PATH
EOF
. "$STARTAPP_ENV_DIR/activate.sh"
glib-compile-schemas "$GSETTINGS_SCHEMA_DIR"
pkg-config --modversion gtk4
