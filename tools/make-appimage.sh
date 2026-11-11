#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds aslgc-<version>-<arch>.AppImage with linuxdeploy and its Qt plugin.
# Qt and QtKeychain are bundled; git is not (aslgc always runs the system git).
# The AppImage runs on distributions with the same or a newer glibc than the build machine,
# so release builds are made on the oldest supported distribution (Ubuntu 24.04).
#
# Usage: tools/make-appimage.sh [output-dir]
# Needs linuxdeploy-<arch>.AppImage and linuxdeploy-plugin-qt-<arch>.AppImage in $LINUXDEPLOY_DIR
# (default: on PATH). Set QMAKE if Qt 6's qmake is not found on its own.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(mkdir -p "${1:-$root}" && cd "${1:-$root}" && pwd)"
arch="$(uname -m)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

linuxdeploy="linuxdeploy-$arch.AppImage"
if [[ -n "${LINUXDEPLOY_DIR:-}" ]]; then
    export PATH="$LINUXDEPLOY_DIR:$PATH"
fi
command -v "$linuxdeploy" >/dev/null || { echo "$linuxdeploy not found (set LINUXDEPLOY_DIR)" >&2; exit 1; }
command -v "linuxdeploy-plugin-qt-$arch.AppImage" >/dev/null ||
    { echo "linuxdeploy-plugin-qt-$arch.AppImage not found (set LINUXDEPLOY_DIR)" >&2; exit 1; }

if [[ -z "${QMAKE:-}" ]]; then
    for candidate in qmake6 /usr/lib/qt6/bin/qmake /usr/lib64/qt6/bin/qmake "/usr/lib/$arch-linux-gnu/qt6/bin/qmake"; do
        if command -v "$candidate" >/dev/null; then QMAKE="$(command -v "$candidate")"; break; fi
    done
fi
[[ -n "${QMAKE:-}" ]] || { echo "Qt 6 qmake not found (set QMAKE)" >&2; exit 1; }
export QMAKE

version="$(sed -n 's/^project(aslgc VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
appdir="$work/AppDir"

cmake -S "$root" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF \
    ${ASLGC_GITHUB_CLIENT_ID:+-DASLGC_GITHUB_CLIENT_ID="$ASLGC_GITHUB_CLIENT_ID"}
cmake --build "$work/build" --parallel
DESTDIR="$appdir" cmake --install "$work/build"

# The credential helper lives in libexec, away from the qt.conf that linuxdeploy writes next to
# the app, and needs its own to find the bundled plugins.
printf '[Paths]\nPrefix = ../../\nPlugins = plugins\n' >"$appdir/usr/libexec/aslgc/qt.conf"

# Start the app by its real path. With linuxdeploy's default AppRun symlink, Qt takes the mount
# point as the application directory and finds neither qt.conf nor the credential helper.
cat >"$work/AppRun" <<'EOF'
#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"
exec "$here/usr/bin/aslgc" "$@"
EOF
chmod +x "$work/AppRun"

# X11 is deployed by default; add Wayland (one plugin since Qt 6.10, egl + generic before) and
# offscreen, which lets CI start the AppImage without a display.
plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
wayland=()
for plugin in libqwayland.so libqwayland-egl.so libqwayland-generic.so; do
    [[ -e "$plugins/platforms/$plugin" ]] && wayland+=("$plugin")
done
[[ ${#wayland[@]} -gt 0 ]] || { echo "Qt Wayland platform plugin not found in $plugins/platforms" >&2; exit 1; }
EXTRA_PLATFORM_PLUGINS="$(IFS=';'; echo "${wayland[*]};libqoffscreen.so")"
export EXTRA_PLATFORM_PLUGINS
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export NO_STRIP=1  # distribution libraries are stripped already; linuxdeploy's strip rejects newer ELF sections
export APPIMAGE_EXTRACT_AND_RUN=1  # lets linuxdeploy run where FUSE is unavailable (containers)

cd "$work"
"$linuxdeploy" --appdir "$appdir" --custom-apprun "$work/AppRun" \
    --deploy-deps-only "$appdir/usr/libexec/aslgc/aslgc-askpass" \
    --plugin qt --output appimage
mv aslgc-*.AppImage "$out/"
ls -l "$out"/aslgc-*.AppImage
