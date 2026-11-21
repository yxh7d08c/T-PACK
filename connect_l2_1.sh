#!/bin/bash
HYC3_BASE_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
export HYC3_BASE_DIR
set -euo pipefail
SOCK=${HYC3_L2_1_SOCK:-$HYC3_BASE_DIR/l2-1-serial.sock}
if [ ! -S "${SOCK}" ]; then
  echo "connect_l2_1.sh: ${SOCK} is not ready. Start $HYC3_BASE_DIR/run_qemu.sh first." >&2
  exit 1
fi
if command -v socat >/dev/null 2>&1; then
  echo "connected. Press Enter if the prompt is not visible; Ctrl-] detaches." >&2
exec socat -,raw,echo=0,escape=0x1d UNIX-CONNECT:"${SOCK}"
fi
if command -v nc >/dev/null 2>&1; then
  exec nc -U "${SOCK}"
fi
echo "connect_l2_1.sh: neither socat nor nc is installed." >&2
exit 1
