#!/usr/bin/env bash
set -euo pipefail
BASE=$(cd -- "$(dirname -- "$0")/.." && pwd)
failed=0
for p in qemu-tdp/build/qemu-system-x86_64 td-partitioning-svsm/svsm.bin edk2-clean/Build/OvmfX64/DEBUG_GCC5/FV/OVMF.fd vmlinuz-5.14.0-710.el9.x86_64 hyc3-bringup-initramfs.img hyc3-bringup-initramfs-l2-2.img; do
  if [[ ! -s "$BASE/$p" ]]; then echo "Missing or empty: $BASE/$p" >&2; failed=1; fi
done
disk1="$BASE/hyc3-l2-1-rootfs.qcow2"
disk2="$BASE/hyc3-l2-2-rootfs.qcow2"
if [[ -n "${HYC3_L2_ROOT_DISK1:-}" ]]; then disk1="${HYC3_L2_ROOT_DISK1:-}"; fi
if [[ -n "${HYC3_L2_ROOT_DISK2:-}" ]]; then disk2="${HYC3_L2_ROOT_DISK2:-}"; fi
for p in "$disk1" "$disk2"; do
  if [[ ! -s "$p" ]]; then echo "Missing or empty root disk: $p" >&2; failed=1; fi
done
if [[ -e "$disk1" && -e "$disk2" ]] && [[ "$disk1" -ef "$disk2" ]]; then
  echo 'Two guests require distinct writable disks.' >&2; failed=1
fi
[[ -c /dev/kvm ]] || { echo '/dev/kvm unavailable' >&2; failed=1; }
for c in python3 socat sudo; do command -v "$c" >/dev/null || { echo "Missing $c" >&2; failed=1; }; done
if (( failed )); then echo 'Read readme.md and supply the required artifacts.' >&2; exit 2; fi
echo 'File checks passed. Verify TDX host support separately.'
