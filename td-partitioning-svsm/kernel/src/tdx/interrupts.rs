// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Copyright (c) 2023-2024 Intel Corporation.
//
// Author: Chuanxiao Dong <chuanxiao.dong@intel.com>

use super::tdp::get_vcpu_cbs;
use super::utils::{TdpVmId, FIRST_VM_ID, MAX_NUM_L2_VMS};
use super::vcpu_comm::VcpuReqFlags;
use crate::cpu::idt::common::X86ExceptionContext;
use crate::cpu::interrupts::register_interrupt_handler;
use crate::cpu::percpu::this_cpu;
use crate::error::SvsmError;
use core::sync::atomic::{AtomicU32, Ordering};

const FIXED_VECTOR_START: u8 = 0xE0;

pub const TIMER_VECTOR: u8 = FIXED_VECTOR_START;
pub const VCPU_KICK_VECTOR: u8 = FIXED_VECTOR_START + 1;

static HYC3_TIMER_IRQ_LOGS: AtomicU32 = AtomicU32::new(0);

fn handle_vcpu_kick(_ctx: &X86ExceptionContext) {
    // No need to do anything
}

fn handle_timer(_ctx: &X86ExceptionContext) {
    let host_apic_id = this_cpu().get_apic_id();
    let mut handled = false;

    // IA32_TSC_DEADLINE is physical-CPU local. A vCPU's deadline may fire on a
    // different physical APIC than its guest APIC id, so wake every registered
    // HYC3 L2 vCPU and let each vLAPIC consume the interrupt only if its own
    // deadline is due. IRQ-window retry is handled separately in virq.rs.
    for idx in 0..MAX_NUM_L2_VMS {
        let vm_id = TdpVmId::new(FIRST_VM_ID + idx as u64).unwrap();
        if let Some(cbs) = get_vcpu_cbs(vm_id) {
            for cb in cbs.values() {
                let log_idx = HYC3_TIMER_IRQ_LOGS.fetch_add(1, Ordering::Relaxed);
                if log_idx < 4 {
                    log::debug!(
                        "HYC3-VTIMER: host APIC{} timer IRQ -> VM{} APIC{}",
                        host_apic_id,
                        vm_id.num(),
                        cb.apic_id
                    );
                }

                cb.make_request(VcpuReqFlags::VTIMER);
                handled = true;
            }
        }
    }

    if !handled {
        let log_idx = HYC3_TIMER_IRQ_LOGS.fetch_add(1, Ordering::Relaxed);
        if log_idx < 4 {
            log::debug!(
                "HYC3-VTIMER: host APIC{} timer IRQ found no TDP vCPU",
                host_apic_id
            );
        }
    }
}

pub fn register_interrupt_handlers() -> Result<(), SvsmError> {
    register_interrupt_handler(Some(VCPU_KICK_VECTOR), handle_vcpu_kick)
        .map(|_| ())
        .map_err(SvsmError::Irq)?;
    register_interrupt_handler(Some(TIMER_VECTOR), handle_timer)
        .map(|_| ())
        .map_err(SvsmError::Irq)
}
