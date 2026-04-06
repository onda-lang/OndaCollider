#!/bin/bash
set -euo pipefail

DESTINATION="${1:-}"

if [ -z "$DESTINATION" ]; then
    echo "Usage: bash ./scripts/fetch-onda.sh /path/to/onda-sdk"
    exit 1
fi

OS_NAME="$(uname -s)"
ARCH_NAME="$(uname -m)"

case "$OS_NAME/$ARCH_NAME" in
    Linux/x86_64|Linux/amd64)
        ASSET_SUFFIX="linux-x64.tar.xz"
        ;;
    Darwin/arm64|Darwin/aarch64)
        ASSET_SUFFIX="macos-arm64.tar.xz"
        ;;
    *)
        echo "[ERROR] No official Onda SDK asset configured for ${OS_NAME}/${ARCH_NAME}."
        echo "[ERROR] Pass an extracted SDK path explicitly instead."
        exit 1
        ;;
esac

if [ -f "${DESTINATION}/include/onda.h" ] && [ -d "${DESTINATION}/lib" ]; then
    echo "[INFO] Reusing existing Onda SDK at ${DESTINATION}"
    exit 0
fi

API_BASE_URL="${ONDA_API_BASE_URL:-https://api.github.com/repos/onda-lang/onda/releases}"
API_HEADERS=(
    -H "Accept: application/vnd.github+json"
    -H "X-GitHub-Api-Version: 2022-11-28"
)

if [ -n "${ONDA_VERSION:-}" ]; then
    RELEASE_API_URL="${API_BASE_URL}/tags/${ONDA_VERSION}"
    echo "[INFO] Resolving Onda release ${ONDA_VERSION}"
    RELEASE_JSON="$(curl -fsSL "${API_HEADERS[@]}" "$RELEASE_API_URL")"
else
    echo "[INFO] Resolving latest Onda release"
    if ! RELEASE_JSON="$(curl -fsSL "${API_HEADERS[@]}" "${API_BASE_URL}/latest")"; then
        echo "[INFO] '/releases/latest' is unavailable; falling back to the releases list."
        RELEASE_JSON="$(curl -fsSL "${API_HEADERS[@]}" "${API_BASE_URL}?per_page=20")"
    fi
fi
RELEASE_TAG="$(printf '%s' "$RELEASE_JSON" | sed 's/,"/\n"/g' | sed -n 's/.*"tag_name":"\([^"]*\)".*/\1/p' | head -n1)"
ARCHIVE_URL="$(printf '%s' "$RELEASE_JSON" | sed 's/,"/\n"/g' | sed -n 's/.*"browser_download_url":"\([^"]*\)".*/\1/p' | grep -E "/onda-[^/]*-${ASSET_SUFFIX}$" | head -n1)"

if [ -z "$RELEASE_TAG" ]; then
    echo "[ERROR] Failed to resolve an Onda release from GitHub."
    exit 1
fi

if [ -z "$ARCHIVE_URL" ]; then
    echo "[ERROR] Release ${RELEASE_TAG} does not contain a matching ${ASSET_SUFFIX} SDK asset"
    exit 1
fi

ASSET_NAME="$(basename "$ARCHIVE_URL")"

CACHE_DIR="$(dirname "$DESTINATION")/.onda-downloads"
ARCHIVE_PATH="${CACHE_DIR}/${RELEASE_TAG}-${ASSET_NAME}"
TMP_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

mkdir -p "$CACHE_DIR"

echo "[INFO] Downloading ${ARCHIVE_URL}"
curl -L --fail --retry 3 -o "$ARCHIVE_PATH" "$ARCHIVE_URL"

echo "[INFO] Extracting ${ASSET_NAME}"
tar -xf "$ARCHIVE_PATH" -C "$TMP_DIR"

SDK_ROOT="$TMP_DIR"
if [ ! -f "${SDK_ROOT}/include/onda.h" ]; then
    HEADER_PATH="$(find "$TMP_DIR" -path '*/include/onda.h' -print -quit || true)"
    if [ -z "$HEADER_PATH" ]; then
        echo "[ERROR] Extracted archive does not contain include/onda.h"
        exit 1
    fi
    SDK_ROOT="$(cd "$(dirname "$(dirname "$HEADER_PATH")")" && pwd)"
fi

if [ ! -d "${SDK_ROOT}/lib" ]; then
    echo "[ERROR] Extracted archive does not contain a lib directory"
    exit 1
fi

rm -rf "$DESTINATION"
mkdir -p "$(dirname "$DESTINATION")"
cp -R "$SDK_ROOT" "$DESTINATION"

echo "[INFO] Onda SDK ${RELEASE_TAG} ready at ${DESTINATION}"
