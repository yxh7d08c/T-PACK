// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Copyright (C) 2023-2024 Intel Corporation
//
// Author:
//  Chuanxiao Dong <chuanxiao.dong@intel.com>
//  Jason CJ Chen <jason.cj.chen@intel.com>

use super::interrupts::{register_interrupt_handlers, TIMER_VECTOR};
use super::tdcall::tdvmcall_sti_halt;
use super::tdp::{get_tdp_vm_id, init_tdps};
use super::utils::{tdvps_l2_ctls, L2CtlsFlags, TdpVmId, MAX_NUM_L2_VMS};
use super::vcpu::{kick_vcpu, Vcpu};
use crate::address::{Address, VirtAddr};
use crate::cpu::control_regs::{read_cr4, write_cr4, CR4Flags};
use crate::cpu::features::{cpu_has_feature, X86_FEATURE_XSAVE};
use crate::cpu::interrupts::{disable_irq, enable_irq};
use crate::cpu::lapic::{LvtTimerMode, LAPIC};
use crate::cpu::msr::rdtsc;
use crate::cpu::percpu::{this_cpu, PerCpuArch};
use crate::error::SvsmError;
use crate::mm::alloc::{allocate_pages, get_order};
use crate::types::PAGE_SIZE;
use crate::utils::zero_mem_region;
use core::any::Any;
use core::cell::OnceCell;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicBool, Ordering};

struct TdpVp {
    vcpu: Vcpu,
    l2ctls: L2CtlsFlags,
}

impl TdpVp {
    fn init(&mut self, vm_id: TdpVmId, apic_id: u32, is_bsp: bool) {
        self.vcpu.init(vm_id, apic_id, is_bsp);
        self.l2ctls = L2CtlsFlags::EnableSharedEPTP;
        tdvps_l2_ctls(vm_id, self.l2ctls).expect("Failed to set L2 controls");
    }

    fn run(&mut self) {
        self.vcpu.run();
        unreachable!("TDX: should never retrun from vcpu run loop");
    }
}

#[derive(Debug)]
pub struct TdPerCpu {
    apic_id: u32,
    is_bsp: bool,
    tdpvps: [OnceCell<VirtAddr>; MAX_NUM_L2_VMS],
}
unsafe impl Send for TdPerCpu {}
unsafe impl Sync for TdPerCpu {}

const HYC3_L2_APIC_STRIDE: u32 = 2;
const HYC3_L2_2_WAIT_TIMEOUT_CYCLES: u64 = 120_000_000_000;
const HYC3_L2_2_WAIT_LOG_CYCLES: u64 = 20_000_000_000;
static TDP_GUESTS_READY: AtomicBool = AtomicBool::new(false);
static L2_2_READY_FOR_L2_1: AtomicBool = AtomicBool::new(false);
static L2_1_READY_FOR_L2_2: AtomicBool = AtomicBool::new(false);
static L2_1_PCI_FILTER_READY: AtomicBool = AtomicBool::new(false);
static L2_2_PCI_FILTER_READY: AtomicBool = AtomicBool::new(false);

pub fn enable_tdp_guests() {
    TDP_GUESTS_READY.store(true, Ordering::Release);
    // Let the BSP/APIC0 enter L2-1 first. L2-2 is released by L2-1's
    // HYC3_CMD_L2_READY VMCALL after Linux/initramfs is alive, which avoids
    // the firmware-stage L2-2-first timer stall.
}

pub fn note_l2_2_ready_for_l2_1() {
    L2_2_READY_FOR_L2_1.store(true, Ordering::Release);
    kick_vcpu(0);
}

pub fn l2_2_ready_for_l2_1() -> bool {
    L2_2_READY_FOR_L2_1.load(Ordering::Acquire)
}

pub fn note_l2_1_ready_for_l2_2() {
    L2_1_READY_FOR_L2_2.store(true, Ordering::Release);
    kick_vcpu(HYC3_L2_APIC_STRIDE);
}

pub fn l2_1_ready_for_l2_2() -> bool {
    L2_1_READY_FOR_L2_2.load(Ordering::Acquire)
}

pub fn note_l2_pci_filter_ready(vm_id: TdpVmId) {
    match vm_id.index() {
        0 => L2_1_PCI_FILTER_READY.store(true, Ordering::Release),
        1 => L2_2_PCI_FILTER_READY.store(true, Ordering::Release),
        _ => {}
    }
}

pub fn l2_pci_filter_ready(vm_id: TdpVmId) -> bool {
    match vm_id.index() {
        0 => L2_1_PCI_FILTER_READY.load(Ordering::Acquire),
        1 => L2_2_PCI_FILTER_READY.load(Ordering::Acquire),
        _ => true,
    }
}

impl PerCpuArch for TdPerCpu {
    fn as_any(&self) -> &dyn Any {
        self
    }

    fn setup_on_cpu(&self) -> Result<(), SvsmError> {
        if cpu_has_feature(X86_FEATURE_XSAVE) {
            write_cr4(read_cr4() | CR4Flags::OSXSAVE);
        }

        (*LAPIC).init();
        (*LAPIC).setup_lvt_timer(LvtTimerMode::TscDeadline, TIMER_VECTOR);
        enable_irq();

        Ok(())
    }

    fn load(&self) {
        // Setup TD environment on BSP only
        if self.is_bsp {
            init_tdps(self.apic_id).expect("Failed to init tdps");
            register_interrupt_handlers().expect("Failed to register interrupt handlers");
        }
    }
}

impl TdPerCpu {
    pub fn new(apic_id: u32, is_bsp: bool) -> Self {
        let tdpvps = {
            let mut tdpvps: [MaybeUninit<OnceCell<VirtAddr>>; MAX_NUM_L2_VMS] =
                unsafe { MaybeUninit::uninit().assume_init() };
            for tdpvp in tdpvps.iter_mut() {
                *tdpvp = MaybeUninit::new(OnceCell::new());
            }
            unsafe { core::mem::transmute::<_, [OnceCell<VirtAddr>; MAX_NUM_L2_VMS]>(tdpvps) }
        };
        Self {
            apic_id,
            is_bsp,
            tdpvps,
        }
    }

    fn run_tdpvp(&self, vm_id: TdpVmId) {
        let vaddr = self.tdpvps[vm_id.index()].get_or_init(|| {
            let order = get_order(core::mem::size_of::<TdpVp>());
            let vaddr = allocate_pages(order).expect("Failed to allocate TdpVp");
            zero_mem_region(vaddr, vaddr + (1 << order) * PAGE_SIZE);
            vaddr
        });
        let tdpvp = unsafe { &mut *vaddr.as_mut_ptr::<TdpVp>().cast::<TdpVp>() };
        let is_vm_bsp = self.apic_id == (vm_id.index() as u32 * HYC3_L2_APIC_STRIDE);

        tdpvp.init(vm_id, self.apic_id, is_vm_bsp);
        tdpvp.run();
    }
}

pub fn this_vcpu(vm_id: TdpVmId) -> &'static Vcpu {
    if let Some(td_percpu) = this_cpu().arch.as_any().downcast_ref::<TdPerCpu>() {
        if let Some(vaddr) = td_percpu.tdpvps[vm_id.index()].get() {
            if !vaddr.is_null() {
                let tdpvp = unsafe { &*vaddr.as_ptr::<TdpVp>().cast::<TdpVp>() };
                return &tdpvp.vcpu;
            }
        }
    }

    panic!("TDX: NO vcpu found for VM{:?}", vm_id)
}

pub fn this_vcpu_mut(vm_id: TdpVmId) -> &'static mut Vcpu {
    if let Some(td_percpu) = this_cpu().arch.as_any().downcast_ref::<TdPerCpu>() {
        if let Some(vaddr) = td_percpu.tdpvps[vm_id.index()].get() {
            if !vaddr.is_null() {
                let tdpvp = unsafe { &mut *vaddr.as_mut_ptr::<TdpVp>().cast::<TdpVp>() };
                return &mut tdpvp.vcpu;
            }
        }
    }

    panic!("TDX: NO vcpu found for VM{:?}", vm_id)
}

fn hyc3_vm_index_for_apic(apic_id: u32) -> Option<usize> {
    match apic_id {
        0 => Some(0),
        2 => Some(1),
        _ => None,
    }
}

fn hyc3_park_inactive_tdp_vcpu(apic_id: u32) -> ! {
    log::warn!(
        "HYC3: parking APIC-ID {} outside L2 run loop during single-vCPU bring-up",
        apic_id
    );
    loop {
        disable_irq();
        tdvmcall_sti_halt();
        enable_irq();
    }
}

pub fn run_tdpvp() {
    let arch = this_cpu().arch.clone();
    let td_percpu = arch.as_any().downcast_ref::<TdPerCpu>().unwrap();

    let apic_id = this_cpu().get_apic_id();
    log::info!(
        "Starting TDP VP on APIC-ID {}, checking assignment...",
        apic_id
    );

    if apic_id != 0 {
        log::info!(
            "APIC-ID {} waiting until SVSM BSP enables TDP guests",
            apic_id
        );
        while !TDP_GUESTS_READY.load(Ordering::Acquire) {
            core::hint::spin_loop();
        }
    }

    let vm_index = match hyc3_vm_index_for_apic(apic_id) {
        Some(vm_index) => vm_index,
        None => hyc3_park_inactive_tdp_vcpu(apic_id),
    };

    if vm_index == 1 {
        let start = rdtsc();
        let mut last_log = 0u64;

        log::info!(
            "APIC-ID {} waiting until L2-1 reaches Linux/initramfs before starting L2-2",
            apic_id
        );
        while !l2_1_ready_for_l2_2() {
            let elapsed = rdtsc().wrapping_sub(start);
            if elapsed >= HYC3_L2_2_WAIT_TIMEOUT_CYCLES {
                log::warn!(
                    "APIC-ID {} starting L2-2 after bounded wait for L2-1 readiness elapsed={:#x}",
                    apic_id, elapsed
                );
                break;
            }
            if elapsed.wrapping_sub(last_log) >= HYC3_L2_2_WAIT_LOG_CYCLES {
                last_log = elapsed;
                log::info!(
                    "APIC-ID {} still waiting for L2-1 Linux/initramfs readiness elapsed={:#x}",
                    apic_id, elapsed
                );
            }
            disable_irq();
            tdvmcall_sti_halt();
            enable_irq();
        }

        log::info!("Assigning APIC-ID {} to VM-1 (L2-2) after L2-1 readiness", apic_id);
        match get_tdp_vm_id(1) {
            Some(vm_id) => td_percpu.run_tdpvp(vm_id),
            None => {
                log::warn!(
                    "HYC3: APIC-ID {} selected for L2-2, but TDCS exposes fewer than 2 L2 VMs; parking",
                    apic_id
                );
                hyc3_park_inactive_tdp_vcpu(apic_id);
            }
        }
    } else {
        log::info!("Assigning APIC-ID {} to VM-0 (L2-1) first", apic_id);
        td_percpu.run_tdpvp(get_tdp_vm_id(0).unwrap());
    }
}
