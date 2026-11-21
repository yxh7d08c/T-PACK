#!/usr/bin/env bash
set -euo pipefail
BASE=$(cd -- "$(dirname -- "$0")/.." && pwd)
JOBS=$(getconf _NPROCESSORS_ONLN)
case "${1:-}" in
 qemu)
  mkdir -p "$BASE/qemu-tdp/build"; cd "$BASE/qemu-tdp/build"
  ../configure --target-list=x86_64-softmmu --enable-kvm --disable-werror --enable-slirp
  ninja -j "$JOBS" ;;
 edk2)
  cd "$BASE/edk2-clean"
  make -C BaseTools -j "$JOBS"
  set +u
  source edksetup.sh
  build -a X64 -p OvmfPkg/OvmfPkgX64.dsc -t GCC5 -b DEBUG -n "$JOBS" ;;
 guest)
  [[ -d "${KDIR:-}" ]] || { echo 'Set KDIR to the build tree matching the GUEST kernel.' >&2; exit 2; }
  cd "$BASE"
  make KDIR="${KDIR:-}"
  gcc -nostdlib -static -fno-pie -no-pie -fno-stack-protector -Os -Wl,-e,_start -o hyc3-module-init hyc3-module-init.c
  install -m755 hyc3-module-init initrd-root/init
  install -m644 tdcall_module.ko initrd-root/tdcall_module.ko
  "$BASE/scripts/build.sh" initrd ;;
 initrd)
  cd "$BASE/initrd-root"
  find . -print0 | LC_ALL=C sort -z | cpio --null -o --format=newc --owner=0:0 | gzip -n -9 > "$BASE/hyc3-bringup-initramfs.img"
  cp "$BASE/hyc3-bringup-initramfs.img" "$BASE/hyc3-bringup-initramfs-l2-2.img" ;;
 svsm-help)
  printf '%s\n' 'On the designated SVSM build host:' 'cd td-partitioning-svsm' 'rustup toolchain install 1.75' 'rustup target add --toolchain 1.75 x86_64-unknown-none' 'make svsm.bin' 'Copy svsm.bin back and record its checksum.' ;;
 *) echo 'Usage: scripts/build.sh {qemu|edk2|guest|initrd|svsm-help}' >&2; exit 2 ;;
esac
