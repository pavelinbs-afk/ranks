#!/bin/bash
# Build lr_core with CMake for CS2 linuxsteamrt64.
#
# IMPORTANT: Do not compile on a very new host (Ubuntu 24+/WSL 2.43) and deploy
# directly — the .so will require GLIBC_2.38+ and fail on the game server.
# By default this script builds inside Docker (Ubuntu 22.04), same idea as
# build_linux.sh / lvlranks-builder2.
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

# Separate artifacts when built in Docker so host GLIBC does not leak in.
if [ "${LR_CORE_IN_DOCKER:-0}" = "1" ]; then
  MARIADB_PREFIX="$DEPS/mariadb-cmake"
  BUILD="$ROOT/build"
else
  MARIADB_PREFIX="$DEPS/mariadb"
  # WSL: CMake on /mnt/d often fails with "Operation not permitted" — use Linux fs.
  if [[ "$ROOT" == /mnt/* ]]; then
    BUILD="${LR_CORE_BUILD_DIR:-$HOME/.cache/lr_core/build}"
  else
    BUILD="$ROOT/build"
  fi
fi

MARIADB_SRC="$DEPS/mariadb-connector-c"
MARIADB_TAG="v3.3.10"
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
    local ver
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
  [ -f /usr/include/openssl/ssl.h ] || missing+=(libssl-dev)
  if [ ${#missing[@]} -gt 0 ]; then
    cat >&2 <<EOF
Missing packages: ${missing[*]}

Install once:
  sudo apt-get update
  sudo apt-get install -y build-essential cmake git libssl-dev zlib1g-dev

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

if [ ! -f "$MARIADB_PREFIX/lib/mariadb/libmariadbclient.a" ]; then
  echo "==> Building static mariadb-connector-c -> $MARIADB_PREFIX"
  if [ ! -d "$MARIADB_SRC/.git" ]; then
    git clone --depth 1 --branch "$MARIADB_TAG" \
      https://github.com/mariadb-corporation/mariadb-connector-c.git "$MARIADB_SRC"
  fi
  MARIADB_BUILD="$MARIADB_SRC/build-static-cmake"
  rm -rf "$MARIADB_BUILD"
  cmake -S "$MARIADB_SRC" -B "$MARIADB_BUILD" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$MARIADB_PREFIX" \
    -DCMAKE_C_STANDARD=17 \
    -DCMAKE_C_STANDARD_REQUIRED=ON \
    -DCMAKE_C_EXTENSIONS=ON \
    -DCMAKE_C_FLAGS="-std=gnu17 -Wno-error" \
    -DBUILD_SHARED_LIBS=OFF \
    -DWITH_SSL=OPENSSL \
    -DWITH_ICONV=OFF \
    -DWITH_CURL=OFF \
    -DWITH_ZSTD=OFF
  cmake --build "$MARIADB_BUILD" -j"$(nproc)"
  cmake --install "$MARIADB_BUILD"

  if [ ! -f "$MARIADB_PREFIX/lib/mariadb/libmariadbclient.a" ]; then
    FOUND="$(find "$MARIADB_PREFIX" -name 'libmariadbclient.a' -print -quit)"
    if [ -z "$FOUND" ]; then
      echo "MariaDB static lib not found under $MARIADB_PREFIX"
      exit 1
    fi
    mkdir -p "$MARIADB_PREFIX/lib/mariadb"
    cp "$FOUND" "$MARIADB_PREFIX/lib/mariadb/libmariadbclient.a"
  fi
fi

chmod +x "$DEPS/hl2sdk-cs2/devtools/bin/linux/protoc" 2>/dev/null || true

cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHL2SDK_PATH="$DEPS/hl2sdk-cs2" \
  -DMMS_PATH="$DEPS/metamod-source" \
  -DMARIADB_PREFIX="$MARIADB_PREFIX"

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
