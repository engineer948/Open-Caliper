#!/usr/bin/env bash
# Builds dist/OpenCaliper.flatpak on this machine.
# Needs: flatpak, flatpak-builder and internet access (KDE runtime + OCCT sources).
# The first run compiles OpenCASCADE (~20-40 min); later runs reuse the cache.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP_ID=io.github.open_workshop_tools.OpenCaliper
MANIFEST="$ROOT/packaging/flatpak/$APP_ID.yml"
cd "$ROOT"

for tool in flatpak flatpak-builder; do
  if ! command -v "$tool" >/dev/null; then
    echo "ERROR: $tool is not installed. Install it first:" >&2
    echo "  Fedora:          sudo dnf install flatpak-builder" >&2
    echo "  Ubuntu/Debian:   sudo apt install flatpak-builder" >&2
    echo "  Arch:            sudo pacman -S flatpak-builder" >&2
    exit 1
  fi
done

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
  --repo=build-flatpak/repo build-flatpak/app "$MANIFEST"

mkdir -p dist
# --runtime-repo lets `flatpak install` fetch the KDE runtime from Flathub by itself.
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  build-flatpak/repo dist/OpenCaliper.flatpak "$APP_ID"
echo "Done: dist/OpenCaliper.flatpak"
echo "Install: flatpak install --user dist/OpenCaliper.flatpak"
