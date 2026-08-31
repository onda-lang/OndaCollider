#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT}/build}"
SERVER_PROGRAM="${ONDA_COLLIDER_SERVER_PROGRAM:-scsynth}"

case "${SERVER_PROGRAM}" in
    scsynth)
        PLUGIN_BINARY="Onda_scsynth.so"
        ;;
    supernova)
        PLUGIN_BINARY="Onda_supernova.so"
        ;;
    *)
        echo "Unsupported SuperCollider server program: ${SERVER_PROGRAM}" >&2
        exit 1
        ;;
esac

if [[ ! -f "${BUILD_DIR}/${PLUGIN_BINARY}" ]]; then
    echo "${PLUGIN_BINARY} not found in ${BUILD_DIR}; build the plugin first." >&2
    exit 1
fi

PLUGIN_DIR="$(mktemp -d)"
SCLANG_LOG="${PLUGIN_DIR}/sclang.log"
SCLANG_CONFIG="${PLUGIN_DIR}/sclang_conf.yaml"
cleanup() {
    rm -rf -- "${PLUGIN_DIR}"
}
trap cleanup EXIT
cp "${BUILD_DIR}/${PLUGIN_BINARY}" "${PLUGIN_DIR}/"

yaml_quote() {
    local value="${1//\'/\'\'}"
    printf "'%s'" "$value"
}

{
    printf 'includePaths:\n  - '
    yaml_quote "${ROOT}/plugins/Onda"
    printf '\nexcludePaths:\n  - '
    yaml_quote "${HOME}/.local/share/SuperCollider/Extensions/Onda"
    printf '\n'
} > "${SCLANG_CONFIG}"

export ONDA_COLLIDER_ROOT="${ROOT}"
export ONDA_COLLIDER_PLUGIN_PATH="${PLUGIN_DIR}"
export ONDA_COLLIDER_SERVER_PROGRAM="${SERVER_PROGRAM}"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

"${SCLANG_EXECUTABLE:-sclang}" -D -l "${SCLANG_CONFIG}" \
    "${ROOT}/tests/integration.scd" 2>&1 | tee "${SCLANG_LOG}"

if grep -q '^\^\^ ERROR:' "${SCLANG_LOG}"; then
    echo "SuperCollider reported a language exception during integration testing." >&2
    exit 1
fi

if grep -Eq 'failed to allocate runtime instance|hot-swap failed|refusing to destroy a compiled program' "${SCLANG_LOG}"; then
    echo "Onda reported an instance-lifetime failure during integration testing." >&2
    exit 1
fi

if ! grep -Fq 'typed init: 2.5 4 5 true' "${SCLANG_LOG}"; then
    echo "Onda init print output did not reach the SuperCollider server log." >&2
    exit 1
fi

if ! grep -Fq 'Onda: delegate observed(kind: 1, amount: 2.5)' "${SCLANG_LOG}"; then
    echo "Onda delegate output did not reach the SuperCollider server log." >&2
    exit 1
fi

if ! grep -Fq 'print occurrence exceeds the 2048-byte host formatting capacity' "${SCLANG_LOG}"; then
    echo "Onda did not safely reject oversized print formatting output." >&2
    exit 1
fi
