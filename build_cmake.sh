#!/bin/bash
# Build lr_core (TAB-only) with CMake for CS2 linuxsteamrt64.
#
# IMPORTANT: Do not compile on a very new host (Ubuntu 24+/WSL 2.43) and deploy
# directly — the .so will require GLIBC_2.38+ and fail on the game server.
# By default this script builds inside Docker (Ubuntu 22.04).
#
# Prerequisites (host):
#   Docker, OR set LR_CORE_NATIVE=1 on Ubuntu 20.04/22.04 only.
#
# Usage:
#   cd lr_core && bash build_cmake.sh
#   LR_CORE_NATIVE=1 bash build_cmake.sh   # local build (dev only)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_ROOT="$(cd "$ROOT/.." && pwd)"
DEPS="${DEPS_ROOT:-$WORK_ROOT/deps}"

if [ "${LR_CORE_IN_DOCKER:-0}" = "1" ]; then
  BUILD="$ROOT/build"
else
  if [[ "$ROOT" == /mnt/* ]]; then
    BUILD="${LR_CORE_BUILD_DIR:-$HOME/.cache/lr_core/build}"
  else
    BUILD="$ROOT/build"
  fi
fi

DOCKER_IMAGE="${LR_CORE_DOCKER_IMAGE:-lr_core-cmake-builder}"

lr_core_host_glibc_ok() {
  if [ -f /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    case "${VERSION_ID:-}" in
      20.04|22.04) return 0 ;;
    esac
  fi
  if command -v ldd >/dev/null 2>&1; then
    local ve
    ver="$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1 || true)"
    case "$ver" in
      2.3[0-5]|2.2[0-9]|2.1[0-9]|2.[0-9]) return 0 ;;
    esac
  fi
  return 1
}

if [ "${LR_CORE_NATIVE:-0}" != "1" ] && [ "${LR_CORE_IN_DOCKER:-0}" != "1" ]; then
  if lr_core_host_glibc_ok; then
    echo "==> Ubuntu 20.04/22.04 (or GLIBC <= 2.35) detected — native build"
    LR_CORE_NATIVE=1
  elif ! command -v docker >/dev/null 2>&1; then
    cat >&2 <<'EOF'
ERROR: Docker is required for a CS2-compatible build on this host.

Your WSL/system GLIBC is too new; the plugin would require GLIBC_2.38+ on the server.

Options:
  1) Install Docker, then re-run: bash build_cmake.sh
  2) Build inside Ubuntu 20.04/22.04 (WSL distro), then re-run: bash build_cmake.sh
  3) Use the AMBuild path: bash build_linux.sh (also needs Docker)
EOF
    exit 1
  fi
fi

if [ "${LR_CORE_NATIVE:-0}" != "1" ] && [ "${LR_CORE_IN_DOCKER:-0}" != "1" ]; then
  if ! docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
    echo "==> Building Docker image $DOCKER_IMAGE (one-time)"
    docker build -f "$ROOT/Dockerfile.cmake" -t "$DOCKER_IMAGE" "$ROOT"
  fi

  echo "==> Building lr_core inside Docker ($DOCKER_IMAGE) for CS2 GLIBC compatibility"
  docker run --rm \
    -v "$WORK_ROOT:/work" \
    -e LR_CORE_IN_DOCKER=1 \
    -e LR_CORE_NATIVE=1 \
    "$DOCKER_IMAGE" \
    bash -lc "sed -i 's/\r$//' /work/lr_core/build_cmake.sh && bash /work/lr_core/build_cmake.sh"
  exit $?
fi

mkdir -p "$DEPS"

if command -v apt-get >/dev/null 2>&1; then
  missing=()
  command -v cmake >/dev/null 2>&1 || missing+=(cmake)
  command -v git >/dev/null 2>&1 || missing+=(git)
  command -v g++ >/dev/null 2>&1 || missing+=(build-essential)
  if [ ${#missing[@]} -gt 0 ]; then
    cat >&2 <<EOF
Missing packages: ${missing[*]}

Install once:
  sudo apt-get update
  sudo apt-get install -y build-essential cmake git

Then re-run: bash build_cmake.sh
EOF
    exit 1
  fi
fi

if [ "$BUILD" != "$ROOT/build" ]; then
  echo "==> Build dir (Linux fs): $BUILD"
fi

if [ ! -f "$DEPS/hl2sdk-cs2/public/tier0/dbg.h" ]; then
  echo "==> Cloning hl2sdk-cs2"
  rm -rf "$DEPS/hl2sdk-cs2"
  git clone --branch cs2 --depth 1 https://github.com/alliedmodders/hl2sdk.git "$DEPS/hl2sdk-cs2"
fi

if [ ! -f "$DEPS/metamod-source/core/ISmmPlugin.h" ]; then
  echo "==> Cloning metamod-source"
  git clone --recursive --depth 1 https://github.com/alliedmodders/metamod-source.git "$DEPS/metamod-source"
fi

chmod +x "$DEPS/hl2sdk-cs2/devtools/bin/linux/protoc" 2>/dev/null || true

cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHL2SDK_PATH="$DEPS/hl2sdk-cs2" \
  -DMMS_PATH="$DEPS/metamod-source"

cmake --build "$BUILD" -j"$(nproc)"

SO="$BUILD/addons/lr_core/bin/linuxsteamrt64/lr_core.so"
if [ -f "$SO" ] && command -v objdump >/dev/null 2>&1; then
  MAX_GLIBC="$(objdump -T "$SO" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -u -V | tail -1 || true)"
  if [ -n "$MAX_GLIBC" ]; then
    echo "==> Max GLIBC symbol required: $MAX_GLIBC"
    case "$MAX_GLIBC" in
      GLIBC_2.3[89]|GLIBC_2.4*|GLIBC_3.*)
        echo "WARNING: This binary may not load on CS2 servers (GLIBC too new)." >&2
        ;;
    esac
  fi
fi

echo "=== BUILD OK ==="
find "$BUILD/addons" -type f
if [ "$BUILD" != "$ROOT/build" ] && [ -d "$BUILD/addons" ]; then
  rm -rf "$ROOT/build"
  mkdir -p "$ROOT/build"
  cp -a "$BUILD/addons" "$ROOT/build/"
  echo "==> Copied addons -> $ROOT/build/addons"
fi
