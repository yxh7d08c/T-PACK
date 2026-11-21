#!/usr/bin/env bash
set -euo pipefail
BASE=$(cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$BASE/kill_hyc3_qemu.py"
