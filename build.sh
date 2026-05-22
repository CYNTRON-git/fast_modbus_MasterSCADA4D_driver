#!/usr/bin/env bash
# build.sh — reproducible build of mplc_protocol_fast_modbus for MasterSCADA 4D
#
# Usage:
#   ./build.sh [linux-armv7hf | linux-x64 | linux-armv8]
#
# Prerequisites:
#   • arm-linux-gnueabihf-g++ (Ubuntu: sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf)
#   • MasterSCADA API headers in API/ (see docs/BUILD_INSTRUCTIONS.md)
#   • SDK .so files in platform/linux/api/mplc_lib_so/
#     (copy from /opt/mplc4/ on the target controller, or see BUILD_INSTRUCTIONS.md)
#   • GNU make

set -euo pipefail

PLATFORM="${1:-linux-armv7hf}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/platform/linux/api"
TARGET="mplc_protocol_fast_modbus"

# ---- Select toolchain ----
case "${PLATFORM}" in
    linux-armv7hf)
        CXX=arm-linux-gnueabihf-g++
        CC=arm-linux-gnueabihf-gcc
        ;;
    linux-armv8)
        CXX=aarch64-linux-gnu-g++
        CC=aarch64-linux-gnu-gcc
        ;;
    linux-x64)
        CXX=g++
        CC=gcc
        ;;
    *)
        echo "ERROR: Unknown platform '${PLATFORM}'." >&2
        echo "Supported: linux-armv7hf  linux-armv8  linux-x64" >&2
        exit 1
        ;;
esac

# ---- Verify toolchain ----
if ! command -v "${CXX}" &>/dev/null; then
    echo "ERROR: Compiler '${CXX}' not found." >&2
    echo "  Ubuntu: sudo apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf" >&2
    exit 1
fi

# ---- Verify SDK libs ----
LIB_DIR="${BUILD_DIR}/mplc_lib_so"
REQUIRED_LIBS=(masterplc.so mplcshare.so mplc_archive.so opcua.so liblua.so mplc_events.so)
MISSING=()
for lib in "${REQUIRED_LIBS[@]}"; do
    [[ -f "${LIB_DIR}/${lib}" ]] || MISSING+=("${lib}")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "ERROR: SDK libraries missing in ${LIB_DIR}/:" >&2
    printf '  %s\n' "${MISSING[@]}" >&2
    echo "  Copy from /opt/mplc4/ on the SA-02m / controller, or see docs/BUILD_INSTRUCTIONS.md" >&2
    exit 1
fi

# ---- Build ----
DEP_LIST="${REQUIRED_LIBS[*]/#/mplc_lib_so/}"
MPLCLIBS="$(realpath "${LIB_DIR}")"

echo "Platform : ${PLATFORM}"
echo "Compiler : $(${CXX} --version | head -1)"
echo "Target   : ${TARGET}.so"

(
    cd "${BUILD_DIR}"
    CXX="${CXX}" CC="${CC}" \
    MPLCLIBS="${MPLCLIBS}" \
    DEPs="${DEP_LIST}" \
    make -j"$(nproc)" "${TARGET}"
)

OUT="${BUILD_DIR}/${TARGET}.so"
if [[ ! -f "${OUT}" ]]; then
    echo "ERROR: Build failed — output not found: ${OUT}" >&2
    exit 1
fi

echo ""
echo "Build succeeded:"
ls -lh "${OUT}"
file "${OUT}"
