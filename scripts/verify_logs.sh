#!/usr/bin/env bash
set -euo pipefail
BASE=$(cd -- "$(dirname -- "$0")/.." && pwd)
grep -Eq 'got result=42|shared-memory \+1 test PASSED' "$BASE/startup.log"
grep -Eq 'L2-2 computed 41 \+ 1 = 42' "$BASE/l2-2-serial.log"
echo 'Found worker computation and requester result in current launch logs.'
echo 'Verify rootfs persistence and page-access denial separately.'
