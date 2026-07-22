#!/bin/bash
set -euo pipefail

DESTINATION="${1:-}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VERSION_FILE="${SCRIPT_DIR}/../ONDA_VERSION"

if [ -z "${ONDA_VERSION:-}" ]; then
    if [ ! -f "$VERSION_FILE" ]; then
        echo "[ERROR] Onda version file not found at ${VERSION_FILE}"
        exit 1
    fi
    ONDA_VERSION="$(tr -d '\r\n' < "$VERSION_FILE")"
fi

if [ -z "$ONDA_VERSION" ]; then
    echo "[ERROR] Onda version is empty. Set ONDA_VERSION or update ${VERSION_FILE}."
    exit 1
fi

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

if [ -f "${DESTINATION}/include/onda.h" ] \
    && [ -d "${DESTINATION}/lib" ] \
    && [ -f "${DESTINATION}/.onda-version" ] \
    && [ "$(tr -d '\r\n' < "${DESTINATION}/.onda-version")" = "$ONDA_VERSION" ]; then
    echo "[INFO] Reusing Onda SDK ${ONDA_VERSION} at ${DESTINATION}"
    exit 0
fi

API_BASE_URL="${ONDA_API_BASE_URL:-https://api.github.com/repos/onda-lang/onda/releases}"
API_HEADERS=(
    -H "Accept: application/vnd.github+json"
    -H "X-GitHub-Api-Version: 2022-11-28"
)
API_TOKEN="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
if [ -n "$API_TOKEN" ]; then
    API_HEADERS+=(-H "Authorization: Bearer ${API_TOKEN}")
fi

fetch_release_json() {
    local url="$1"
    curl -fsSL "${API_HEADERS[@]}" "$url"
}

RELEASE_API_URL="${API_BASE_URL}/tags/${ONDA_VERSION}"
echo "[INFO] Resolving Onda release ${ONDA_VERSION}"
RELEASE_JSON="$(fetch_release_json "$RELEASE_API_URL")"

RELEASE_TAG="$(python3 -c 'import json,sys; data=json.load(sys.stdin); print((data[0] if isinstance(data,list) else data).get("tag_name",""))' <<<"$RELEASE_JSON")"
ARCHIVE_URL="$(python3 -c 'import json,sys,re; data=json.load(sys.stdin); rel=(data[0] if isinstance(data,list) else data); suffix=sys.argv[1]; rx=re.compile(r"/onda-[^/]*-" + re.escape(suffix) + r"$"); print(next((a.get("browser_download_url","") for a in rel.get("assets",[]) if rx.search(a.get("browser_download_url",""))), ""))' "$ASSET_SUFFIX" <<<"$RELEASE_JSON")"

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
printf '%s\n' "$RELEASE_TAG" > "${DESTINATION}/.onda-version"

echo "[INFO] Onda SDK ${RELEASE_TAG} ready at ${DESTINATION}"
