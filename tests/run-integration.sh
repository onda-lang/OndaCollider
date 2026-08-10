#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build}"

if [[ ! -f "${BUILD_DIR}/Onda_scsynth.so" ]]; then
    echo "Onda_scsynth.so not found in ${BUILD_DIR}; build the plugin first." >&2
    exit 1
fi

PLUGIN_DIR="$(mktemp -d)"
SCLANG_LOG="${PLUGIN_DIR}/sclang.log"
cleanup() {
    rm -rf -- "${PLUGIN_DIR}"
}
trap cleanup EXIT
cp "${BUILD_DIR}/Onda_scsynth.so" "${PLUGIN_DIR}/"

export ONDA_COLLIDER_ROOT="${ROOT}"
export ONDA_COLLIDER_PLUGIN_PATH="${PLUGIN_DIR}"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

"${SCLANG_EXECUTABLE:-sclang}" -D \
    --exclude-path "${HOME}/.local/share/SuperCollider/Extensions/Onda" \
    --include-path "${ROOT}/plugins/Onda" \
    "${ROOT}/tests/integration.scd" 2>&1 | tee "${SCLANG_LOG}"

if grep -q '^\^\^ ERROR:' "${SCLANG_LOG}"; then
    echo "SuperCollider reported a language exception during integration testing." >&2
    exit 1
fi
