#!/bin/bash
set -euo pipefail

if [ -z "${1:-}" ]; then
    echo "[ERROR] SuperCollider path not provided."
    echo "Usage: ./$(basename "$0") /path/to/supercollider [/optional/path/to/onda-sdk or /path/to/Extensions] [/optional/install/path]"
    exit 1
fi

SC_PATH="$1"
ARG2="${2:-}"
ARG3="${3:-}"
ONDA_SDK_PATH=""
INSTALL_DEST=""
STAGING_DIR="$(pwd)/build/Install"
INSTALL_ROOT="$STAGING_DIR"
FINAL_INSTALL_DIR="$STAGING_DIR/Onda"

if [ -n "$ARG3" ]; then
    ONDA_SDK_PATH="$ARG2"
    INSTALL_DEST="$ARG3"
elif [ -n "$ARG2" ]; then
    if [ -f "$ARG2/include/onda.h" ]; then
        ONDA_SDK_PATH="$ARG2"
    else
        INSTALL_DEST="$ARG2"
    fi
fi

if [ -z "$ONDA_SDK_PATH" ]; then
    ONDA_SDK_PATH="$(pwd)/build/onda-sdk"
    echo "[INFO] Onda SDK path not provided. Downloading the configured Onda release..."
    bash "$(dirname "$0")/scripts/fetch-onda.sh" "$ONDA_SDK_PATH"
fi

if [ -n "$INSTALL_DEST" ]; then
    INSTALL_BASENAME="$(basename "$INSTALL_DEST")"
    INSTALL_PARENT="$(dirname "$INSTALL_DEST")"

    if [ "$INSTALL_BASENAME" = "Onda" ]; then
        INSTALL_ROOT="$INSTALL_PARENT"
        FINAL_INSTALL_DIR="$INSTALL_DEST"
    else
        INSTALL_ROOT="$INSTALL_DEST"
        FINAL_INSTALL_DIR="$INSTALL_DEST/Onda"
    fi
fi

echo "[INFO] SC_PATH: $SC_PATH"
echo "[INFO] ONDA_SDK_PATH: $ONDA_SDK_PATH"
echo "[INFO] INSTALL_ROOT: $INSTALL_ROOT"
echo "[INFO] FINAL_INSTALL_DIR: $FINAL_INSTALL_DIR"

echo "[INFO] Configuring CMake..."
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DSC_PATH="$SC_PATH" \
    -DONDA_SDK_PATH="$ONDA_SDK_PATH" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_ROOT"

echo "[INFO] Building Release configuration..."
cmake --build build --config Release --parallel

echo "[INFO] Packaging (installing to target prefix)..."
cmake --install build --config Release

echo "[SUCCESS] Build complete. Package available at $FINAL_INSTALL_DIR"
