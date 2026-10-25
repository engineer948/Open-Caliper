#!/usr/bin/env bash
# Runs inside ubuntu:22.04 as root: from build-appimage.sh, or directly in CI
# (.github/workflows/build.yml). Do not run on your own machine.
set -euo pipefail

SRC=${SRC:-/src}
APP_ID=io.github.engineer948.OpenCaliper
OCCT_TAG=V7_9_3
CACHE=$SRC/packaging/appimage/.cache
OCCT_PREFIX=$CACHE/occt-$OCCT_TAG

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build git wget file ca-certificates \
  qt6-base-dev qt6-base-dev-tools libqt6opengl6-dev qt6-qpa-plugins \
  libgl-dev libgl1-mesa-dev libx11-dev libxext-dev libxi-dev libxmu-dev \
  libxkbcommon-dev libvulkan-dev libfreetype6-dev libfontconfig1-dev

# 1. OpenCASCADE (cached)
mkdir -p "$CACHE"
if [ ! -f "$OCCT_PREFIX/lib/cmake/opencascade/OpenCASCADEConfig.cmake" ]; then
  rm -rf /tmp/occt /tmp/occt-build
  git clone --depth 1 --branch "$OCCT_TAG" https://github.com/Open-Cascade-SAS/OCCT.git /tmp/occt
  cmake -S /tmp/occt -B /tmp/occt-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DINSTALL_DIR="$OCCT_PREFIX" \
    -DBUILD_MODULE_Draw=OFF \
    -DBUILD_DOC_Overview=OFF \
    -DUSE_FREETYPE=ON \
    -DUSE_TK=OFF
  cmake --build /tmp/occt-build
  cmake --install /tmp/occt-build
fi

# 2. The app
rm -rf /tmp/app-build /tmp/AppDir
cmake -S "$SRC" -B /tmp/app-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="$OCCT_PREFIX"
cmake --build /tmp/app-build
DESTDIR=/tmp/AppDir cmake --install /tmp/app-build

# 3. AppImage (linuxdeploy collects Qt, OCCT and their dependencies)
for tool in linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
            linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage; do
  [ -f "$CACHE/$(basename "$tool")" ] || wget -q -O "$CACHE/$(basename "$tool")" "https://github.com/$tool"
  chmod +x "$CACHE/$(basename "$tool")"
done

mkdir -p "$SRC/dist"
export APPIMAGE_EXTRACT_AND_RUN=1 # no FUSE inside containers
export QMAKE=/usr/lib/qt6/bin/qmake
export LD_LIBRARY_PATH="$OCCT_PREFIX/lib"
export OUTPUT="$SRC/dist/OpenCaliper-x86_64.AppImage"
cd /tmp
"$CACHE/linuxdeploy-x86_64.AppImage" --appdir /tmp/AppDir \
  --desktop-file "/tmp/AppDir/usr/share/applications/$APP_ID.desktop" \
  --icon-file "/tmp/AppDir/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" \
  --plugin qt --output appimage

if [ -n "${HOST_UID:-}" ]; then chown -R "$HOST_UID:$HOST_GID" "$SRC/dist" "$CACHE"; fi
echo "Done: dist/OpenCaliper-x86_64.AppImage"
