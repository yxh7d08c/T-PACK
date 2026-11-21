#!/bin/bash
HYC3_BASE_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
export HYC3_BASE_DIR
export HYC3_MODE=bringup
exec $HYC3_BASE_DIR/run_qemu.sh "$@"
