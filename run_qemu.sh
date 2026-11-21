#!/bin/bash
HYC3_BASE_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
export HYC3_BASE_DIR
set -o pipefail
"$HYC3_BASE_DIR/scripts/preflight.sh"

QEMU=$HYC3_BASE_DIR/qemu-tdp/build/qemu-system-x86_64
KERNEL=$HYC3_BASE_DIR/vmlinuz-5.14.0-710.el9.x86_64
DRACUT_INITRD=$HYC3_BASE_DIR/initramfs-5.14.0-710.el9.x86_64.img
FULLBOOT_INITRD=$HYC3_BASE_DIR/hyc3-fullboot-initramfs.img
BRINGUP_INITRD=$HYC3_BASE_DIR/hyc3-bringup-initramfs.img
L2BIOS=$HYC3_BASE_DIR/edk2-clean/Build/OvmfX64/DEBUG_GCC5/FV/OVMF.fd
BIOS=$HYC3_BASE_DIR/td-partitioning-svsm/svsm.bin
L2_ROOT_DISK1=${HYC3_L2_ROOT_DISK1:-$HYC3_BASE_DIR/hyc3-l2-1-rootfs.qcow2}
L2_ROOT_DISK2=${HYC3_L2_ROOT_DISK2:-$HYC3_BASE_DIR/hyc3-l2-2-rootfs.qcow2}
L2_1_SSH_PORT=${HYC3_L2_1_SSH_PORT:-10021}
L2_2_SSH_PORT=${HYC3_L2_2_SSH_PORT:-10022}
HYC3_L2_1_NET=${HYC3_L2_1_NET:-auto}
HYC3_L2_2_NET=${HYC3_L2_2_NET:-0}
HYC3_L2_1_SSH=${HYC3_L2_1_SSH:-auto}
HYC3_EXPOSE_L2_2_SERIAL=${HYC3_EXPOSE_L2_2_SERIAL:-1}
HYC3_AUTOTEST=${HYC3_AUTOTEST:-1}
DRIVE_CACHE=${HYC3_DRIVE_CACHE:-writeback}
LOG=$HYC3_BASE_DIR/startup.log
L2_1_SERIAL_LOG=${LOG}
L2_1_SERIAL_ALIAS=$HYC3_BASE_DIR/l2-1-serial.log
L2_1_SERIAL_SOCK=$HYC3_BASE_DIR/l2-1-serial.sock
L2_2_SERIAL_LOG=$HYC3_BASE_DIR/l2-2-serial.log
L2_2_SERIAL_SOCK=$HYC3_BASE_DIR/l2-2-serial.sock
MODE=${HYC3_MODE:-auto}
ROOTFS_MODE=${HYC3_ROOTFS:-virtio}
NUM_L2_VMS=${HYC3_NUM_L2_VMS:-2}
# Default to the qcow2-backed HYC3 shell. The full systemd path is still
# available with HYC3_REAL_INIT=1, but it currently stalls after hostname
# because the TDP/SVSM interrupt/timer path is not stable enough for the full
# distro boot.
HYC3_REAL_INIT=${HYC3_REAL_INIT:-0}
case "${ROOTFS_MODE}" in
  1|on|yes|true) ROOTFS_MODE=virtio ;;
  0|off|no|false) ROOTFS_MODE=off ;;
esac

if [ "${MODE}" = "auto" ]; then
  # HYC3 L2-on-SVSM cannot reliably boot the large distro dracut image: the
  # TDX EFI stub converts the initrd via thousands of 4K MapGPA calls before
  # Linux prints anything, which looks like a hang and can starve the other L2.
  # Use the small HYC3 initramfs as the rootfs handoff loader by default; it
  # mounts the qcow2-backed root device and exposes a real-rootfs shell.
  # Set HYC3_REAL_INIT=1 to hand off to the distro's /sbin/init.
  MODE=bringup
fi

if [ -n "${HYC3_DISK+x}" ]; then
  DISK=${HYC3_DISK}
elif [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ]; then
  DISK=${ROOTFS_MODE}
else
  DISK=none
fi

if [ ! -f "${L2_ROOT_DISK1}" ]; then
  L2_ROOT_DISK1=$HYC3_BASE_DIR/CentOS-Stream-GenericCloud-9-latest.x86_64.qcow2
fi
if [ ! -f "${L2_ROOT_DISK2}" ]; then
  L2_ROOT_DISK2=$HYC3_BASE_DIR/CentOS-Stream-GenericCloud-9-latest-2.x86_64.qcow2
fi

# The two-L2 qcow2 rootfs path relies on the SVSM SIRTE single-global-drain
# fix.  Without it L2-2 can consume VM1's shared MSI completion ring and L2-1
# hangs on later directory I/O such as `ls /home` or `mkdir`.
if [ "${NUM_L2_VMS}" != "1" ] && [ "${ROOTFS_MODE}" != "off" ] && [ "${HYC3_ALLOW_OLD_SVSM:-0}" != "1" ]; then
  if ! grep -a -q "queued single owner global drain" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 single-owner SIRTE global-drain fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/sirte.rs and kernel/src/tdx/vcpu.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because VM2 can still drain VM1's shared completion ring and starve L2-1 /home or mkdir I/O." >&2
    exit 2
  fi
  if ! grep -a -q "gated shared eager alias" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 gated eager shared-MapGPA alias fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vmexit.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because L2-1 can still hang on /home or mkdir without this fix." >&2
    exit 2
  fi
  if ! grep -a -q "preserve staged external vector" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 staged-external dummy-vmentry preserve fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vcpu.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because dummy VM-entry can still requeue virtio-blk MSI vector32 forever and hang L2-1 rootfs I/O." >&2
    exit 2
  fi
  if ! grep -a -q "fixed high notify vector" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 fixed high SIRTE notify-vector fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/tdp.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because the host SIRTE notify vector can collide with guest MSI vector 32 and hang L2-1 rootfs I/O." >&2
    exit 2
  fi
  if ! grep -a -q "clear blocking ISR" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 blocking-ISR rescue for low-vector MSI completions." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because stale vLAPIC ISR/PPR state can still block L2-1 /home I/O." >&2
    exit 2
  fi
  if ! grep -a -q "rescue blocked pending vector" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 pending-IRR/PPR rescue for low-vector MSI completions." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because an already-pending L2-1 block completion can still be blocked by stale ISR/PPR state." >&2
    exit 2
  fi
  if ! grep -a -q "auto-EOI edge block MSI vector32" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 vector32 software auto-EOI fix for virtio-blk completions." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because L2-1 can still hang on /home or mkdir when vector32 remains in vLAPIC ISR/PPR." >&2
    exit 2
  fi
  if ! grep -a -q "tracking timer-origin ISR" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 vLAPIC timer-origin tracking fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because virtio MSI vector 32 can be mistaken for a stale firmware/LAPIC timer vector and hang L2 boot or rootfs I/O." >&2
    exit 2
  fi
  if ! grep -a -q "defer timer for pending external without rearm" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 pending-external-before-timer vLAPIC fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because the old timer-defer path can rearm host LAPIC timers while virtio MSI vector 32 is pending and hang L2-1 /home I/O." >&2
    exit 2
  fi
  if ! grep -a -q "staged external clears blocking ISR" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 staged-external ISR rescue fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs and kernel/src/tdx/vcpu.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because staged vector32 can remain blocked by high-priority timer ISR and hang L2-1 /home I/O." >&2
    exit 2
  fi
  if ! grep -a -q "prefer block MSI vector 32 over timer" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 block-MSI-before-timer delivery fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs and kernel/src/tdx/virq.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because LAPIC timer/high software vectors can still starve virtio MSI vector 32 and hang L2-1 /home I/O." >&2
    exit 2
  fi
  if grep -a -q "immediate requeue block MSI vector32 after dummy vmentry" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} still contains the old HYC3 immediate dummy-vmentry block-MSI requeue path." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vcpu.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because that path can requeue virtio-blk MSI vector32 forever and hang L2-1 rootfs I/O." >&2
    exit 2
  fi
  if ! grep -a -q "clear stale block MSI ISR vector32 before redelivery" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 stale block-MSI ISR cleanup fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because a stale vector32 ISR can still block later L2-1 rootfs I/O completions." >&2
    exit 2
  fi
  if ! grep -a -q "staged clear stale block MSI ISR vector32 before redelivery" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 staged stale block-MSI ISR cleanup fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vlapic.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because a VM-entry-staged vector32 can still leave stale ISR state and hang L2-1 rootfs I/O." >&2
    exit 2
  fi
  if ! grep -a -q "preserve staged block MSI vector32" "${BIOS}" 2>/dev/null || ! grep -a -q "requeue non-block staged external vector=" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 dummy-vmentry staged-interrupt split fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vcpu.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because non-block timer/software vectors can remain staged across dummy vmentry and stall early Linux before /init." >&2
    exit 2
  fi
  if ! grep -a -q "dest4 vector32 stays with VM2 only" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 no-duplicate dest4 MSI routing fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/sirte.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because raw_dest=4 completions can still be spuriously injected into L2-1 and poison rootfs I/O progress." >&2
    exit 2
  fi
  if [ "${HYC3_AUTOTEST:-1}" = "1" ] && ! grep -a -q "HYC3-SHARED: staged fixed shared GPA registered" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 staged fixed shared-page registration fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/vmexit.rs." >&2
    echo "run_qemu.sh: refusing autotest because an older VM1/VM2 eager shared-page alias can stop L2-2 after EFI stub." >&2
    exit 2
  fi
  if ! grep -a -q "firmware-stage PCI hide" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 firmware-stage PCI-device isolation fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/ioreq.rs." >&2
    echo "run_qemu.sh: refusing the default two-L2 rootfs boot because OVMF can reset the other L2's virtio-blk BAR before Linux enables the late PCI filter, which hangs L2-1 /home I/O." >&2
    exit 2
  fi
  if grep -a -q "L2-2) first; L2-1 waits for HYC3 WAIT_INPUT" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} still contains the old HYC3 L2-2-first boot ordering." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/percpu.rs." >&2
    echo "run_qemu.sh: refusing this boot because L2-2 can stall in OVMF xAPIC timer setup before its serial console exists, while L2-1 waits forever." >&2
    exit 2
  fi
  if ! grep -a -q "Assigning APIC-ID .* to VM-0 (L2-1) first" "${BIOS}" 2>/dev/null || ! grep -a -q "after L2-1 readiness" "${BIOS}" 2>/dev/null; then
    echo "run_qemu.sh: ${BIOS} is missing the HYC3 L2-1-first boot ordering fix." >&2
    echo "run_qemu.sh: rebuild/copy td-partitioning-svsm/svsm.bin after copying kernel/src/tdx/percpu.rs." >&2
    echo "run_qemu.sh: refusing this boot because a stale percpu.rs still starts L2-2 first and prevents both L2 consoles from coming up." >&2
    exit 2
  fi
fi

case "${MODE}" in
  full)
    INITRD=${HYC3_INITRD:-${FULLBOOT_INITRD}}
    COMMON_APPEND="rw idle=poll memmap=2M\$8M nox2apic nolapic_timer clocksource=tsc tsc=reliable no_timer_check i8042.noaux i8042.nokbd i8042.nomux i8042.nopnp pci=nomsi irqaffinity=0 nmi_watchdog=0 nowatchdog nosmp nr_cpus=1 maxcpus=1 rootwait rootfstype=xfs rd.driver.pre=vmw_pvscsi rd.driver.pre=sd_mod scsi_mod.scan=sync rd.luks=0 rd.md=0 rd.dm=0 rd.lvm=0 rd.neednet=0 ip=off loglevel=7 systemd.show_status=1 systemd.log_target=kmsg systemd.journald.forward_to_console=1"
    ;;

  dracut)
    INITRD=${HYC3_INITRD:-${DRACUT_INITRD}}
    COMMON_APPEND="rw idle=poll memmap=2M\$8M nox2apic nolapic_timer clocksource=tsc tsc=reliable no_timer_check i8042.noaux i8042.nokbd i8042.nomux i8042.nopnp pci=nomsi irqaffinity=0 nmi_watchdog=0 nowatchdog nosmp nr_cpus=1 maxcpus=1 rootwait rootfstype=xfs rd.driver.pre=vmw_pvscsi rd.driver.pre=sd_mod scsi_mod.scan=sync rd.luks=0 rd.md=0 rd.dm=0 rd.lvm=0 rd.neednet=0 ip=off loglevel=7 systemd.show_status=1 systemd.log_target=kmsg systemd.journald.forward_to_console=1"
    ;;
  bringup)
    INITRD=${HYC3_INITRD:-${BRINGUP_INITRD}}
    COMMON_APPEND="rw rdinit=/init idle=poll memmap=2M\$8M nox2apic nolapic_timer clocksource=tsc tsc=reliable no_timer_check i8042.noaux i8042.nokbd i8042.nomux i8042.nopnp pci=nomsi irqaffinity=0 nmi_watchdog=0 nowatchdog nosmp nr_cpus=1 maxcpus=1 rd.driver.pre=vmw_pvscsi rd.driver.pre=sd_mod scsi_mod.scan=sync rd.luks=0 rd.md=0 rd.dm=0 rd.lvm=0 rd.neednet=0 ip=off loglevel=7 systemd.log_target=kmsg systemd.log_level=debug systemd.show_status=1 systemd.journald.forward_to_console=1 rd.info rd.debug rd.shell rd.retry=30"
    ;;
  *)
    echo "run_qemu.sh: invalid HYC3_MODE=${MODE}; expected auto, full, bringup, or dracut" >&2
    exit 2
    ;;
esac

if [ -z "${HYC3_INITRD2+x}" ] && [ "${MODE}" = "bringup" ]; then
  INITRD2=$HYC3_BASE_DIR/hyc3-bringup-initramfs-l2-2.img
  /bin/cp -f "${INITRD}" "${INITRD2}"
else
  INITRD2=${HYC3_INITRD2:-${INITRD}}
fi

if [ "${HYC3_PAD_INITRD:-1}" = "1" ]; then
  pad_initrd_for_efi() {
    local src="$1"
    local dst="$2"
    local size aligned

    if [ ! -f "${src}" ]; then
      echo "run_qemu.sh: initrd not found: ${src}" >&2
      exit 2
    fi
    if gzip -t "${src}" >/dev/null 2>&1; then
      gzip -dc "${src}" > "${dst}"
      initrd_format=gzip-newc
    else
      /bin/cp -f "${src}" "${dst}"
      initrd_format=raw
    fi
    size=$(stat -c %s "${dst}")
    aligned=$(( ((size + 4095) / 4096) * 4096 ))
    if [ "${aligned}" -ne "${size}" ]; then
      truncate -s "${aligned}" "${dst}"
    fi
    echo "run_qemu.sh: prepared initrd ${src} -> ${dst} format=${initrd_format} size=${size} aligned=${aligned}" | tee -a "${LOG}"
  }

  INITRD_RUN=$HYC3_BASE_DIR/.hyc3-initrd-l2-1.img
  INITRD2_RUN=$HYC3_BASE_DIR/.hyc3-initrd-l2-2.img
  pad_initrd_for_efi "${INITRD}" "${INITRD_RUN}"
  # Keep both L2 initrds in the same raw newc + page-aligned form. L2-1
  # already uses this path reliably; L2-2's compressed initrd2 repeatedly
  # reached Linux as an invalid initramfs stream in the HYC3 TDP EFI handoff.
  if [ "${HYC3_PAD_INITRD2:-1}" = "1" ]; then
    pad_initrd_for_efi "${INITRD2}" "${INITRD2_RUN}"
    INITRD2=${INITRD2_RUN}
  else
    echo "run_qemu.sh: using compressed initrd2 ${INITRD2} without EFI padding" | tee -a "${LOG}"
  fi
  INITRD=${INITRD_RUN}
fi

case "${DISK}" in
  none) ROOTDEV=/dev/ram0 ;;
  pvscsi) ROOTDEV=/dev/sda ;;
  virtio) ROOTDEV=/dev/vda ;;
  ahci) ROOTDEV=/dev/sda ;;
  *)
    echo "run_qemu.sh: invalid HYC3_DISK=${DISK}; expected none, pvscsi, virtio, or ahci" >&2
    exit 2
    ;;
esac

KERNEL_ROOTDEV=${ROOTDEV}
if [ "${MODE}" = "bringup" ]; then
  # The custom rdinit owns qcow2 discovery/mount. Keep the kernel's own
  # root-device fallback away from /dev/sdX so block open retries do not
  # obscure the handoff path or disconnect the L2-1 console.
  KERNEL_ROOTDEV=/dev/ram0
fi

L2_1_ROOTFS_MODE=off
L2_2_ROOTFS_MODE=off
if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ]; then
  L2_1_ROOTFS_MODE=${ROOTFS_MODE}
  L2_2_ROOTFS_MODE=${HYC3_L2_2_ROOTFS:-${ROOTFS_MODE}}
  case "${L2_2_ROOTFS_MODE}" in
    1|on|yes|true) L2_2_ROOTFS_MODE=${ROOTFS_MODE} ;;
    0|off|no|false) L2_2_ROOTFS_MODE=off ;;
  esac
fi

# Full qcow2 handoff needs ACPI/MADT so Linux can set up the local APIC
# clockevent. The old no-ACPI fallback reaches /init, but leaves LOC at zero and
# stalls RCU while virtio_blk registers the disk.
if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ -z "${HYC3_LEGACY_NOACPI+x}" ]; then
  HYC3_LEGACY_NOACPI=0
fi

# Rootfs-backed virtio/PVSCSI needs MSI/MSI-X in this TDX/SVSM stack. The
# legacy INTx experiment lets L2 reach /init but virtio-blk rejects probe with
# -EINVAL, so keep INTx as an explicit opt-in only.
if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ -z "${HYC3_ENABLE_MSI+x}" ]; then
  case "${DISK}" in
    pvscsi|virtio) HYC3_ENABLE_MSI=1 ;;
    *) HYC3_ENABLE_MSI=0 ;;
  esac
fi

if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ "${HYC3_VIRTIO_INTX:-0}" = "1" ] && [ "${HYC3_ALLOW_UNSTABLE_INTX:-0}" != "1" ]; then
  echo "run_qemu.sh: ignoring HYC3_VIRTIO_INTX=1 in rootfs mode; set HYC3_ALLOW_UNSTABLE_INTX=1 to force this unstable INTx experiment" >&2
  HYC3_VIRTIO_INTX=0
fi

# Keep x2APIC disabled by default for the rootfs handoff while the SVSM x2APIC
# timer path is still noisy. ACPI stays enabled, so Linux still gets MADT/LAPIC
# clockevent setup through the xAPIC path.
if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ -z "${HYC3_ALLOW_X2APIC+x}" ]; then
  HYC3_ALLOW_X2APIC=0
fi
if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ -z "${HYC3_ENABLE_IOAPIC+x}" ]; then
  # MSI/MSI-X storage completions target the local APIC and do not need IOAPIC.
  # Keeping IOAPIC off avoids the current TDP/SVSM early-boot stall while ACPI
  # still provides MADT/LAPIC timer setup.
  HYC3_ENABLE_IOAPIC=0
fi

# Keep the tiny bring-up shell conservative, but enable the LAPIC timer for
# rootfs mode. Full storage/module probing needs timer ticks to make progress.
# Use HYC3_FORCE_NO_LAPIC_TIMER=1 only as a regression escape hatch.
if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ -z "${HYC3_FORCE_NOSMP+x}" ]; then
  HYC3_FORCE_NOSMP=1
fi

if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ]; then
  COMMON_APPEND="${COMMON_APPEND} nohz=off highres=off elevator=none"
  # Full qcow2 handoff needs ACPI/MADT for LAPIC timer setup, but not HPET.
  # Use HYC3_LEGACY_NOACPI=1 only to reproduce the old minimal initramfs shell path.
  if [ "${HYC3_LEGACY_NOACPI:-0}" = "1" ]; then
    COMMON_APPEND="${COMMON_APPEND} acpi=off pci=noacpi noapic nohpet hpet=disable"
  else
    COMMON_APPEND="${COMMON_APPEND} nohpet hpet=disable"
  fi
  if [ "${HYC3_ENABLE_IOAPIC:-0}" != "1" ]; then
    # Keep ACPI tables/MADT available for LAPIC setup, but do not let Linux use
    # ACPI PCI IRQ routing while IO-APIC is disabled. Q35 reports PCI GSIs in
    # the IO-APIC range, and that path currently stalls before the initramfs.
    COMMON_APPEND="${COMMON_APPEND} noapic acpi=noirq"
  fi
fi

if [ "${HYC3_ENABLE_MSI:-0}" = "1" ]; then
  COMMON_APPEND="${COMMON_APPEND//pci=nomsi /}"
fi

if [ "${HYC3_ALLOW_X2APIC:-0}" = "1" ]; then
  COMMON_APPEND="${COMMON_APPEND//nox2apic /}"
fi

if [ "${HYC3_ALLOW_LAPIC_TIMER:-0}" = "1" ] || { [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ "${HYC3_FORCE_NO_LAPIC_TIMER:-0}" != "1" ]; }; then
  COMMON_APPEND="${COMMON_APPEND//nolapic_timer /}"
fi

if [ "${HYC3_ENABLE_IOAPIC:-0}" = "1" ]; then
  COMMON_APPEND="${COMMON_APPEND//noapic /}"
fi

if [ "${HYC3_ENABLE_IOAPIC:-0}" = "1" ] || { [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ "${HYC3_FORCE_NOSMP:-0}" != "1" ]; }; then
  COMMON_APPEND="${COMMON_APPEND//nosmp /}"
fi

if [ "${HYC3_ENABLE_NET:-auto}" = "auto" ]; then
  if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ]; then
    HYC3_ENABLE_NET=1
  else
    HYC3_ENABLE_NET=0
  fi
fi

if [ "${DISK}" = "virtio" ]; then
  COMMON_APPEND="${COMMON_APPEND//rd.driver.pre=vmw_pvscsi /}"
  COMMON_APPEND="${COMMON_APPEND} rd.driver.pre=virtio rd.driver.pre=virtio_ring rd.driver.pre=virtio_pci rd.driver.pre=virtio_blk"
fi

if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ] && [ "${HYC3_REAL_INIT:-1}" = "1" ]; then
  COMMON_APPEND="${COMMON_APPEND} hyc3_real_init=systemd systemd.unit=multi-user.target"
fi

if [ -n "${HYC3_APPEND_EXTRA:-}" ]; then
  COMMON_APPEND="${COMMON_APPEND} ${HYC3_APPEND_EXTRA}"
fi

QEMU_NETDEV_HELP="${HYC3_QEMU_NETDEV_HELP:-}"
qemu_has_netdev_user() {
  if [ -z "${QEMU_NETDEV_HELP}" ]; then
    QEMU_NETDEV_HELP="$(${QEMU} -netdev help 2>&1 || true)"
  fi
  printf '%s\n' "${QEMU_NETDEV_HELP}" | grep -Eq '(^|[[:space:]])user($|[[:space:]])'
}

if [ "${HYC3_L2_1_NET}" = "auto" ]; then
  HYC3_L2_1_NET=${HYC3_ENABLE_NET:-0}
fi
if [ "${HYC3_L2_2_NET}" = "auto" ]; then
  # Production policy: L2-2 is a private worker and gets no externally reachable NIC unless explicitly requested.
  HYC3_L2_2_NET=0
fi
if [ "${HYC3_L2_1_SSH}" = "auto" ]; then
  HYC3_L2_1_SSH=${HYC3_L2_1_NET:-0}
fi

if { [ "${HYC3_L2_1_NET:-0}" = "1" ] || [ "${HYC3_L2_2_NET:-0}" = "1" ]; } && ! qemu_has_netdev_user; then
  if [ "${HYC3_REQUIRE_NET:-0}" = "1" ]; then
    echo "run_qemu.sh: QEMU lacks user/slirp netdev; install libslirp-devel and rebuild qemu-tdp, or set HYC3_ENABLE_NET=0" >&2
    exit 2
  fi
  echo "run_qemu.sh: warning: QEMU lacks user/slirp netdev; disabling SSH port-forward network for this run" >&2
  HYC3_ENABLE_NET=0
  HYC3_L2_1_NET=0
  HYC3_L2_2_NET=0
  HYC3_L2_1_SSH=0
fi

APPEND1_EXTRA=""
APPEND2_EXTRA=""
if [ "${L2_1_ROOTFS_MODE}" != "off" ]; then
  APPEND1_EXTRA="${APPEND1_EXTRA} hyc3_rootfs=${L2_1_ROOTFS_MODE} hyc3_rootdev=${ROOTDEV}"
fi
if [ "${L2_2_ROOTFS_MODE}" != "off" ]; then
  APPEND2_EXTRA="${APPEND2_EXTRA} hyc3_rootfs=${L2_2_ROOTFS_MODE} hyc3_rootdev=${ROOTDEV}"
fi
if [ "${HYC3_AUTOTEST:-1}" = "1" ]; then
  APPEND1_EXTRA="${APPEND1_EXTRA} hyc3_autotest=1"
  APPEND2_EXTRA="${APPEND2_EXTRA} hyc3_autotest=1"
else
  APPEND1_EXTRA="${APPEND1_EXTRA} hyc3_autotest=0"
  APPEND2_EXTRA="${APPEND2_EXTRA} hyc3_autotest=0"
fi
if [ "${HYC3_L2_1_SSH:-0}" = "1" ]; then
  APPEND1_EXTRA="${APPEND1_EXTRA} hyc3_l2_1_ssh=1"
fi

APPEND1="earlycon=uart8250,io,0x3f8,115200 console=ttyS0 root=${KERNEL_ROOTDEV} ${COMMON_APPEND}${APPEND1_EXTRA}"
APPEND2="earlycon=uart8250,io,0x2f8,115200 console=ttyS1 root=${KERNEL_ROOTDEV} ${COMMON_APPEND}${APPEND2_EXTRA}"

: > "${LOG}"
rm -f "${L2_1_SERIAL_ALIAS}"
ln -s "${LOG}" "${L2_1_SERIAL_ALIAS}"
: > "${L2_2_SERIAL_LOG}"
rm -f "${L2_1_SERIAL_SOCK}" "${L2_2_SERIAL_SOCK}"

run_qemu_cmd() {
  local disk_args=()
  local net_args=()
  local serial_args=()

  if [ "${HYC3_ENABLE_NET:-auto}" = "auto" ]; then
    if [ -n "${ROOTFS_MODE}" ] && [ "${ROOTFS_MODE}" != "off" ]; then
      HYC3_ENABLE_NET=1
    else
      HYC3_ENABLE_NET=0
    fi
  fi

  serial_args=(
    -chardev socket,id=l2serial0,path=${L2_1_SERIAL_SOCK},server=on,wait=off,logfile=${L2_1_SERIAL_LOG},logappend=on
    -device isa-serial,chardev=l2serial0,index=0
  )
  if [ "${HYC3_EXPOSE_L2_2_SERIAL:-1}" = "1" ]; then
    serial_args+=(
      -chardev socket,id=l2serial1,path=${L2_2_SERIAL_SOCK},server=on,wait=off,logfile=${L2_2_SERIAL_LOG},logappend=on
      -device isa-serial,chardev=l2serial1,index=1
    )
  fi

  if [ "${HYC3_L2_1_NET:-0}" = "1" ]; then
    net_args+=(
      -netdev user,id=hyc3net1,hostfwd=tcp:127.0.0.1:${L2_1_SSH_PORT}-:22
      -device virtio-net-pci,netdev=hyc3net1,id=hyc3netdev1,addr=0x5,disable-legacy=on,iommu_platform=on,ioeventfd=off,vectors=3,romfile=
    )
  fi
  if [ "${HYC3_L2_2_NET:-0}" = "1" ]; then
    net_args+=(
      -netdev user,id=hyc3net2,hostfwd=tcp:127.0.0.1:${L2_2_SSH_PORT}-:22
      -device virtio-net-pci,netdev=hyc3net2,id=hyc3netdev2,addr=0x6,disable-legacy=on,iommu_platform=on,ioeventfd=off,vectors=3,romfile=
    )
  fi

  case "${DISK}" in
    none)
      disk_args=()
      ;;
    pvscsi)
      disk_args=(
        -drive if=none,id=l2disk1,file=${L2_ROOT_DISK1},format=qcow2,cache=${DRIVE_CACHE}
        -device pvscsi,id=hyc3scsi1,addr=0x3,use_msg=0,romfile=
        -device scsi-hd,drive=l2disk1,bus=hyc3scsi1.0,scsi-id=0,lun=0
        -drive if=none,id=l2disk2,file=${L2_ROOT_DISK2},format=qcow2,cache=${DRIVE_CACHE}
        -device pvscsi,id=hyc3scsi2,addr=0x4,use_msg=0,romfile=
        -device scsi-hd,drive=l2disk2,bus=hyc3scsi2.0,scsi-id=0,lun=0
      )
      ;;
    virtio)
      local virtio_props
      if [ "${HYC3_VIRTIO_INTX:-0}" = "1" ]; then
        virtio_props="disable-legacy=on,iommu_platform=on,ioeventfd=off,vectors=0,modern-pio-notify=on,queue-size=128,config-wce=off,write-cache=off,discard=off,write-zeroes=off,romfile="
      else
        virtio_props="disable-legacy=on,iommu_platform=on,ioeventfd=off,vectors=2,queue-size=128,config-wce=off,write-cache=off,discard=off,write-zeroes=off,romfile="
      fi
      disk_args=(
        -drive if=none,id=l2disk1,file=${L2_ROOT_DISK1},format=qcow2,cache=${DRIVE_CACHE}
        -device virtio-blk-pci,drive=l2disk1,id=hyc3blk1,addr=0x3,${virtio_props}
        -drive if=none,id=l2disk2,file=${L2_ROOT_DISK2},format=qcow2,cache=${DRIVE_CACHE}
        -device virtio-blk-pci,drive=l2disk2,id=hyc3blk2,addr=0x4,${virtio_props}
      )
      ;;
    ahci)
      disk_args=(
        -drive if=none,id=l2disk1,file=${L2_ROOT_DISK1},format=qcow2,cache=${DRIVE_CACHE}
        -device ich9-ahci,id=hyc3ahci1,addr=0x3,romfile=
        -device ide-hd,drive=l2disk1,bus=hyc3ahci1.0
        -drive if=none,id=l2disk2,file=${L2_ROOT_DISK2},format=qcow2,cache=${DRIVE_CACHE}
        -device ich9-ahci,id=hyc3ahci2,addr=0x4,romfile=
        -device ide-hd,drive=l2disk2,bus=hyc3ahci2.0
      )
      ;;
  esac

  sudo env "HYC3_SERIAL_IO_DEBUG=${HYC3_SERIAL_IO_DEBUG:-0}" "HYC3_PVSCSI_DEBUG=${HYC3_PVSCSI_DEBUG:-0}" "HYC3_QEMU_DEBUG=${HYC3_QEMU_DEBUG:-0}" "$QEMU" \
    -m 4G \
    -smp 4 \
    -cpu host,-hle,-rtm \
    -nodefaults \
    -machine q35,sata=off,accel=kvm,kernel_irqchip=split,l2bios=${L2BIOS},confidential-guest-support=tdx \
    -object "{\"qom-type\":\"tdx-guest\",\"id\":\"tdx\",\"quote-generation-socket\":{\"type\":\"unix\",\"path\":\"/var/run/tdx-qgs/qgs.socket\"},\"num-l2-vms\":${NUM_L2_VMS},\"svsm\":true}" \
    -bios ${BIOS} \
    -kernel ${KERNEL} \
    -initrd ${INITRD} \
    -append "${APPEND1}" \
    -kernel2 ${KERNEL} \
    -initrd2 ${INITRD2} \
    -append2 "${APPEND2}" \
    "${disk_args[@]}" \
    "${net_args[@]}" \
    -display none \
    -vga none \
    -monitor none \
    "${serial_args[@]}" \
    -action reboot=shutdown \
    -action shutdown=pause \
    -action panic=pause \
    -action watchdog=pause
}

if [[ " ${COMMON_APPEND} " == *" nolapic_timer "* ]]; then
  EFFECTIVE_LAPIC_TIMER=0
else
  EFFECTIVE_LAPIC_TIMER=1
fi
if [[ " ${COMMON_APPEND} " == *" nosmp "* ]]; then
  EFFECTIVE_NOSMP=1
else
  EFFECTIVE_NOSMP=0
fi

echo "run_qemu.sh: mode=${MODE} num_l2_vms=${NUM_L2_VMS} disk=${DISK} rootfs=${ROOTFS_MODE:-off} l2_1_rootfs=${L2_1_ROOTFS_MODE} l2_2_rootfs=${L2_2_ROOTFS_MODE} real_init=${HYC3_REAL_INIT:-1} drive_cache=${DRIVE_CACHE} net=${HYC3_ENABLE_NET:-auto} l2_1_net=${HYC3_L2_1_NET:-0} l2_2_net=${HYC3_L2_2_NET:-0} l2_1_ssh=${HYC3_L2_1_SSH:-0} l2_2_serial=${HYC3_EXPOSE_L2_2_SERIAL:-1} autotest=${HYC3_AUTOTEST:-1} ssh_ports=${L2_1_SSH_PORT}/${L2_2_SSH_PORT} msi=${HYC3_ENABLE_MSI:-0} x2apic=${HYC3_ALLOW_X2APIC:-0} lapic_timer=${EFFECTIVE_LAPIC_TIMER} nosmp=${EFFECTIVE_NOSMP} ioapic=${HYC3_ENABLE_IOAPIC:-0} legacy_noacpi=${HYC3_LEGACY_NOACPI:-0} virtio_intx=${HYC3_VIRTIO_INTX:-0} initrd=${INITRD} initrd2=${INITRD2}" | tee -a "${LOG}"
echo "run_qemu.sh: L2-1 serial socket ${L2_1_SERIAL_SOCK}" | tee -a "${LOG}"
echo "run_qemu.sh: L2-1 serial log ${L2_1_SERIAL_LOG}" | tee -a "${LOG}"
echo "run_qemu.sh: L2-2 serial log ${L2_2_SERIAL_LOG}" | tee -a "${LOG}"
echo "run_qemu.sh: L2-2 serial socket ${L2_2_SERIAL_SOCK}" | tee -a "${LOG}"

if [ "${HYC3_ATTACH:-1}" = "1" ] && [ -t 0 ]; then
  RUN_LOG=$HYC3_BASE_DIR/run-wrapper.out
  CONSOLE_WAIT=${HYC3_CONSOLE_WAIT:-900}
  : > "${RUN_LOG}"
  echo "run_qemu.sh: launching QEMU in background; waiting up to ${CONSOLE_WAIT}s for L2-1 console..." | tee -a "${LOG}"
  run_qemu_cmd >>"${RUN_LOG}" 2>&1 &
  qemu_pid=$!
  cleanup() {
    kill "${qemu_pid}" 2>/dev/null || true
    pkill -P "${qemu_pid}" 2>/dev/null || true
    /usr/bin/python3 $HYC3_BASE_DIR/kill_hyc3_qemu.py >/dev/null 2>&1 || true
  }
  trap cleanup EXIT INT TERM

  for _ in $(seq 1 "${CONSOLE_WAIT}"); do
    if ! kill -0 "${qemu_pid}" 2>/dev/null; then
      echo "run_qemu.sh: QEMU exited before L2-1 console was ready" | tee -a "${LOG}"
      tail -80 "${RUN_LOG}" >&2 || true
      wait "${qemu_pid}"
      exit $?
    fi
    if [ "${L2_1_ROOTFS_MODE}" = "off" ]; then
      ready_re="hyc3-L2-1#|tdcall_module: HYC3 shared-memory \+1 test PASSED"
    else
      if [ "${HYC3_REAL_INIT:-1}" = "1" ]; then
        ready_re="Welcome to|login:|Started OpenSSH|Started OpenSSH server daemon|Reached target.*(Multi-User|Graphical)|Startup finished"
      else
        ready_re="HYC3-BRINGUP: real rootfs shell mode|HYC3 real qcow2 rootfs console ready"
      fi
    fi

    l2_2_ready_re=""
    if [ "${NUM_L2_VMS}" -gt 1 ] && [ "${HYC3_EXPOSE_L2_2_SERIAL:-1}" = "1" ]; then
      if [ "${L2_2_ROOTFS_MODE}" = "off" ]; then
        l2_2_ready_re="hyc3-L2-2#|tdcall_module: HYC3 shared-memory \+1 test PASSED"
      else
        if [ "${HYC3_REAL_INIT:-1}" = "1" ]; then
          l2_2_ready_re="Welcome to|login:|Started OpenSSH|Started OpenSSH server daemon|Reached target.*(Multi-User|Graphical)|Startup finished"
        else
          l2_2_ready_re="HYC3-BRINGUP: real rootfs shell mode|HYC3 real qcow2 rootfs console ready|tdcall_module: HYC3 shared-memory \+1 test PASSED"
        fi
      fi
    fi

    if grep -Eq "${ready_re}" "${LOG}" 2>/dev/null && { [ -z "${l2_2_ready_re}" ] || grep -Eq "${l2_2_ready_re}" "${L2_2_SERIAL_LOG}" 2>/dev/null; }; then
      echo "run_qemu.sh: L2-1 and L2-2 consoles reached the requested boot target." | tee -a "${LOG}"
      echo "run_qemu.sh: attached to L2-1 console. Press Enter if the prompt is not visible; use Ctrl-] to leave the console." | tee -a "${LOG}"
      $HYC3_BASE_DIR/connect_l2_1.sh
      console_status=$?
      echo "run_qemu.sh: L2-1 console disconnected; QEMU is still running." | tee -a "${LOG}"
      echo "run_qemu.sh: connect to L2-2 with $HYC3_BASE_DIR/connect_l2_2.sh, then press Enter if the prompt is not visible. Stop QEMU with $HYC3_BASE_DIR/stop_qemu.sh" | tee -a "${LOG}"
      if [ "${HYC3_KILL_ON_DETACH:-0}" = "1" ]; then
        cleanup
        wait "${qemu_pid}" 2>/dev/null || true
      else
        # The QEMU launcher is a background shell job. If this wrapper exits
        # after a normal console detach, bash may otherwise SIGHUP/TERM that job.
        disown "${qemu_pid}" 2>/dev/null || true
      fi
      trap - EXIT INT TERM
      exit ${console_status}
    fi
    sleep 1
  done

  echo "run_qemu.sh: timed out after ${CONSOLE_WAIT}s waiting for requested L2 console targets" | tee -a "${LOG}"
  tail -120 "${LOG}" >&2 || true
  cleanup
  wait "${qemu_pid}" 2>/dev/null || true
  exit 1
fi

run_qemu_cmd 2>&1 | tee -a "${LOG}"
qemu_status=${PIPESTATUS[0]}
echo "run_qemu.sh: qemu exited with status ${qemu_status}" | tee -a "${LOG}"
exit ${qemu_status}
