#!/usr/bin/env bash
# Builds dist/OpenCaliper-x86_64.AppImage inside an Ubuntu 22.04 container.
# Building on an old distro keeps the glibc requirement low (2.35), so the
# AppImage also runs on older distributions.
# Needs podman or docker and internet access. The first run compiles
# OpenCASCADE (~20-40 min); later runs reuse packaging/appimage/.cache.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="$(command -v podman || command -v docker || true)"
if [ -z "$ENGINE" ]; then
  echo "podman or docker is required" >&2
  exit 1
fi

mkdir -p "$ROOT/packaging/appimage/.cache" "$ROOT/dist"
"$ENGINE" run --rm \
  -v "$ROOT:/src:Z" \
  -e SRC=/src -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  ubuntu:22.04 bash /src/packaging/appimage/inside-container.sh
