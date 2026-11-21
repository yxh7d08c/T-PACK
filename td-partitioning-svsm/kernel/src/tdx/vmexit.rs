// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Copyright (c) 2023-2024 Intel Corporation.
//
// Author: Chuanxiao Dong <chuanxiao.dong@intel.com>

use super::error::{ErrExcp, TdxError};
use super::gctx::{GuestCpuContext, GuestCpuGPRegCode};
use super::gmem::{accept_guest_mem, convert_guest_mem};
use super::ioreq::{IoDirection, IoReq, IoType};
use super::percpu::{
    l2_pci_filter_ready, note_l2_1_ready_for_l2_2, note_l2_2_ready_for_l2_1,
    note_l2_pci_filter_ready, this_vcpu, this_vcpu_mut,
};
use super::tdcall::{tdcall_get_td_info, tdvmcall_sti_halt};
use super::tdp::this_tdp;
use super::utils::{
    td_add_page_alias, td_invept, GPAAttr, L2ExitInfo, TdCallLeaf, TdVmCallLeaf, TdpVmId,
    DUMMY_EXIT_REASON, TDCS_CONFIG_FLAGS, TDCS_NOTIFY_ENABLES, TDCS_TD_CTLS,
    TDCS_TOPOLOGY_ENUM_CONFIGURED, TDG_VP_VMCALL_INVALID_OPERAND, TDG_VP_VMCALL_RETRY,
    TDG_VP_VMCALL_SUCCESS, TDX_OPERAND_INVALID, TDX_SUCCESS, TD_CTLS_HYC3_ALLOWED_MASK,
};
use super::vcpu_comm::VcpuReqFlags;
use super::vmcs_lib::{
    VMX_INT_INFO_ERR_CODE_VALID, VMX_INT_INFO_VALID, VMX_INT_TYPE_MASK, VMX_INT_TYPE_NMI,
};
use crate::address::{Address, GuestPhysAddr};
use crate::cpu::control_regs::{write_xcr, CR4Flags, XCR0Flags, XCR0_RSVD};
use crate::cpu::features::cpu_has_xcr0_features;
use crate::cpu::idt::common::triple_fault;
use crate::cpu::interrupts::{disable_irq, enable_irq};
use crate::mm::address_space::is_kernel_phys_addr_valid;
use crate::mm::guestmem::{gpa_is_shared, gpa_strip_c_bit};
use crate::mm::memory::is_guest_phys_addr_valid;
use crate::types::{PageSize, PAGE_SIZE, PAGE_SIZE_2M};

bitflags::bitflags! {
    #[derive(Copy, Clone, Debug)]
    pub struct EPTViolationQualFlags: u64 {
        const R     = 1 << 0;
        const W     = 1 << 1;
        const X     = 1 << 2;
        const RA    = 1 << 3;
        const WA    = 1 << 4;
        const XA    = 1 << 5;
        const GLA   = 1 << 7;
        const TRAN  = 1 << 8;
    }
}

#[derive(Copy, Clone, Debug)]
pub enum CrAccessType {
    MovToCr(u8, GuestCpuGPRegCode),
    MovFromCr(u8, GuestCpuGPRegCode),
    Clts,
    Lmsw,
}

#[derive(Copy, Clone, Debug)]
pub enum VmExitReason {
    ExceptionNMI {
        intr_info: u32,
        intr_errcode: u32,
    },
    ExternalInterrupt,
    TripleFault,
    InitSignal,
    InterruptWindow,
    Cpuid,
    Hlt,
    Vmcall {
        cpl: u8,
    },
    CrAccess {
        access_type: CrAccessType,
    },
    IoInstruction {
        size: usize,
        read: bool,
        string: bool,
        port: u16,
    },
    MsrRead,
    MsrWrite,
    TprBelowThreshold,
    EoiInduced {
        vec: u8,
    },
    EptViolation {
        fault_gpa: GuestPhysAddr,
        flags: EPTViolationQualFlags,
    },
    Wbinvd,
    Xsetbv {
        cpl: u8,
    },
    Tdcall,
    Dummy,
    UnSupported(u32),
}

impl VmExitReason {
    fn new(l2exit_info: &L2ExitInfo) -> Self {
        let exit_reason = l2exit_info.exit_reason;
        match exit_reason {
            0 => VmExitReason::ExceptionNMI {
                intr_info: l2exit_info.exit_intr_info,
                intr_errcode: l2exit_info.exit_intr_errcode,
            },
            1 => VmExitReason::ExternalInterrupt,
            2 => VmExitReason::TripleFault,
            3 => VmExitReason::InitSignal,
            7 => VmExitReason::InterruptWindow,
            10 => VmExitReason::Cpuid,
            12 => VmExitReason::Hlt,
            18 => VmExitReason::Vmcall {
                cpl: l2exit_info.cpl,
            },
            28 => VmExitReason::CrAccess {
                access_type: {
                    let qualification = l2exit_info.exit_qual;
                    let cr_num = (qualification & 0xf) as u8;
                    let reg_idx = ((qualification >> 8) & 0xf) as u8;
                    match (qualification >> 4) & 0x3 {
                        0 => CrAccessType::MovToCr(
                            cr_num,
                            GuestCpuGPRegCode::try_from(reg_idx).unwrap(),
                        ),
                        1 => CrAccessType::MovFromCr(
                            cr_num,
                            GuestCpuGPRegCode::try_from(reg_idx).unwrap(),
                        ),
                        2 => CrAccessType::Clts,
                        3 => CrAccessType::Lmsw,
                        _ => unreachable!("VmExitCRInfo: invalid qualification"),
                    }
                },
            },
            30 => VmExitReason::IoInstruction {
                size: ((l2exit_info.exit_qual & 0x7) + 1) as usize,
                read: (l2exit_info.exit_qual & 0x8) != 0,
                string: (l2exit_info.exit_qual & 0x10) != 0,
                port: (l2exit_info.exit_qual >> 16) as u16,
            },
            31 => VmExitReason::MsrRead,
            32 => VmExitReason::MsrWrite,
            43 => VmExitReason::TprBelowThreshold,
            45 => VmExitReason::EoiInduced {
                vec: (l2exit_info.exit_qual & 0xff) as u8,
            },
            48 => VmExitReason::EptViolation {
                fault_gpa: GuestPhysAddr::from(l2exit_info.fault_gpa),
                flags: EPTViolationQualFlags::from_bits_truncate(l2exit_info.exit_qual),
            },
            54 => VmExitReason::Wbinvd,
            55 => VmExitReason::Xsetbv {
                cpl: l2exit_info.cpl,
            },
            77 => VmExitReason::Tdcall,
            DUMMY_EXIT_REASON => VmExitReason::Dummy,
            _ => VmExitReason::UnSupported(exit_reason),
        }
    }
}

use core::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering};

const HYC3_L2_XCR0_ALLOWED: u64 = (1 << 0) | (1 << 1) | (1 << 2);

const HYC3_CONTROL_START_GPA: u64 = 0x0080_0000;
const HYC3_CONTROL_END_GPA: u64 = 0x00a0_0000;
const HYC3_SHARED_RESULT_GPA: u64 = 0x0080_0000;
const HYC3_MAX_PAYLOAD: usize = 4096;
const HYC3_L2_ACTIVE_VCPUS_PER_VM: u32 = 1;
const HYC3_L2_APIC_STRIDE: u32 = 2;
const HYC3_CMD_GET_QUOTE: u64 = 1;
const HYC3_CMD_SUBMIT: u64 = 2;
const HYC3_CMD_WAIT_INPUT: u64 = 3;
const HYC3_CMD_COMPLETE: u64 = 4;
const HYC3_CMD_WAIT_RESULT: u64 = 5;
const HYC3_CMD_L2_READY: u64 = 6;
const HYC3_CMD_REGISTER_SHARED: u64 = 7;
const HYC3_STATE_EMPTY: u32 = 0;
const HYC3_STATE_INPUT_READY: u32 = 1;
const HYC3_STATE_DONE: u32 = 2;

static HYC3_STATE: AtomicU32 = AtomicU32::new(HYC3_STATE_EMPTY);
static HYC3_SIZE: AtomicUsize = AtomicUsize::new(0);
static HYC3_TD_CTLS: AtomicU64 = AtomicU64::new(0);
static HYC3_NMI_DROP_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_MAPGPA_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_MAPGPA_EAGER_ALIAS_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_MAPGPA_DEFERRED_ALIAS_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_IRQWIN_LOGS: AtomicU32 = AtomicU32::new(0);
static HYC3_SHARED_REGISTERED: AtomicU32 = AtomicU32::new(0);

fn hyc3_is_pci_mmio_gpa(gpa: GuestPhysAddr) -> bool {
    let addr = u64::from(gpa);

    (0x8000_0000..0xe000_0000).contains(&addr)
        || (0xf000_0000..0xfec0_0000).contains(&addr)
        || (0x1c000_0000..0x9c000_0000).contains(&addr)
}

fn hyc3_control_range_ok(gpa: u64, size: usize) -> bool {
    if size == 0 {
        return false;
    }

    match gpa.checked_add(size as u64) {
        Some(end) => gpa >= HYC3_CONTROL_START_GPA && end <= HYC3_CONTROL_END_GPA,
        None => false,
    }
}

#[derive(Debug, Copy, Clone)]
pub struct VmExit {
    vm_id: TdpVmId,
    exit_instr_len: u32,
    pub reason: VmExitReason,
}

impl VmExit {
    pub fn new(vm_id: TdpVmId, l2exit_info: L2ExitInfo) -> Self {
        let reason = VmExitReason::new(&l2exit_info);
        VmExit {
            vm_id,
            exit_instr_len: l2exit_info.exit_instr_len,
            reason,
        }
    }

    fn skip_instruction(&self) {
        let ctx = this_vcpu_mut(self.vm_id).get_ctx_mut();
        ctx.set_rip(ctx.get_rip() + self.exit_instr_len as u64);
    }

    fn handle_exception_nmi(&self, intr_info: u32, intr_errcode: u32) -> Result<(), TdxError> {
        if (intr_info & VMX_INT_INFO_VALID) == 0 {
            return Ok(());
        }

        if (intr_info & VMX_INT_TYPE_MASK) == VMX_INT_TYPE_NMI {
            let n = HYC3_NMI_DROP_LOGS.fetch_add(1, Ordering::Relaxed);
            if n < 8 {
                log::warn!(
                    "HYC3: dropping unexpected L2 NMI for VM{} intr_info={:#x}",
                    self.vm_id.num(),
                    intr_info
                );
            }
            return Ok(());
        }

        this_vcpu_mut(self.vm_id).queue_exception(
            intr_info as u8,
            if (intr_info & VMX_INT_INFO_ERR_CODE_VALID) != 0 {
                intr_errcode
            } else {
                0
            },
        )
    }

    fn handle_external_interrupt(&self) {
        // Nothing needs to be done here as external interrupt
        // will be handled by svsm already when enable IRQ after
        // the vmexit happened.
    }

    fn handle_triple_fault(&self) {
        // Handle triple fault vmexit by triggering triple fault
        // to shutdown svsm.
        triple_fault();
    }

    fn handle_init_signal(&self) {
        // Intel SDM Volume 3, 25.2:
        // INIT signals. INIT signals cause VM exits. A logical processer performs none
        // of the operations normally associated with these events. Such exits do not modify
        // register state or clear pending events as they would outside of VMX operation (If
        // a logical processor is the wait-for-SIPI state, INIT signals are blocked. They do
        // not cause VM exits in this case).
        //
        // So, it is safe to ignore the signal.
    }

    fn handle_interrupt_window(&self) {
        let n = HYC3_IRQWIN_LOGS.fetch_add(1, Ordering::Relaxed);
        if n < 16 {
            log::warn!(
                "HYC3-IRQWIN: VM{} interrupt-window exit, retrying pending event",
                self.vm_id.num()
            );
        }

        this_vcpu(self.vm_id)
            .get_cb()
            .make_request(VcpuReqFlags::REQ_EVENT | VcpuReqFlags::DIS_IRQWIN);
    }

    fn handle_cpuid(&self) {
        let ctx = this_vcpu_mut(self.vm_id).get_ctx_mut();
        let vcpuid = this_tdp(self.vm_id).get_vcpuid(
            ctx.get_gpreg(GuestCpuGPRegCode::Rax) as u32,
            ctx.get_gpreg(GuestCpuGPRegCode::Rcx) as u32,
        );

        ctx.set_gpreg(GuestCpuGPRegCode::Rax, vcpuid.eax as u64);
        ctx.set_gpreg(GuestCpuGPRegCode::Rbx, vcpuid.ebx as u64);
        ctx.set_gpreg(GuestCpuGPRegCode::Rcx, vcpuid.ecx as u64);
        ctx.set_gpreg(GuestCpuGPRegCode::Rdx, vcpuid.edx as u64);

        self.skip_instruction();
    }

    fn handle_hlt(&self, skip_ins: bool) {
        disable_irq();
        if !this_vcpu(self.vm_id).has_pending_events() {
            tdvmcall_sti_halt();
        }
        enable_irq();
        if skip_ins {
            self.skip_instruction();
        }
    }

    fn handle_vmcall(&self, cpl: u8) -> Result<(), TdxError> {
        // Doesn't emulate any vmcall but inject #GP or #UD
        if cpl != 0 {
            // Inject GP if vmcall is sending from non-ring0
            Err(TdxError::InjectExcp(ErrExcp::GP(0)))
        } else {
            // Inject UD for other cases
            Err(TdxError::InjectExcp(ErrExcp::UD))
        }
    }

    fn handle_cr(&self, access_type: CrAccessType) -> Result<(), TdxError> {
        let ctx = this_vcpu_mut(self.vm_id).get_ctx_mut();
        match access_type {
            CrAccessType::MovToCr(cr, reg) => {
                let cr_val = ctx.get_gpreg(reg);
                match cr {
                    0 => ctx.set_cr0(cr_val)?,
                    4 => ctx.set_cr4(cr_val)?,
                    8 => this_vcpu_mut(self.vm_id)
                        .get_vlapic_mut()
                        .write_cr8(cr_val)?,
                    _ => {
                        log::error!("Handle CR: Mov to CR{} unsupported", cr);
                        return Err(TdxError::UnSupportedVmExit(VmExitReason::CrAccess {
                            access_type,
                        }));
                    }
                }
            }
            CrAccessType::MovFromCr(cr, reg) => match cr {
                8 => {
                    let cr_val = this_vcpu(self.vm_id).get_vlapic().read_cr8()?;
                    ctx.set_gpreg(reg, cr_val);
                }
                _ => {
                    log::error!("Handle CR: Mov from CR{} unsupported", cr);
                    return Err(TdxError::UnSupportedVmExit(VmExitReason::CrAccess {
                        access_type,
                    }));
                }
            },
            _ => {
                log::error!("Handle CR: unsupported access_type {:?}", access_type);
                return Err(TdxError::UnSupportedVmExit(VmExitReason::CrAccess {
                    access_type,
                }));
            }
        }

        self.skip_instruction();

        Ok(())
    }

    fn handle_io(&self, size: usize, read: bool, string: bool, port: u16) -> Result<(), TdxError> {
        let io_dir = if read {
            IoDirection::Read
        } else {
            IoDirection::Write
        };
        let mut io_req = if string {
            IoReq::new(
                self.vm_id,
                IoType::StringPio {
                    port,
                    instr_len: self.exit_instr_len as usize,
                },
                io_dir,
            )
        } else {
            IoReq::new(self.vm_id, IoType::Pio { port, size }, io_dir)
        };

        io_req.emulate()?;

        if !io_req.need_retry() {
            self.skip_instruction();
        }

        Ok(())
    }

    fn handle_rdmsr(&self) -> Result<(), TdxError> {
        let ctx = this_vcpu_mut(self.vm_id).get_ctx_mut();
        let msr = ctx.get_gpreg(GuestCpuGPRegCode::Rcx) as u32;

        ctx.get_msrs().read(msr).map(|v| {
            // Store the MSR contents in RAX and RDX
            // The high-order 32 bits of each of RAX and RDX are cleared
            ctx.set_gpreg(GuestCpuGPRegCode::Rax, (v as u32).into());
            ctx.set_gpreg(GuestCpuGPRegCode::Rdx, v >> 32);
            self.skip_instruction();
        })
    }

    fn handle_wrmsr(&self) -> Result<(), TdxError> {
        let ctx = this_vcpu_mut(self.vm_id).get_ctx_mut();
        let msr = ctx.get_gpreg(GuestCpuGPRegCode::Rcx) as u32;
        let data = (ctx.get_gpreg(GuestCpuGPRegCode::Rdx) << 32)
            | (ctx.get_gpreg(GuestCpuGPRegCode::Rax) as u32 as u64);

        ctx.get_msrs_mut().write(msr, data).map(|_| {
            self.skip_instruction();
        })
    }

    fn handle_below_threshold(&self) {
        this_vcpu_mut(self.vm_id)
            .get_vlapic_mut()
            .handle_below_threshold_vmexit();
    }

    fn handle_eoi(&self, vec: u8) {
        this_vcpu_mut(self.vm_id)
            .get_vlapic_mut()
            .handle_eoi_vmexit(vec);
    }

    fn handle_page_alias(&self, gpa: GuestPhysAddr, size: PageSize) -> Result<bool, TdxError> {
        let (shared, raw_gpa) = if gpa_is_shared(gpa) {
            (true, gpa_strip_c_bit(gpa))
        } else {
            (false, gpa)
        };

        // Do not apply a static GPA range deny-list here. The same guest-physical
        // address range can be valid private memory in different L2 VMs; isolation
        // must come from the per-VM alias operation below, not from rejecting L2-1's
        // normal low-memory addresses during firmware boot.

        if hyc3_is_pci_mmio_gpa(raw_gpa) {
            return Ok(false);
        }

        if !is_guest_phys_addr_valid(raw_gpa) {
            if is_kernel_phys_addr_valid(raw_gpa.to_host_phys_addr()) {
                log::error!(
                    "Add alias for svsm phys addr 0x{:x}",
                    raw_gpa.to_host_phys_addr()
                );
                return Err(TdxError::GuestFatalErr);
            }

            // Represent this is a guest MMIO
            return Ok(false);
        }

        // set Read/Write/Execute_supervisor/Valid
        let attr = (GPAAttr::R | GPAAttr::W | GPAAttr::Xs).bits();
        let gpa_attr = attr | GPAAttr::Valid.bits();
        let attr_flags = attr;
        let (tdx_level, len) = match size {
            PageSize::Regular => (0, PAGE_SIZE),
            PageSize::Huge => (1, PAGE_SIZE_2M),
        };

        // Shared DMA pages also need a per-L2 alias. Converting them to shared
        // lets the host device see the memory; the alias lets only this L2 VM
        // use that GPA. The HYC3 control window is still mediated separately by
        // the 0x8888 VMCALL ABI, so this does not grant L2-1 access to L2-2
        // private pages.
        if !shared {
            accept_guest_mem(raw_gpa.page_align(), raw_gpa.page_align() + len)?;
        }

        let gpa_mapping = (raw_gpa.page_align().bits() as u64) | tdx_level;
        td_add_page_alias(self.vm_id, gpa_mapping, gpa_attr, attr_flags)?;
        td_invept(self.vm_id);

        Ok(true)
    }

    fn hyc3_alias_mapgpa_range(
        &self,
        start: GuestPhysAddr,
        size: usize,
        accept_private: bool,
    ) -> Result<(), TdxError> {
        let attr = (GPAAttr::R | GPAAttr::W | GPAAttr::Xs).bits();
        let gpa_attr = attr | GPAAttr::Valid.bits();
        let attr_flags = attr;
        let mut cur = start.page_align();
        let end = (start + size).page_align_up();

        while u64::from(cur) < u64::from(end) {
            if !hyc3_is_pci_mmio_gpa(cur) && is_guest_phys_addr_valid(cur) {
                if accept_private {
                    accept_guest_mem(cur, cur + PAGE_SIZE)?;
                }
                td_add_page_alias(self.vm_id, u64::from(cur), gpa_attr, attr_flags)?;
            }
            cur = cur + PAGE_SIZE;
        }

        td_invept(self.vm_id);
        Ok(())
    }


    fn hyc3_register_fixed_shared_page(&self) -> Result<(), TdxError> {
        let start = GuestPhysAddr::from(HYC3_SHARED_RESULT_GPA).page_align();
        let end = start + PAGE_SIZE;
        let attr = (GPAAttr::R | GPAAttr::W | GPAAttr::Xs).bits();
        let gpa_attr = attr | GPAAttr::Valid.bits();
        let attr_flags = attr;
        let vm_bit = 1u32 << self.vm_id.index();

        accept_guest_mem(start, end)?;
        td_add_page_alias(self.vm_id, u64::from(start), gpa_attr, attr_flags)?;
        td_invept(self.vm_id);

        let old = HYC3_SHARED_REGISTERED.fetch_or(vm_bit, Ordering::AcqRel);
        if (old & vm_bit) == 0 {
            log::warn!(
                "HYC3-SHARED: staged fixed shared GPA registered at {:#x}-{:#x} for VM{}",
                HYC3_SHARED_RESULT_GPA,
                HYC3_SHARED_RESULT_GPA + PAGE_SIZE as u64,
                self.vm_id.num()
            );
        }

        Ok(())
    }

    fn handle_ept_violation(
        &self,
        gpa: GuestPhysAddr,
        flags: EPTViolationQualFlags,
    ) -> Result<(), TdxError> {
        // Handle page alias first
        if self.handle_page_alias(gpa, PageSize::Regular)? {
            return Ok(());
        }

        // Not memory address, handle as MMIO
        let io_dir = if flags.contains(EPTViolationQualFlags::R) {
            IoDirection::Read
        } else {
            IoDirection::Write
        };

        let mut io_req = IoReq::new(
            self.vm_id,
            IoType::Mmio {
                addr: gpa,
                instr_len: self.exit_instr_len as usize,
            },
            io_dir,
        );

        io_req.emulate()?;

        if !io_req.need_retry() {
            self.skip_instruction();
        }

        Ok(())
    }

    fn handle_wbinvd(&self) {
        self.skip_instruction();
    }

    fn handle_tdg_vp_vmcall(&self, ctx: &mut GuestCpuContext) {
        let exit_type = ctx.get_gpreg(GuestCpuGPRegCode::R10);
        let leaf = ctx.get_gpreg(GuestCpuGPRegCode::R11);

        if exit_type != 0 {
            if exit_type == 5 {
                // KVM_HC_KICK_CPU
                let dest_v_apic_id = ctx.get_gpreg(GuestCpuGPRegCode::R11) as u32;
                let mut phys_apic_id = None;
                if let Some(cbs) = crate::tdx::tdp::get_vcpu_cbs(self.vm_id) {
                    for (apic_id, _cb) in cbs.iter() {
                        let mut v_apic_id = *apic_id;
                        if v_apic_id >= 2 {
                            v_apic_id -= 2;
                        }
                        if v_apic_id == dest_v_apic_id {
                            phys_apic_id = Some(*apic_id);
                            break;
                        }
                    }
                }

                if let Some(p_apic_id) = phys_apic_id {
                    crate::tdx::vcpu::kick_vcpu(p_apic_id);
                    ctx.set_gpreg(GuestCpuGPRegCode::R10, 0); // Success
                } else {
                    // Could not find destination, just return success to avoid hanging?
                    // Actually, if we return success, it will continue.
                    ctx.set_gpreg(GuestCpuGPRegCode::R10, 0);
                }
            } else {
                log::warn!("Unsupported KVM hypercall exit_type {}", exit_type);
                // Return -KVM_ENOSYS (-38)
                ctx.set_gpreg(GuestCpuGPRegCode::R10, 0xffffffffffffffda);
            }
            self.skip_instruction();
            return;
        }

        match TdVmCallLeaf::from(leaf) {
            TdVmCallLeaf::Custom8888 => {
                let cmd = ctx.get_gpreg(GuestCpuGPRegCode::R12);
                let arg0 = ctx.get_gpreg(GuestCpuGPRegCode::R13);
                let arg1 = ctx.get_gpreg(GuestCpuGPRegCode::R14) as usize;
                let vm_index = self.vm_id.index();

                match cmd {
                    HYC3_CMD_L2_READY if vm_index == 0 => {
                        note_l2_1_ready_for_l2_2();
                        log::warn!("HYC3: L2-1 reached Linux/initramfs; allowing L2-2 to start");
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                    }
                    HYC3_CMD_REGISTER_SHARED if vm_index == 0 || vm_index == 1 => {
                        match self.hyc3_register_fixed_shared_page() {
                            Ok(()) => {
                                ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                                ctx.set_gpreg(GuestCpuGPRegCode::R11, HYC3_SHARED_RESULT_GPA);
                            }
                            Err(e) => {
                                log::error!(
                                    "HYC3-SHARED: register fixed shared GPA failed for VM{} err={:?}",
                                    self.vm_id.num(),
                                    e
                                );
                                ctx.set_gpreg(
                                    GuestCpuGPRegCode::R10,
                                    TDG_VP_VMCALL_INVALID_OPERAND,
                                );
                            }
                        }
                    }
                    HYC3_CMD_GET_QUOTE if vm_index == 0 => {
                        // Quote transfer is intentionally disabled in the bring-up ABI.
                        // Keep the command non-fatal so L2-1 can continue to the shared-page test.
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                        ctx.set_gpreg(GuestCpuGPRegCode::R11, 0);
                    }
                    HYC3_CMD_SUBMIT if vm_index == 0 => {
                        if arg0 != HYC3_SHARED_RESULT_GPA
                            || arg1 == 0
                            || arg1 > HYC3_MAX_PAYLOAD
                            || !hyc3_control_range_ok(arg0, arg1)
                        {
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                        } else if let Err(e) = self.hyc3_register_fixed_shared_page() {
                            log::error!(
                                "HYC3-SHARED: submit register fixed shared GPA failed err={:?}",
                                e
                            );
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                        } else {
                            HYC3_SIZE.store(arg1, Ordering::Release);
                            HYC3_STATE.store(HYC3_STATE_INPUT_READY, Ordering::Release);
                            log::warn!(
                                "HYC3-SHARED: VM1 submit fixed shared GPA {:#x} len={}",
                                HYC3_SHARED_RESULT_GPA,
                                arg1
                            );
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                            ctx.set_gpreg(GuestCpuGPRegCode::R11, HYC3_SHARED_RESULT_GPA);
                        }
                    }
                    HYC3_CMD_WAIT_INPUT if vm_index == 1 => {
                        let state = HYC3_STATE.load(Ordering::Acquire);
                        note_l2_2_ready_for_l2_1();
                        if state != HYC3_STATE_INPUT_READY {
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_RETRY);
                            ctx.set_gpreg(GuestCpuGPRegCode::R11, state as u64);
                        } else if let Err(e) = self.hyc3_register_fixed_shared_page() {
                            log::error!(
                                "HYC3-SHARED: VM2 WAIT_INPUT register fixed shared GPA failed err={:?}",
                                e
                            );
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                        } else {
                            let size = HYC3_SIZE.load(Ordering::Acquire);
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                            ctx.set_gpreg(GuestCpuGPRegCode::R11, HYC3_SHARED_RESULT_GPA);
                            ctx.set_gpreg(GuestCpuGPRegCode::R12, size as u64);
                        }
                    }
                    HYC3_CMD_COMPLETE if vm_index == 1 => {
                        if arg0 != HYC3_SHARED_RESULT_GPA
                            || arg1 == 0
                            || arg1 > HYC3_MAX_PAYLOAD
                            || !hyc3_control_range_ok(arg0, arg1)
                        {
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                        } else {
                            HYC3_SIZE.store(arg1, Ordering::Release);
                            HYC3_STATE.store(HYC3_STATE_DONE, Ordering::Release);
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                        }
                    }
                    HYC3_CMD_WAIT_RESULT if vm_index == 0 => {
                        let state = HYC3_STATE.load(Ordering::Acquire);
                        if state != HYC3_STATE_DONE {
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_RETRY);
                            ctx.set_gpreg(GuestCpuGPRegCode::R11, state as u64);
                        } else {
                            let size = HYC3_SIZE.load(Ordering::Acquire);
                            if arg0 != 0 && (arg1 < size || !hyc3_control_range_ok(arg0, size)) {
                                ctx.set_gpreg(
                                    GuestCpuGPRegCode::R10,
                                    TDG_VP_VMCALL_INVALID_OPERAND,
                                );
                            } else if arg0 != 0 {
                                ctx.set_gpreg(
                                    GuestCpuGPRegCode::R10,
                                    TDG_VP_VMCALL_INVALID_OPERAND,
                                );
                            } else {
                                HYC3_STATE.store(HYC3_STATE_EMPTY, Ordering::Release);
                                ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                                ctx.set_gpreg(GuestCpuGPRegCode::R11, size as u64);
                                ctx.set_gpreg(GuestCpuGPRegCode::R12, HYC3_SHARED_RESULT_GPA);
                            }
                        }
                    }
                    _ => {
                        log::error!(
                            "Unsupported HYC3 VMCALL cmd {} from VM index {}",
                            cmd,
                            vm_index
                        );
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                    }
                }
            }
            TdVmCallLeaf::MapGpa => {
                let gpa = GuestPhysAddr::from(ctx.get_gpreg(GuestCpuGPRegCode::R12));
                let shared = gpa_is_shared(gpa);
                let start = gpa_strip_c_bit(gpa);
                let size = ctx.get_gpreg(GuestCpuGPRegCode::R13) as usize;
                let end = start + size;
                let log_idx = HYC3_MAPGPA_LOGS.fetch_add(1, Ordering::Relaxed);

                if log_idx < 96 {
                    log::warn!(
                        "HYC3-MAPGPA: VM{} req raw={:#x} start={:#x} end={:#x} size={:#x} shared={}",
                        self.vm_id.num(),
                        u64::from(gpa),
                        u64::from(start),
                        u64::from(end),
                        size,
                        shared
                    );
                }

                match convert_guest_mem(start, end, shared) {
                    Ok(size) => {
                        if log_idx < 96 {
                            log::warn!(
                                "HYC3-MAPGPA: VM{} ok converted={:#x} requested={:#x} shared={}",
                                self.vm_id.num(),
                                size,
                                (end - start) as u64,
                                shared
                            );
                        }
                        if size != 0 {
                            // Eagerly alias shared DMA pages only after the L2 Linux
                            // PCI/MSI path has enabled HYC3 filtering. Firmware and
                            // EFI-stub MapGPA traffic is intentionally left lazy; eager
                            // aliasing there can prevent the rootfs block device from
                            // appearing at all.
                            let eager_shared = shared && l2_pci_filter_ready(self.vm_id);
                            if !shared || eager_shared {
                                if let Err(e) = self.hyc3_alias_mapgpa_range(start, size, !shared) {
                                    log::error!(
                                        "HYC3-MAPGPA: VM{} {} alias failed start={:#x} size={:#x} err={:?}",
                                        self.vm_id.num(),
                                        if shared { "shared eager" } else { "private" },
                                        u64::from(start),
                                        size,
                                        e
                                    );
                                    ctx.set_gpreg(
                                        GuestCpuGPRegCode::R10,
                                        TDG_VP_VMCALL_INVALID_OPERAND,
                                    );
                                    self.skip_instruction();
                                    return;
                                }
                            }

                            if shared && eager_shared {
                                let alias_log_idx =
                                    HYC3_MAPGPA_EAGER_ALIAS_LOGS.fetch_add(1, Ordering::Relaxed);
                                if alias_log_idx < 16 {
                                    log::warn!(
                                        "HYC3-MAPGPA: VM{} gated shared eager alias start={:#x} size={:#x}",
                                        self.vm_id.num(),
                                        u64::from(start),
                                        size
                                    );
                                }
                            } else if shared {
                                let alias_log_idx =
                                    HYC3_MAPGPA_DEFERRED_ALIAS_LOGS.fetch_add(1, Ordering::Relaxed);
                                if alias_log_idx < 8 {
                                    log::warn!(
                                        "HYC3-MAPGPA: VM{} shared deferred-pre-pci alias start={:#x} size={:#x}",
                                        self.vm_id.num(),
                                        u64::from(start),
                                        size
                                    );
                                }
                            }
                        }
                        if size == (end - start) {
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                        } else {
                            ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_RETRY);
                            ctx.set_gpreg(GuestCpuGPRegCode::R11, u64::from(gpa + size));
                        }
                    }
                    Err(e) => {
                        log::error!(
                            "HYC3-MAPGPA: VM{} failed raw={:#x} start={:#x} size={:#x} shared={} err={:?}",
                            self.vm_id.num(),
                            u64::from(gpa),
                            u64::from(start),
                            size,
                            shared,
                            e
                        );
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                    }
                }
            }
            TdVmCallLeaf::Io => {
                let size = ctx.get_gpreg(GuestCpuGPRegCode::R12) as usize;
                let io_dir = if ctx.get_gpreg(GuestCpuGPRegCode::R13) == 0 {
                    IoDirection::Read
                } else {
                    IoDirection::Write
                };
                let port = ctx.get_gpreg(GuestCpuGPRegCode::R14) as u16;

                let mut io_req = IoReq::new(self.vm_id, IoType::TdvmcallPio { port, size }, io_dir);

                match io_req.emulate() {
                    Ok(_) => ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS),
                    Err(e) => {
                        log::error!("Failed to handle TDVMCALL PIO: {:x?}", e);
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                    }
                }
            }
            TdVmCallLeaf::Mmio => {
                let size = ctx.get_gpreg(GuestCpuGPRegCode::R12) as usize;
                let addr = GuestPhysAddr::from(ctx.get_gpreg(GuestCpuGPRegCode::R14));
                let io_dir = if ctx.get_gpreg(GuestCpuGPRegCode::R13) == 0 {
                    IoDirection::Read
                } else {
                    IoDirection::Write
                };

                let mut io_req =
                    IoReq::new(self.vm_id, IoType::TdvmcallMmio { addr, size }, io_dir);

                match io_req.emulate() {
                    Ok(_) => ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS),
                    Err(e) => {
                        log::error!("Failed to handle TDVMCALL MMIO: {:x?}", e);
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                    }
                }
            }
            TdVmCallLeaf::Halt => {
                self.handle_hlt(false);
            }
            TdVmCallLeaf::MSRRead => {
                let msr = ctx.get_gpreg(GuestCpuGPRegCode::R12) as u32;

                match ctx.get_msrs().read(msr) {
                    Ok(data) => {
                        ctx.set_gpreg(GuestCpuGPRegCode::R11, data);
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS);
                    }
                    Err(e) => {
                        log::error!("Failed to rdmsr 0x{:x}: err {:x?}", msr, e);
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                    }
                }
            }
            TdVmCallLeaf::MSRWrite => {
                let msr = ctx.get_gpreg(GuestCpuGPRegCode::R12) as u32;
                let data = ctx.get_gpreg(GuestCpuGPRegCode::R13);

                match ctx.get_msrs_mut().write(msr, data) {
                    Ok(_) => ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_SUCCESS),
                    Err(e) => {
                        log::error!("Failed to wrmsr 0x{:x}: err {:x?}", msr, e);
                        ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
                    }
                }
            }
            TdVmCallLeaf::UnSupported => {
                log::error!("Unsupported tdcall leaf {}", leaf);
                ctx.set_gpreg(GuestCpuGPRegCode::R10, TDG_VP_VMCALL_INVALID_OPERAND);
            }
        }

        self.skip_instruction();
    }

    fn handle_tdg_vp_info(&self, ctx: &mut GuestCpuContext) -> u64 {
        match tdcall_get_td_info() {
            Ok(td_info) => {
                let apic_id = this_vcpu(self.vm_id).apic_id();
                let vm_apic_base = self.vm_id.index() as u32 * HYC3_L2_APIC_STRIDE;
                let local_vcpu_index = apic_id.saturating_sub(vm_apic_base);

                ctx.set_gpreg(GuestCpuGPRegCode::Rcx, td_info.gpaw);
                ctx.set_gpreg(GuestCpuGPRegCode::Rdx, td_info.attributes);
                ctx.set_gpreg(
                    GuestCpuGPRegCode::R8,
                    HYC3_L2_ACTIVE_VCPUS_PER_VM as u64
                        | ((HYC3_L2_ACTIVE_VCPUS_PER_VM as u64) << 32),
                );
                ctx.set_gpreg(GuestCpuGPRegCode::R9, local_vcpu_index as u64);
                ctx.set_gpreg(GuestCpuGPRegCode::R10, 0);
                ctx.set_gpreg(GuestCpuGPRegCode::R11, 0);

                log::info!(
                    "TDG.VP.INFO VM {} APIC {} -> local vCPU {}",
                    self.vm_id.num(),
                    apic_id,
                    local_vcpu_index
                );

                TDX_SUCCESS
            }
            Err(e) => {
                log::error!("Failed to handle tdg_vp_info: {:?}", e);
                TDX_OPERAND_INVALID
            }
        }
    }

    fn handle_tdcall(&self) {
        let ctx = this_vcpu_mut(self.vm_id).get_ctx_mut();
        let leaf = ctx.get_gpreg(GuestCpuGPRegCode::Rax);

        let tdcall = TdCallLeaf::from(leaf);
        let ret = match tdcall {
            TdCallLeaf::TdgVpVmcall => return self.handle_tdg_vp_vmcall(ctx),
            TdCallLeaf::TdgVpInfo => self.handle_tdg_vp_info(ctx),
            // ignore mem accept tdcall from L2 guest
            TdCallLeaf::TdgMemAccept => TDX_SUCCESS,
            TdCallLeaf::TdgVmRd => {
                let rcx = ctx.get_gpreg(GuestCpuGPRegCode::Rcx);
                let rdx = ctx.get_gpreg(GuestCpuGPRegCode::Rdx);
                let field = if rdx != 0 { rdx } else { rcx };

                match field {
                    0 | TDCS_NOTIFY_ENABLES | TDCS_CONFIG_FLAGS => {
                        ctx.set_gpreg(GuestCpuGPRegCode::R8, 0);
                        TDX_SUCCESS
                    }
                    TDCS_TD_CTLS => {
                        ctx.set_gpreg(GuestCpuGPRegCode::R8, HYC3_TD_CTLS.load(Ordering::Acquire));
                        TDX_SUCCESS
                    }
                    TDCS_TOPOLOGY_ENUM_CONFIGURED => {
                        ctx.set_gpreg(GuestCpuGPRegCode::R8, 1);
                        TDX_SUCCESS
                    }
                    _ => {
                        log::error!("Unsupported tdcall for VM_RD field 0x{:x}", field);
                        TDX_OPERAND_INVALID
                    }
                }
            }
            TdCallLeaf::TdgVmWr => {
                let rcx = ctx.get_gpreg(GuestCpuGPRegCode::Rcx);
                let rdx = ctx.get_gpreg(GuestCpuGPRegCode::Rdx);
                let value = ctx.get_gpreg(GuestCpuGPRegCode::R8);
                let mask = ctx.get_gpreg(GuestCpuGPRegCode::R9);

                if (rcx == 0) && (rdx == TDCS_NOTIFY_ENABLES) {
                    // TODO: shall we enable TDCS_NOTIFY_ENABLES from L1?
                    log::warn!(
                        "Ignore TDCS_NOTIFY_ENABLES setting from L2 VM {}; enabling HYC3 PCI filter",
                        self.vm_id.num()
                    );
                    note_l2_pci_filter_ready(self.vm_id);
                    TDX_SUCCESS
                } else if (rcx == 0)
                    && (rdx == TDCS_TD_CTLS)
                    && ((mask & !TD_CTLS_HYC3_ALLOWED_MASK) == 0)
                {
                    let old = HYC3_TD_CTLS.load(Ordering::Acquire);
                    let new = (old & !mask) | (value & mask);
                    HYC3_TD_CTLS.store(new, Ordering::Release);
                    TDX_SUCCESS
                } else {
                    log::error!("Unsupported tdcall for VM_WR");
                    TDX_OPERAND_INVALID
                }
            }
            TdCallLeaf::UnSupported => {
                log::error!("Unsupported tdcall leaf {}", leaf);
                TDX_OPERAND_INVALID
            }
        };

        ctx.set_gpreg(GuestCpuGPRegCode::Rax, ret);

        self.skip_instruction();
    }

    fn handle_xsetbv(&self, cpl: u8) -> Result<(), TdxError> {
        let ctx = this_vcpu(self.vm_id).get_ctx();
        let data64 = (ctx.get_gpreg(GuestCpuGPRegCode::Rdx) << 32)
            | (ctx.get_gpreg(GuestCpuGPRegCode::Rax) as u32 as u64);
        let cr4 = CR4Flags::from_bits_truncate(ctx.get_cr4());

        if !this_vcpu(self.vm_id).get_vmcs().is_xsave_enabled()
            || !cr4.contains(CR4Flags::OSXSAVE)
            || !cpu_has_xcr0_features(data64)
        {
            return Err(TdxError::InjectExcp(ErrExcp::UD));
        }

        if (data64 & !HYC3_L2_XCR0_ALLOWED) != 0 {
            log::warn!(
                "Reject unsupported L2 XCR0 value {:#x}; allowed mask {:#x}",
                data64,
                HYC3_L2_XCR0_ALLOWED
            );
            return Err(TdxError::InjectExcp(ErrExcp::GP(0)));
        }

        // to access XCR0,'rcx' reg should be 0
        if cpl == 0
            && ctx.get_gpreg(GuestCpuGPRegCode::Rcx) == 0
            && ((data64 & XCR0Flags::X87.bits()) != 0)
            && ((data64 & XCR0_RSVD) == 0)
        {
            // XCR0[2:1] (SSE state & AVX state) can't not be
            // set to 10b as it is necessary to set both bits
            // to use AVX instructions.
            if data64 & (XCR0Flags::SSE | XCR0Flags::AVX).bits() != XCR0Flags::AVX.bits() {
                write_xcr(data64);
                self.skip_instruction();
                return Ok(());
            }
        }

        Err(TdxError::InjectExcp(ErrExcp::GP(0)))
    }

    pub fn handle_vmexits(&self) -> Result<(), TdxError> {
        match self.reason {
            VmExitReason::ExceptionNMI {
                intr_info,
                intr_errcode,
            } => self.handle_exception_nmi(intr_info, intr_errcode)?,
            VmExitReason::ExternalInterrupt => self.handle_external_interrupt(),
            VmExitReason::TripleFault => self.handle_triple_fault(),
            VmExitReason::InitSignal => self.handle_init_signal(),
            VmExitReason::InterruptWindow => self.handle_interrupt_window(),
            VmExitReason::Cpuid => self.handle_cpuid(),
            VmExitReason::Hlt => self.handle_hlt(true),
            VmExitReason::Vmcall { cpl } => self.handle_vmcall(cpl)?,
            VmExitReason::CrAccess { access_type } => self.handle_cr(access_type)?,
            VmExitReason::IoInstruction {
                size,
                read,
                string,
                port,
            } => self.handle_io(size, read, string, port)?,
            VmExitReason::MsrRead => self.handle_rdmsr()?,
            VmExitReason::MsrWrite => self.handle_wrmsr()?,
            VmExitReason::TprBelowThreshold => self.handle_below_threshold(),
            VmExitReason::EoiInduced { vec } => self.handle_eoi(vec),
            VmExitReason::EptViolation { fault_gpa, flags } => {
                self.handle_ept_violation(fault_gpa, flags)?
            }
            VmExitReason::Wbinvd => self.handle_wbinvd(),
            VmExitReason::Xsetbv { cpl } => self.handle_xsetbv(cpl)?,
            VmExitReason::Tdcall => self.handle_tdcall(),
            VmExitReason::Dummy => return Ok(()),
            VmExitReason::UnSupported(v) => {
                log::error!("UnSupported VmExit Reason {}", v);
                return Err(TdxError::InjectExcp(ErrExcp::UD));
            }
        };

        Ok(())
    }
}
