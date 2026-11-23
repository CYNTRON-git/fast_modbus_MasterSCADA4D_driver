#!/usr/bin/env bash
set -euo pipefail
workspace=${CYNTRON_BUILD_ROOT:-/Users/cyntron/cyntron-build}
repo=$(cd "$(dirname "$0")/.." && pwd)
source "$workspace/env/base.sh"
out=${1:?new build directory required}
mkdir -p "$(dirname "$out")"
mkdir "$out"
out=$(cd "$out" && pwd)
exec "$workspace/run-task" --profile background -- docker --context colima-cyntron run --rm \
 --platform linux/amd64 --pull=never --network none --read-only --cpus 1 --memory 2g --pids-limit 128 \
 --tmpfs /tmp:rw,size=256m --mount type=volume,source=cyntron-sa-bsp-work,target=/kit,readonly \
 --mount "type=bind,source=$repo,target=/src,readonly" \
 --mount "type=bind,source=$out,target=/output" \
 sha256:9af56be350d47f08b71df02abac56118fdc3bfa886adc26c64eb612fb3d50e7a \
 timeout 900 python3 -I /src/tools/build_arm_vendor.py --inputs /kit/inputs/sa-mplc-inputs \
 --work-dir /output/work --toolchain /kit/toolchains/arm-mpssoft-linux-gnueabihf --build
