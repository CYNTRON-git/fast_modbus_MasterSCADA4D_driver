#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-host-tests}"
case "$MODE" in
  host-tests)
    cmake -S "$ROOT" -B "$ROOT/build/host" -DFMB_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
    cmake --build "$ROOT/build/host" --parallel "${BUILD_JOBS:-1}"
    ctest --test-dir "$ROOT/build/host" --output-on-failure
    ;;
  linux-armv7hf)
    shift
    exec python3 -I "$ROOT/tools/build_arm_vendor.py" "$@"
    ;;
  *) echo "Usage: $0 host-tests | linux-armv7hf --inputs KIT --work-dir NEW_DIR --toolchain MPS_TOOLCHAIN [--build]" >&2; exit 2;;
esac
