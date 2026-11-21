// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Copyright (C) 2023 Intel Corporation
//
// Author: Jason CJ Chen <jason.cj.chen@intel.com>
extern crate alloc;

use super::error::TdxError;
use super::tdp::{get_tdp_vm_id, get_vcpu_cbs, this_tdp};
use super::utils::{td_convert_kernel_pages, td_share_irte_hdr, TdpVmId};
use super::vcpu_comm::VcpuReqFlags;
use super::vlapic::vlapic_receive_intr;
use crate::address::VirtAddr;
use crate::cpu::idt::common::X86ExceptionContext;
use crate::cpu::lapic::LAPIC;
use crate::locking::SpinLock;
use crate::mm::{virt_to_phys, PAGE_SIZE};
use alloc::{
    alloc::{alloc_zeroed, Layout},
    boxed::Box,
};
use bitfield_struct::bitfield;
use core::fmt;
use core::sync::atomic::{fence, AtomicU32, Ordering};

#[bitfield(u8)]
struct SirteMode {
    // physic: 0, logical: 1
    dest_mode: bool,
    // edge: 0, level: 1
    trig_mode: bool,
    #[bits(3)]
    delivery_mode: usize,
    #[bits(3)]
    rsv: usize,
}

// Each entry in the shared buffer is 16 bytes in len.
// This includes 4 bytes for type (msi/irqchip) and
// remaining 12 bytes for msi event or irqchip event.
// Add rsvd to ensure msi event is of 12 bytes in
// size.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct MsiEvent {
    mode: SirteMode,
    vector: u8,
    dest_id: u8,
    rsvd: [u8; 9],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct IrqChipEvent {
    pin: u32,
    level: u32,
    source_id: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
union IrqEvent {
    msi: MsiEvent,
    irqchip: IrqChipEvent,
}

#[allow(unreachable_patterns)]
impl fmt::Debug for IrqEvent {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        unsafe {
            match self {
                Self { msi } => write!(f, "{:?}", msi),
                Self { irqchip } => write!(f, "{:?}", irqchip),
            }
        }
    }
}

const IRQ_TYPE_IOAPIC: u32 = 1;
const IRQ_TYPE_MSI: u32 = 2;
const HYC3_SIRTE_LOG_LIMIT: u32 = 32;
const HYC3_L2_2_MSI_DEST_MARKER: u32 = 4;
static HYC3_SIRTE_MSI_LOGS_VM1: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_MSI_LOGS_VM2: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_NOTIFY_LOGS_VM1: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_NOTIFY_LOGS_VM2: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_INIT_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_UNKNOWN_NOTIFY_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_EMPTY_DRAIN_LOGS_VM2: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_GLOBAL_NOTIFY_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_SIRTE_DEST4_GUARD_LOGS: AtomicU32 = AtomicU32::new(0);

fn hyc3_sirte_msi_logs(vm_id: TdpVmId) -> &'static AtomicU32 {
    if vm_id.index() == 1 {
        &HYC3_SIRTE_MSI_LOGS_VM2
    } else {
        &HYC3_SIRTE_MSI_LOGS_VM1
    }
}

fn hyc3_sirte_notify_logs(vm_id: TdpVmId) -> &'static AtomicU32 {
    if vm_id.index() == 1 {
        &HYC3_SIRTE_NOTIFY_LOGS_VM2
    } else {
        &HYC3_SIRTE_NOTIFY_LOGS_VM1
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct SirtEntry {
    irq_type: u32,
    event: IrqEvent,
}

impl Default for SirtEntry {
    fn default() -> Self {
        SirtEntry {
            irq_type: 0,
            event: IrqEvent {
                msi: Default::default(),
            },
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
// Sirte page is 4K. Of the 4K, first 48 bytes are allocated for header
// which comprises of vec + rsvd(16 bytes) and shared buffer(32 bytes).
// The header is fixed at 48 bytes so that the remaining 4048 (4096- 48)
// bytes are exactly a multiple of sirte entry which is of 16 bytes in len.
// For this reason add padding to shared buffer to make it 32 bytes.
struct MetaData {
    // number of elements
    num_elem: u32,
    // sizeof of elements
    elem_size: u32,
    // offset from base, to read
    head: u32,
    // offset from base, to write
    tail: u32,
    // ele_num * ele_size
    size: u32,
    padding: [u32; 3],
}

impl MetaData {
    fn is_empty(&self) -> bool {
        self.head == self.tail
    }

    fn update_head(&mut self) {
        let mut pos = self.head + 1;
        if pos >= self.num_elem {
            pos = 0;
        }
        self.head = pos;
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct SirteHeader {
    vec: u8,
    // padding to make buffer(PAGE_SIZE - Header) a multiple of struct SirtEntry (16 bytes)
    rsv: [u8; 15],
    mdata: MetaData,
}

// set size of sirte page to 8K bytes.
// 48 (sizeof(SirteHeader)) + 8144 data bytes.
const NUM_SIRTE_ENTRIES: usize = 509;
#[repr(C)]
#[repr(align(4096))]
#[derive(Debug)]
struct SirtePage {
    sirte_hdr: SirteHeader,
    data: [SirtEntry; NUM_SIRTE_ENTRIES],
}

impl SirtePage {
    fn init(&mut self, notification_vector: u8) {
        let data_size = core::mem::size_of::<SirtePage>() - core::mem::size_of::<SirteHeader>();
        let elem_size = core::mem::size_of::<SirtEntry>();
        let num_elem = data_size / elem_size;

        self.sirte_hdr.vec = notification_vector;
        self.sirte_hdr.mdata.head = 0;
        self.sirte_hdr.mdata.tail = 0;
        self.sirte_hdr.mdata.num_elem = num_elem as u32;
        self.sirte_hdr.mdata.elem_size = elem_size as u32;
        self.sirte_hdr.mdata.size = (num_elem * elem_size) as u32;
    }

    fn read(&mut self) -> Option<SirtEntry> {
        if !self.sirte_hdr.mdata.is_empty() {
            // Get the address of the SirtePage
            let entry = self.data[self.sirte_hdr.mdata.head as usize];
            // Ensure data is copied out before updating shared buffer head.
            fence(Ordering::SeqCst);
            self.sirte_hdr.mdata.update_head();
            Some(entry)
        } else {
            None
        }
    }
}

#[derive(Debug)]
struct RawSirte {
    vm_id: TdpVmId,
    page: SirtePage,
}

impl RawSirte {
    fn new(vmid: TdpVmId) -> Box<RawSirte> {
        let layout = Layout::new::<Self>();
        unsafe {
            let addr = alloc_zeroed(layout);
            let raw_sirte = addr.cast::<Self>();
            if raw_sirte.is_null() {
                panic!("TDX: RawSirte allocation failed for VM{:?}", vmid);
            }
            (*raw_sirte).vm_id = vmid;
            Box::from_raw(raw_sirte)
        }
    }

    fn init(&mut self, nv: u8) -> Result<(), TdxError> {
        let vaddr = VirtAddr::from(core::ptr::addr_of!(self.page));
        let paddr = virt_to_phys(vaddr);
        let num_shared_pages = core::mem::size_of::<SirtePage>() / PAGE_SIZE;

        td_convert_kernel_pages(paddr, paddr + core::mem::size_of::<SirtePage>(), true)?;
        self.page.init(nv);
        let ret = td_share_irte_hdr(paddr, self.vm_id, num_shared_pages as u64);
        let n = HYC3_SIRTE_INIT_LOGS.fetch_add(1, Ordering::Relaxed);
        if n < HYC3_SIRTE_LOG_LIMIT {
            log::warn!(
                "HYC3-SIRTE: VM{} init paddr=0x{:x} pages={} notify_vec={} ret={:?}",
                self.vm_id.num(),
                u64::from(paddr),
                num_shared_pages,
                nv,
                ret
            );
        }
        ret
    }

    fn handle_sirte(&mut self) {
        self.handle_sirte_for_vm(self.vm_id);
    }

    fn handle_sirte_for_vm(&mut self, target_vm_id: TdpVmId) {
        let mut drained = 0usize;
        while let Some(sirte) = self.page.read() {
            drained += 1;
            match sirte.irq_type {
                IRQ_TYPE_MSI => unsafe {
                    let level = sirte.event.msi.mode.trig_mode();
                    let raw_dest = sirte.event.msi.dest_id as u32;
                    let mut dest = raw_dest;
                    let mut phys = !sirte.event.msi.mode.dest_mode();
                    let delmode = sirte.event.msi.mode.delivery_mode() as u32;
                    let vector = sirte.event.msi.vector;
                    let inject_vm_id = if self.vm_id.index() == 0 {
                        // VM1's SIRTE page is the legacy/global ring used by
                        // current KVM TDP MSI delivery.  QEMU marks L2-2
                        // requesters with dest 4, otherwise the entry belongs
                        // to L2-1.  Non-VM1 rings are local and must not
                        // reinterpret a guest-local dest 1 as VM1.
                        if raw_dest == HYC3_L2_2_MSI_DEST_MARKER {
                            get_tdp_vm_id(1).unwrap_or(target_vm_id)
                        } else {
                            get_tdp_vm_id(0).unwrap_or(target_vm_id)
                        }
                    } else {
                        target_vm_id
                    };

                    if inject_vm_id.index() == 1 {
                        // VM2 is exposed to Linux as guest-local APIC0.  The
                        // global ring uses marker dest 4, while a true VM2-local
                        // ring may use guest-local dest 1; both should be
                        // injected as a single logical target inside VM2.
                        dest = 1;
                        phys = false;
                    }

                    let n = hyc3_sirte_msi_logs(inject_vm_id).fetch_add(1, Ordering::Relaxed);
                    if n < HYC3_SIRTE_LOG_LIMIT {
                        log::warn!(
                            "HYC3-SIRTE: global VM{} request VM{} -> VM{} MSI vec={} raw_dest={} dest={} phys={} delmode={} level={}",
                            self.vm_id.num(),
                            target_vm_id.num(),
                            inject_vm_id.num(),
                            vector,
                            raw_dest,
                            dest,
                            phys,
                            delmode,
                            level
                        );
                    }
                    vlapic_receive_intr(inject_vm_id, level, dest, phys, delmode, vector);

                    if self.vm_id.index() == 0
                        && raw_dest == HYC3_L2_2_MSI_DEST_MARKER
                        && vector == 32
                    {
                        let guard_n = HYC3_SIRTE_DEST4_GUARD_LOGS.fetch_add(1, Ordering::Relaxed);
                        if guard_n < HYC3_SIRTE_LOG_LIMIT {
                            log::warn!(
                                "HYC3-SIRTE: dest4 vector32 stays with VM2 only raw_dest={} primary_vm={} level={}",
                                raw_dest,
                                inject_vm_id.num(),
                                level
                            );
                        }
                    }
                },
                IRQ_TYPE_IOAPIC => unsafe {
                    this_tdp(target_vm_id).get_vioapic().update_pinstate(
                        sirte.event.irqchip.pin as usize,
                        sirte.event.irqchip.level,
                        sirte.event.irqchip.source_id,
                    );
                },
                _ => {
                    log::error!("handle_sirte(): Incorrect irq_type = {}!", sirte.irq_type);
                    break;
                }
            }
        }

        if drained == 0 && target_vm_id.index() == 1 {
            let n = HYC3_SIRTE_EMPTY_DRAIN_LOGS_VM2.fetch_add(1, Ordering::Relaxed);
            if n < HYC3_SIRTE_LOG_LIMIT {
                log::warn!(
                    "HYC3-SIRTE: global VM{} -> VM{} drain found empty ring",
                    self.vm_id.num(),
                    target_vm_id.num()
                );
            }
        }
    }
}

#[derive(Debug)]
pub struct Sirte {
    raw: SpinLock<Box<RawSirte>>,
}

impl Sirte {
    pub fn new(vmid: TdpVmId) -> Sirte {
        Sirte {
            raw: SpinLock::new(RawSirte::new(vmid)),
        }
    }

    pub fn init(&self, nv: u8) -> Result<(), TdxError> {
        self.raw.lock().init(nv)
    }

    pub fn handle_sirte(&self) {
        self.raw.lock().handle_sirte()
    }

    pub fn handle_sirte_for_vm(&self, target_vm_id: TdpVmId) {
        self.raw.lock().handle_sirte_for_vm(target_vm_id)
    }
}

fn hyc3_queue_sirte_request(vm_id: TdpVmId, notify_vec: u8, apic_id: u32, reason: &str) -> bool {
    let n = hyc3_sirte_notify_logs(vm_id).fetch_add(1, Ordering::Relaxed);

    if let Some(cbs) = get_vcpu_cbs(vm_id) {
        let mut queued = 0usize;
        let bsp_apic_id = if vm_id.index() == 1 { 2 } else { 0 };

        if let Some(cb) = cbs.get(&bsp_apic_id) {
            cb.make_request(VcpuReqFlags::SIRTE);
            queued = 1;
        } else {
            for cb in cbs.values() {
                cb.make_request(VcpuReqFlags::SIRTE);
                queued += 1;
            }
        }

        if queued != 0 && n < HYC3_SIRTE_LOG_LIMIT {
            log::warn!(
                "HYC3-SIRTE: notify vec={} {} VM{} on host APIC {}; queued to {} VM vCPU(s)",
                notify_vec,
                reason,
                vm_id.num(),
                apic_id,
                queued
            );
        }
        queued != 0
    } else {
        if n < HYC3_SIRTE_LOG_LIMIT {
            log::warn!(
                "HYC3-SIRTE: notify vec={} {} VM{} on host APIC {}, but no VM vCPU callbacks are ready",
                notify_vec,
                reason,
                vm_id.num(),
                apic_id
            );
        }
        false
    }
}

fn hyc3_vm_id_from_notify_apic(apic_id: u32) -> Option<TdpVmId> {
    if apic_id >= 2 {
        get_tdp_vm_id(1).or_else(|| get_tdp_vm_id(0))
    } else {
        get_tdp_vm_id(0)
    }
}

pub fn handle_sirte_request(ctx: &X86ExceptionContext) {
    let notify_vec = ctx.vector as u8;
    let apic_id = LAPIC.id();
    let target_vm = hyc3_vm_id_from_notify_apic(apic_id);

    if let Some(vm_id) = target_vm {
        let n = HYC3_SIRTE_GLOBAL_NOTIFY_LOGS.fetch_add(1, Ordering::Relaxed);
        if n < HYC3_SIRTE_LOG_LIMIT {
            log::warn!(
                "HYC3-SIRTE: global notify vec={} host APIC {} -> VM{} fanout",
                notify_vec,
                apic_id,
                vm_id.num()
            );
        }

        // HYC3/KVM currently writes MSI events for both L2 guests into VM1's
        // shared/global SIRTE page.  Keep VM1 as the single drain owner for
        // that ring, then route each entry by the requester marker.  Letting
        // VM2 drain VM1's ring can consume or miss VM1's later rootfs I/O
        // completions while L2-1 is blocked in directory or metadata reads.
        if let Some(global_vm_id) = get_tdp_vm_id(0) {
            if !hyc3_queue_sirte_request(
                global_vm_id,
                notify_vec,
                apic_id,
                "queued single owner global drain",
            ) && global_vm_id.index() != vm_id.index()
            {
                hyc3_queue_sirte_request(vm_id, notify_vec, apic_id, "queued fallback local drain");
            }
        } else {
            hyc3_queue_sirte_request(vm_id, notify_vec, apic_id, "queued fallback local drain");
        }
    } else {
        let n = HYC3_SIRTE_UNKNOWN_NOTIFY_LOGS.fetch_add(1, Ordering::Relaxed);
        if n < HYC3_SIRTE_LOG_LIMIT {
            log::warn!(
                "HYC3-SIRTE: notify vec={} on host APIC {} has no HYC3 target VM",
                notify_vec,
                apic_id
            );
        }
    }
}
