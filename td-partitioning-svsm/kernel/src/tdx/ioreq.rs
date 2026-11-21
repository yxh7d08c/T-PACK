// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Copyright (c) 2023 Intel Corporation.
//
// Author: Chuanxiao Dong <chuanxiao.dong@intel.com>

extern crate alloc;

use super::error::TdxError;
use super::gctx::{GuestCpuContext, GuestCpuGPRegCode};
use super::gmem::{copy_from_gpa, copy_to_gpa};
use super::instr_emul::InstrEmul;
use super::percpu::{this_vcpu, this_vcpu_mut};
use super::tdcall::{
    tdvmcall_io_read_16, tdvmcall_io_read_32, tdvmcall_io_read_8, tdvmcall_io_write_16,
    tdvmcall_io_write_32, tdvmcall_io_write_8, tdvmcall_mmio_read, tdvmcall_mmio_write,
};
use super::tdp::this_tdp;
use super::utils::TdpVmId;
use crate::address::GuestPhysAddr;
use crate::fw_cfg::FwCfg;
use crate::locking::RWLock;
use crate::mm::address_space::is_kernel_phys_addr_valid;
use crate::mm::guestmem::{gpa_is_shared, gpa_strip_c_bit};
use crate::mm::memory::is_guest_phys_addr_valid;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};

#[derive(Copy, Clone, Debug, PartialEq)]
pub enum AddrSize {
    ZeroByte,
    OneByte,
    TwoBytes,
    FourBytes = 4,
    EightBytes = 8,
}

pub fn copy_from_gpa_addrsize(gpa: GuestPhysAddr, size: AddrSize) -> Result<u64, TdxError> {
    match size {
        AddrSize::ZeroByte => Ok(0),
        AddrSize::OneByte => Ok(copy_from_gpa::<u8>(gpa)? as u64),
        AddrSize::TwoBytes => Ok(copy_from_gpa::<u16>(gpa)? as u64),
        AddrSize::FourBytes => Ok(copy_from_gpa::<u32>(gpa)? as u64),
        AddrSize::EightBytes => Ok(copy_from_gpa::<u64>(gpa)?),
    }
}

pub fn copy_to_gpa_addrsize(gpa: GuestPhysAddr, size: AddrSize, val: u64) -> Result<(), TdxError> {
    match size {
        AddrSize::ZeroByte => Ok(()),
        AddrSize::OneByte => copy_to_gpa::<u8>(gpa, val as u8),
        AddrSize::TwoBytes => copy_to_gpa::<u16>(gpa, val as u16),
        AddrSize::FourBytes => copy_to_gpa::<u32>(gpa, val as u32),
        AddrSize::EightBytes => copy_to_gpa::<u64>(gpa, val),
    }
}

impl AddrSize {
    pub fn mask(&self) -> u64 {
        match self {
            AddrSize::ZeroByte => 0,
            AddrSize::OneByte => (1 << 8) - 1,
            AddrSize::TwoBytes => (1 << 16) - 1,
            AddrSize::FourBytes => (1 << 32) - 1,
            AddrSize::EightBytes => u64::MAX,
        }
    }
}

const PCI_CONFIG_ADDRESS_PORT: u16 = 0x0CF8;
const PCI_CONFIG_DATA_START: u16 = 0x0CFC;
const PCI_CONFIG_DATA_END: u16 = 0x0CFF;
const PCI_ECAM_BASE: u64 = 0xE000_0000;
const PCI_ECAM_SIZE: u64 = 0x1000_0000;
const HYC3_L2_1_BLOCK_DEV: u8 = 3;
const HYC3_L2_2_BLOCK_DEV: u8 = 4;
const HYC3_L2_1_NET_DEV: u8 = 5;
const HYC3_L2_2_NET_DEV: u8 = 6;
const HYC3_FW_CFG_ACPI_TABLES: &str = "etc/acpi/tables";
const HYC3_MAX_ACPI_TABLES_SIZE: u32 = 128 * 1024;
const HYC3_ACPI_HEADER_CHECKSUM_OFFSET: usize = 9;
const HYC3_MADT_ENTRIES_OFFSET: usize = 44;
const HYC3_L2_ACTIVE_VCPUS_PER_VM: u64 = 1;

#[derive(Clone, Copy, Debug)]
struct Hyc3FwCfgPatch {
    offset: u32,
    value: u8,
}

static HYC3_FW_CFG_ACPI_SELECTOR: AtomicU32 = AtomicU32::new(0);
static HYC3_PCI_MMIO_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_PCI_HIDE_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_FW_CFG_ACPI_PATCHES: RWLock<Vec<Hyc3FwCfgPatch>> = RWLock::new(Vec::new());

fn hyc3_read_le_u32(buf: &[u8], offset: usize) -> Option<u32> {
    let bytes = buf.get(offset..offset.checked_add(4)?)?;
    Some(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

fn hyc3_find_acpi_table(buf: &[u8], signature: &[u8; 4]) -> Option<(usize, usize)> {
    let mut offset = 0usize;

    while offset.checked_add(8)? <= buf.len() {
        let sig = buf.get(offset..offset + 4)?;
        if sig.iter().all(|b| *b == 0) {
            break;
        }

        let length = hyc3_read_le_u32(buf, offset + 4)? as usize;
        if length < 8 || offset.checked_add(length)? > buf.len() {
            break;
        }

        if sig == &signature[..] {
            return Some((offset, length));
        }

        offset += length;
    }

    None
}

fn hyc3_build_acpi_patches(acpi_tables: &[u8]) -> Vec<Hyc3FwCfgPatch> {
    let mut patches = Vec::new();
    let (madt_offset, madt_length) = match hyc3_find_acpi_table(acpi_tables, b"APIC") {
        Some(found) => found,
        None => {
            log::warn!("HYC3: fw_cfg ACPI tables do not contain MADT");
            return patches;
        }
    };

    if madt_length <= HYC3_MADT_ENTRIES_OFFSET {
        log::warn!("HYC3: MADT is too small for L2 CPU filtering");
        return patches;
    }

    let mut madt = Vec::with_capacity(madt_length);
    for byte in &acpi_tables[madt_offset..madt_offset + madt_length] {
        madt.push(*byte);
    }

    let mut disabled_entries = 0u32;
    let mut entry_offset = HYC3_MADT_ENTRIES_OFFSET;
    while entry_offset + 2 <= madt.len() {
        let entry_type = madt[entry_offset];
        let entry_length = madt[entry_offset + 1] as usize;

        if entry_length < 2 || entry_offset + entry_length > madt.len() {
            break;
        }

        match entry_type {
            0 if entry_length >= 8 => {
                let apic_id = madt[entry_offset + 3];
                if apic_id >= HYC3_L2_ACTIVE_VCPUS_PER_VM as u8 {
                    for i in 0..4 {
                        madt[entry_offset + 4 + i] = 0;
                    }
                    disabled_entries += 1;
                }
            }
            9 if entry_length >= 16 => {
                let apic_id = u32::from_le_bytes([
                    madt[entry_offset + 4],
                    madt[entry_offset + 5],
                    madt[entry_offset + 6],
                    madt[entry_offset + 7],
                ]);
                if apic_id >= HYC3_L2_ACTIVE_VCPUS_PER_VM as u32 {
                    for i in 0..4 {
                        madt[entry_offset + 8 + i] = 0;
                    }
                    disabled_entries += 1;
                }
            }
            _ => {}
        }

        entry_offset += entry_length;
    }

    if disabled_entries == 0 {
        log::warn!("HYC3: MADT has no CPU entries to disable for L2 guests");
        return patches;
    }

    madt[HYC3_ACPI_HEADER_CHECKSUM_OFFSET] = 0;
    let checksum = madt.iter().fold(0u8, |sum, byte| sum.wrapping_add(*byte));
    madt[HYC3_ACPI_HEADER_CHECKSUM_OFFSET] = 0u8.wrapping_sub(checksum);

    for (index, byte) in madt.iter().enumerate() {
        if *byte != acpi_tables[madt_offset + index] {
            patches.push(Hyc3FwCfgPatch {
                offset: (madt_offset + index) as u32,
                value: *byte,
            });
        }
    }

    log::info!(
        "HYC3: prepared {} fw_cfg MADT byte patches; disabled {} non-BSP L2 CPU entries",
        patches.len(),
        disabled_entries
    );
    patches
}

pub fn hyc3_init_l2_acpi_filter(fw_cfg: &FwCfg<'_>) {
    let file = match fw_cfg.file_selector(HYC3_FW_CFG_ACPI_TABLES) {
        Ok(file) => file,
        Err(e) => {
            log::warn!("HYC3: cannot find fw_cfg ACPI tables for L2 filtering: {:?}", e);
            return;
        }
    };

    if file.size() == 0 || file.size() > HYC3_MAX_ACPI_TABLES_SIZE {
        log::warn!(
            "HYC3: ignoring fw_cfg ACPI tables size {} for L2 filtering",
            file.size()
        );
        return;
    }

    let mut acpi_tables = Vec::with_capacity(file.size() as usize);
    fw_cfg.select(file.selector());
    for _ in 0..file.size() {
        acpi_tables.push(fw_cfg.read_char() as u8);
    }

    let patches = hyc3_build_acpi_patches(&acpi_tables);
    if patches.is_empty() {
        return;
    }

    let patch_count = patches.len();
    *HYC3_FW_CFG_ACPI_PATCHES.lock_write() = patches;
    HYC3_FW_CFG_ACPI_SELECTOR.store(file.selector() as u32, Ordering::Release);
    log::info!(
        "HYC3: L2 fw_cfg ACPI filter armed for selector 0x{:x} with {} byte patches",
        file.selector(),
        patch_count
    );
}

fn hyc3_fw_cfg_data_bytes(size: AddrSize) -> u32 {
    match size {
        AddrSize::OneByte => 1,
        AddrSize::TwoBytes => 2,
        AddrSize::FourBytes => 4,
        _ => 0,
    }
}

fn hyc3_patch_fw_cfg_data(selector: u16, offset: u32, data: u64, size: AddrSize) -> u64 {
    let acpi_selector = HYC3_FW_CFG_ACPI_SELECTOR.load(Ordering::Acquire);
    let bytes = hyc3_fw_cfg_data_bytes(size);

    if acpi_selector == 0 || selector as u32 != acpi_selector || bytes == 0 {
        return data;
    }

    let patches = HYC3_FW_CFG_ACPI_PATCHES.lock_read();
    if patches.is_empty() {
        return data;
    }

    let mut patched = data;
    for byte_index in 0..bytes {
        let byte_offset = offset.wrapping_add(byte_index);
        if let Some(patch) = patches.iter().find(|patch| patch.offset == byte_offset) {
            let shift = byte_index * 8;
            patched &= !(0xffu64 << shift);
            patched |= (patch.value as u64) << shift;
        }
    }

    patched
}
fn pio_touches_pci_config_data(port: u16, size: AddrSize) -> bool {
    let bytes = match size {
        AddrSize::OneByte => 1,
        AddrSize::TwoBytes => 2,
        AddrSize::FourBytes => 4,
        _ => 0,
    };
    if bytes == 0 {
        return false;
    }

    let end = port.saturating_add(bytes - 1);
    port <= PCI_CONFIG_DATA_END && end >= PCI_CONFIG_DATA_START
}

fn hyc3_is_pci_mmio_window(addr: u64) -> bool {
    (0x8000_0000..0xe000_0000).contains(&addr)
        || (0xf000_0000..0xfec0_0000).contains(&addr)
        || (0x1c000_0000..0x9c000_0000).contains(&addr)
}

fn hyc3_pci_bdf_hidden_for_vm(vm_id: TdpVmId, bus: u8, device: u8, function: u8) -> bool {
    // HYC3: hide the peer L2 devices from firmware too, not just after Linux
    // writes TDCS_NOTIFY_ENABLES.  Otherwise one OVMF instance can reset the
    // other VM's virtio-blk BAR while that peer already has its XFS rootfs
    // mounted, which leaves later directory I/O stuck in __xfs_buf_submit.
    if bus != 0 || function != 0 {
        return false;
    }

    let hidden = match vm_id.index() {
        0 => device == HYC3_L2_2_BLOCK_DEV || device == HYC3_L2_2_NET_DEV,
        1 => device == HYC3_L2_1_BLOCK_DEV || device == HYC3_L2_1_NET_DEV,
        _ => false,
    };

    if hidden {
        let n = HYC3_PCI_HIDE_LOGS.fetch_add(1, Ordering::Relaxed);
        if n < 64 {
            log::warn!(
                "HYC3-PCI-HIDE: firmware-stage PCI hide VM{} bus={} dev={} fn={}",
                vm_id.num(),
                bus,
                device,
                function
            );
        }
    }

    hidden
}

fn hyc3_pci_cfg_hidden_for_vm(vm_id: TdpVmId, cfg_addr: u32) -> bool {
    if cfg_addr & 0x8000_0000 == 0 {
        return false;
    }

    let bus = ((cfg_addr >> 16) & 0xff) as u8;
    let device = ((cfg_addr >> 11) & 0x1f) as u8;
    let function = ((cfg_addr >> 8) & 0x07) as u8;
    hyc3_pci_bdf_hidden_for_vm(vm_id, bus, device, function)
}

fn hyc3_pci_ecam_hidden_for_vm(vm_id: TdpVmId, raw_addr: u64) -> bool {
    if raw_addr < PCI_ECAM_BASE || raw_addr >= PCI_ECAM_BASE + PCI_ECAM_SIZE {
        return false;
    }

    let offset = raw_addr - PCI_ECAM_BASE;
    let bus = ((offset >> 20) & 0xff) as u8;
    let device = ((offset >> 15) & 0x1f) as u8;
    let function = ((offset >> 12) & 0x07) as u8;
    hyc3_pci_bdf_hidden_for_vm(vm_id, bus, device, function)
}

impl TryFrom<usize> for AddrSize {
    type Error = TdxError;

    fn try_from(val: usize) -> Result<AddrSize, Self::Error> {
        match val {
            0 => Ok(AddrSize::ZeroByte),
            1 => Ok(AddrSize::OneByte),
            2 => Ok(AddrSize::TwoBytes),
            4 => Ok(AddrSize::FourBytes),
            8 => Ok(AddrSize::EightBytes),
            _ => Err(TdxError::InvalidAddrSize),
        }
    }
}

#[derive(Copy, Clone, Debug, PartialEq)]
pub enum IoDirection {
    Read,
    Write,
}

#[derive(Copy, Clone, Debug)]
pub struct IoOperation {
    io_dir: IoDirection,
    data: u64,
    retry: bool,
}

impl IoOperation {
    fn new(io_dir: IoDirection) -> Self {
        IoOperation {
            io_dir,
            data: 0,
            retry: false,
        }
    }

    pub fn is_read(&self) -> bool {
        self.io_dir == IoDirection::Read
    }

    pub fn get_read_data(&self) -> Option<u64> {
        if self.is_read() {
            Some(self.data)
        } else {
            None
        }
    }

    pub fn set_write_data(&mut self, val: u64) {
        if !self.is_read() {
            self.data = val;
        }
    }

    pub fn set_retry(&mut self) {
        self.retry = true;
    }
}

trait IoEmul {
    // Write the read data back
    fn emulate_read(&mut self, _io_op: &mut IoOperation) -> Result<(), TdxError> {
        Err(TdxError::IoEmulNotSupport)
    }

    // Get the write data
    fn emulate_write(&mut self, _io_op: &mut IoOperation) -> Result<(), TdxError> {
        Err(TdxError::IoEmulNotSupport)
    }

    // Get the IO operation size
    fn get_opsize(&self) -> AddrSize {
        AddrSize::ZeroByte
    }
}

struct IoInstrEmul<'a>(InstrEmul<'a>);

impl IoEmul for IoInstrEmul<'_> {
    fn emulate_read(&mut self, io_op: &mut IoOperation) -> Result<(), TdxError> {
        self.0.emulate_instr(io_op)
    }

    fn emulate_write(&mut self, io_op: &mut IoOperation) -> Result<(), TdxError> {
        self.0.emulate_instr(io_op)
    }

    // Get the IO operation size
    fn get_opsize(&self) -> AddrSize {
        self.0.get_opsize()
    }
}

struct PioEmul {
    ctx: &'static mut GuestCpuContext,
    size: AddrSize,
}

impl IoEmul for PioEmul {
    fn emulate_read(&mut self, io_op: &mut IoOperation) -> Result<(), TdxError> {
        let rax = self.ctx.get_gpreg(GuestCpuGPRegCode::Rax);
        let mask = self.get_opsize().mask();
        self.ctx
            .set_gpreg(GuestCpuGPRegCode::Rax, (rax & !mask) | (io_op.data & mask));
        Ok(())
    }

    fn emulate_write(&mut self, io_op: &mut IoOperation) -> Result<(), TdxError> {
        let rax = self.ctx.get_gpreg(GuestCpuGPRegCode::Rax);
        io_op.data = rax & self.get_opsize().mask();

        Ok(())
    }

    fn get_opsize(&self) -> AddrSize {
        self.size
    }
}

struct TdvmcallIoEmul {
    ctx: &'static mut GuestCpuContext,
    size: AddrSize,
}

impl IoEmul for TdvmcallIoEmul {
    fn emulate_read(&mut self, io_op: &mut IoOperation) -> Result<(), TdxError> {
        self.ctx.set_gpreg(GuestCpuGPRegCode::R11, io_op.data);
        Ok(())
    }

    fn emulate_write(&mut self, io_op: &mut IoOperation) -> Result<(), TdxError> {
        io_op.data = self.ctx.get_gpreg(GuestCpuGPRegCode::R15);
        Ok(())
    }

    fn get_opsize(&self) -> AddrSize {
        self.size
    }
}

#[derive(Clone, Copy, Debug)]
pub enum IoType {
    Mmio {
        addr: GuestPhysAddr,
        instr_len: usize,
    },
    TdvmcallMmio {
        addr: GuestPhysAddr,
        size: usize,
    },
    Pio {
        port: u16,
        size: usize,
    },
    StringPio {
        port: u16,
        instr_len: usize,
    },
    TdvmcallPio {
        port: u16,
        size: usize,
    },
}

pub struct IoReq {
    vm_id: TdpVmId,
    io_type: IoType,
    io_op: IoOperation,
}

impl IoReq {
    pub fn new(vm_id: TdpVmId, io_type: IoType, io_dir: IoDirection) -> IoReq {
        IoReq {
            vm_id,
            io_type,
            io_op: IoOperation::new(io_dir),
        }
    }

    fn emulate_mmio(&mut self, gpa: GuestPhysAddr, mut emul: impl IoEmul) -> Result<(), TdxError> {
        let size = emul.get_opsize();
        let addr = u64::from(gpa);
        let raw_addr = if gpa_is_shared(gpa) {
            u64::from(gpa_strip_c_bit(gpa))
        } else {
            addr
        };

        // Check if the requested GPA is private address. Not allow TDP guest accessing its private
        // memory or SVSM's memory through an MMIO request. PCI MMIO BARs live inside guest
        // physical address windows, so they must still be forwarded to the host device model.
        if !gpa_is_shared(gpa)
            && !hyc3_is_pci_mmio_window(raw_addr)
            && (is_guest_phys_addr_valid(gpa) || is_kernel_phys_addr_valid(gpa.to_host_phys_addr()))
        {
            return Err(TdxError::IoEmul(self.io_type));
        }

        if hyc3_is_pci_mmio_window(raw_addr) {
            let n = HYC3_PCI_MMIO_LOGS.fetch_add(1, Ordering::Relaxed);
            if n < 128 {
                log::warn!(
                    "HYC3-PCI-MMIO: VM{} {} addr={:#x} raw={:#x} size={:?}",
                    self.vm_id.num(),
                    if self.io_op.is_read() { "read" } else { "write" },
                    addr,
                    raw_addr,
                    size
                );
            }
        }

        let io_op = &mut self.io_op;
        let (vlapic_mmio_start, vlapic_mmio_end) = this_vcpu(self.vm_id).get_vlapic().mmio_range();
        let (vioapic_mmio_start, vioapic_mmio_end) =
            this_tdp(self.vm_id).get_vioapic().get_mmio_range();
        if io_op.is_read() {
            if hyc3_pci_ecam_hidden_for_vm(self.vm_id, raw_addr) {
                io_op.data = size.mask();
                emul.emulate_read(io_op)?;
                return Ok(());
            }

            io_op.data = match raw_addr {
                // MMIO belongs to virtual lapic is emulated by SVSM.
                v if (v >= vlapic_mmio_start && (v + size as u64) < vlapic_mmio_end) => {
                    this_vcpu(self.vm_id)
                        .get_vlapic()
                        .read_reg((raw_addr - vlapic_mmio_start) as u32)? as u64
                }
                //MMIO belonging to virtual IOAPIC
                v if v >= vioapic_mmio_start && (v + size as u64) < vioapic_mmio_end => {
                    this_tdp(self.vm_id)
                        .get_vioapic()
                        .mmio_read(v, size as u32)? as u64
                }
                // Other MMIO are emulated by host, so send MMIO tdvmcall
                // to host to emulate.
                _ => match size {
                    AddrSize::OneByte => tdvmcall_mmio_read::<u8>(raw_addr as usize) as u64,
                    AddrSize::TwoBytes => tdvmcall_mmio_read::<u16>(raw_addr as usize) as u64,
                    AddrSize::FourBytes => tdvmcall_mmio_read::<u32>(raw_addr as usize) as u64,
                    AddrSize::EightBytes => tdvmcall_mmio_read::<u64>(raw_addr as usize),
                    _ => return Err(TdxError::IoEmul(self.io_type)),
                },
            };
            emul.emulate_read(io_op)?;
        } else {
            emul.emulate_write(io_op)?;
            if hyc3_pci_ecam_hidden_for_vm(self.vm_id, raw_addr) {
                return Ok(());
            }

            match raw_addr {
                // MMIO belongs to virtual lapic is emulated by SVSM.
                v if (v >= vlapic_mmio_start && (v + size as u64) < vlapic_mmio_end) => {
                    this_vcpu_mut(self.vm_id)
                        .get_vlapic_mut()
                        .write_reg((raw_addr - vlapic_mmio_start) as u32, io_op.data)?
                }
                //MMIO belonging to virtual IOAPIC
                v if v >= vioapic_mmio_start && (v + size as u64) < vioapic_mmio_end => this_tdp(
                    self.vm_id,
                )
                .get_vioapic()
                .mmio_write(v, io_op.data as u32, size as u32)?,
                // Other MMIO are emulated by host, so send MMIO tdvmcall
                // to host to emulate.
                _ => match size {
                    AddrSize::OneByte => {
                        tdvmcall_mmio_write::<u8>(raw_addr as *const u8, io_op.data as u8)
                    }
                    AddrSize::TwoBytes => {
                        tdvmcall_mmio_write::<u16>(raw_addr as *const u16, io_op.data as u16)
                    }
                    AddrSize::FourBytes => {
                        tdvmcall_mmio_write::<u32>(raw_addr as *const u32, io_op.data as u32)
                    }
                    AddrSize::EightBytes => {
                        tdvmcall_mmio_write::<u64>(raw_addr as *const u64, io_op.data)
                    }
                    _ => return Err(TdxError::IoEmul(self.io_type)),
                },
            }
        }
        Ok(())
    }

    fn emulate_pio(&mut self, port: u16, mut emul: impl IoEmul) -> Result<(), TdxError> {
        let io_op = &mut self.io_op;
        let size = emul.get_opsize();
        if io_op.is_read() {
            if pio_touches_pci_config_data(port, size) {
                let vcpu = this_vcpu_mut(self.vm_id);
                let cfg_addr = vcpu.get_pci_cfg_addr();
                if hyc3_pci_cfg_hidden_for_vm(self.vm_id, cfg_addr) {
                    io_op.data = size.mask();
                    emul.emulate_read(io_op)?;
                    return Ok(());
                }
                tdvmcall_io_write_32(PCI_CONFIG_ADDRESS_PORT, cfg_addr);
            }

            io_op.data = match size {
                AddrSize::OneByte => tdvmcall_io_read_8(port) as u64,
                AddrSize::TwoBytes => tdvmcall_io_read_16(port) as u64,
                AddrSize::FourBytes => tdvmcall_io_read_32(port) as u64,
                _ => return Err(TdxError::IoEmul(self.io_type)),
            };

            // Intercept fw_cfg data reads for CPU count and the ACPI tables blob.
            if port == 0x511 {
                let vcpu = this_vcpu_mut(self.vm_id);
                let selector = vcpu.get_fw_cfg_selector();
                let offset = vcpu.get_fw_cfg_offset();

                io_op.data = hyc3_patch_fw_cfg_data(selector, offset, io_op.data, size);
                vcpu.advance_fw_cfg_offset(hyc3_fw_cfg_data_bytes(size));

                if selector == 0x05 || selector == 0x0F {
                    // Force the boot CPU count to the single active BSP vCPU per L2.
                    if io_op.data > HYC3_L2_ACTIVE_VCPUS_PER_VM
                        && io_op.data <= 255
                    {
                        io_op.data = HYC3_L2_ACTIVE_VCPUS_PER_VM;
                    }
                }
            }

            // Intercept QEMU CPU hotplug IO ports (0x0CD8 - 0x0CE0) and PIIX4 (0xAF00 - 0xAF08)
            // ICH9_CPU_HOTPLUG_BASE is 0x0CD8, PIIX4_CPU_HOTPLUG_BASE is 0xAF00
            if (port >= 0x0CD8 && port <= 0x0CE0) || (port >= 0xAF00 && port <= 0xAF08) {
                // If EDK2 reads QEMU_CPUHP_R_CMD_DATA2 (offset 0)
                // we return 0xFFFFFFFF to pretend modern hotplug is unsupported.
                // This forces EDK2 to fallback to fw_cfg boot CPU count.
                io_op.data = 0xFFFFFFFF;
            }

            emul.emulate_read(io_op)?;
        } else {
            emul.emulate_write(io_op)?;

            if port == PCI_CONFIG_ADDRESS_PORT && size == AddrSize::FourBytes {
                let vcpu = this_vcpu_mut(self.vm_id);
                vcpu.set_pci_cfg_addr(io_op.data as u32);
            }

            if pio_touches_pci_config_data(port, size) {
                let vcpu = this_vcpu_mut(self.vm_id);
                let cfg_addr = vcpu.get_pci_cfg_addr();
                if hyc3_pci_cfg_hidden_for_vm(self.vm_id, cfg_addr) {
                    return Ok(());
                }
                tdvmcall_io_write_32(PCI_CONFIG_ADDRESS_PORT, cfg_addr);
            }

            match size {
                AddrSize::OneByte => tdvmcall_io_write_8(port, io_op.data as u8),
                AddrSize::TwoBytes => tdvmcall_io_write_16(port, io_op.data as u16),
                AddrSize::FourBytes => tdvmcall_io_write_32(port, io_op.data as u32),
                _ => return Err(TdxError::IoEmul(self.io_type)),
            };

            // Track fw_cfg selector
            if port == 0x510 {
                let vcpu = this_vcpu_mut(self.vm_id);
                vcpu.set_fw_cfg_selector(io_op.data as u16);
            }
        }

        Ok(())
    }

    pub fn emulate(&mut self) -> Result<(), TdxError> {
        match self.io_type {
            IoType::Mmio { addr, instr_len } => self.emulate_mmio(
                addr,
                IoInstrEmul(InstrEmul::decode_instr(
                    this_vcpu_mut(self.vm_id).get_ctx_mut(),
                    instr_len,
                )?),
            ),
            IoType::TdvmcallMmio { addr, size } => {
                let emul = TdvmcallIoEmul {
                    ctx: this_vcpu_mut(self.vm_id).get_ctx_mut(),
                    size: AddrSize::try_from(size)?,
                };
                self.emulate_mmio(addr, emul)
            }
            IoType::Pio { port, size } => {
                let emul = PioEmul {
                    ctx: this_vcpu_mut(self.vm_id).get_ctx_mut(),
                    size: AddrSize::try_from(size)?,
                };
                self.emulate_pio(port, emul)
            }
            IoType::StringPio { port, instr_len } => self.emulate_pio(
                port,
                IoInstrEmul(InstrEmul::decode_instr(
                    this_vcpu_mut(self.vm_id).get_ctx_mut(),
                    instr_len,
                )?),
            ),
            IoType::TdvmcallPio { port, size } => {
                let emul = TdvmcallIoEmul {
                    ctx: this_vcpu_mut(self.vm_id).get_ctx_mut(),
                    size: AddrSize::try_from(size)?,
                };
                self.emulate_pio(port, emul)
            }
        }
    }

    pub fn need_retry(&self) -> bool {
        self.io_op.retry
    }
}
