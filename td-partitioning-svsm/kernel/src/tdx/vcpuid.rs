// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Copyright (c) 2023 Intel Corporation.
//
// Author: Chuanxiao Dong <chuanxiao.dong@intel.com>

extern crate alloc;

use super::percpu::this_vcpu;
use super::utils::TdpVmId;
use super::vcr::get_cr4_reserved_mask;
use crate::cpu::control_regs::{read_cr4, CR4Flags};
use crate::cpu::cpuid::{
    Cpuid01Ecx, Cpuid01Edx, Cpuid06Eax, Cpuid06Ecx, Cpuid07_0Ebx, Cpuid07_0Ecx, Cpuid07_0Edx,
    Cpuid40000001Eax, Cpuid80000001Edx, CPUID01_EBX_APICID_MASK, CPUID01_EBX_APICID_SHIFT,
};
use crate::cpu::msr::{MsrIa32MiscEnable, MSR_IA32_MISC_ENABLE};
use alloc::collections::btree_map::BTreeMap;
use core::arch::x86_64::{CpuidResult, __cpuid_count};
use core::cmp::{Ord, Ordering};

const HYC3_CPUID_LEAF: u32 = 0x4000_0100;
const HYC3_L2_ACTIVE_VCPUS_PER_VM: u32 = 1;
const HYC3_L2_APIC_STRIDE: u32 = 2;
const CPUID01_EBX_LOGICAL_CPUS_MASK: u32 = 0x00ff_0000;
const CPUID01_EBX_LOGICAL_CPUS_SHIFT: u32 = 16;
const CPUID04_EAX_CORES_PER_PACKAGE_MASK: u32 = 0xfc00_0000;
const CPUID04_EAX_CORES_PER_PACKAGE_SHIFT: u32 = 26;
const HYC3_L2_XSTATE_ALLOWED_MASK: u32 = (1 << 0) | (1 << 1) | (1 << 2);
const HYC3_L2_XSTATE_SIZE: u32 = 0x0000_0340;

fn mask_hyc3_extended_features(val: &mut CpuidResult) {
    val.ebx &= !(
        Cpuid07_0Ebx::MPX
            | Cpuid07_0Ebx::AVX512F
            | Cpuid07_0Ebx::AVX512DQ
            | Cpuid07_0Ebx::AVX512_IFMA
            | Cpuid07_0Ebx::AVX512PF
            | Cpuid07_0Ebx::AVX512ER
            | Cpuid07_0Ebx::AVX512CD
            | Cpuid07_0Ebx::AVX512BW
            | Cpuid07_0Ebx::AVX512VL
    )
    .bits();
    val.ecx &= !(
        Cpuid07_0Ecx::AVX512_VBMI
            | Cpuid07_0Ecx::PKU
            | Cpuid07_0Ecx::OSPKE
            | Cpuid07_0Ecx::WAITPKG
            | Cpuid07_0Ecx::AVX512_VBMI2
            | Cpuid07_0Ecx::CET_SS
            | Cpuid07_0Ecx::AVX512_VNNI
            | Cpuid07_0Ecx::AVX512_BITALG
            | Cpuid07_0Ecx::AVX512_VPOPCNTDQ
    )
    .bits();
    val.edx &= !(
        Cpuid07_0Edx::AVX512_4VNNIW
            | Cpuid07_0Edx::AVX512_4FMAPS
            | Cpuid07_0Edx::AVX512_VP2INTERSECT
            | Cpuid07_0Edx::CET_IBT
            | Cpuid07_0Edx::AMX_BF16
            | Cpuid07_0Edx::AVX512_FP16
            | Cpuid07_0Edx::AMX_TILE
            | Cpuid07_0Edx::AMX_INT8
    )
    .bits();
}

#[derive(Clone, Copy, Debug)]
struct VcpuidKey {
    leaf: u32,
    // Make subleaf as option. If a leaf doesn't
    // have valid subleaf, then its subleaf in the
    // key is None. A value Some(0) represents a
    // valid subleaf 0
    subleaf: Option<u32>,
}

impl VcpuidKey {
    fn new(leaf: u32, subleaf: Option<u32>) -> Self {
        Self { leaf, subleaf }
    }
}

impl Ord for VcpuidKey {
    fn cmp(&self, other: &Self) -> Ordering {
        let ret = self.leaf.cmp(&other.leaf);
        match ret {
            Ordering::Equal => {
                // If the VcpuidKey's subleaf is none, means
                // there is no valid subleaf for this leaf.
                // So these two keys are equal if their leaf
                // are equal.
                if other.subleaf.is_none() {
                    Ordering::Equal
                } else {
                    self.subleaf.cmp(&other.subleaf)
                }
            }
            _ => ret,
        }
    }
}

impl PartialOrd for VcpuidKey {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for VcpuidKey {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}

impl Eq for VcpuidKey {}

fn init_vcpuid_entry(leaf: u32, subleaf: Option<u32>) -> (VcpuidKey, CpuidResult) {
    let subleaf_val = if let Some(v) = subleaf { v } else { 0 };

    (
        VcpuidKey::new(leaf, subleaf),
        match leaf {
            0x00000007 => {
                if subleaf_val == 0 {
                    let cr4_reserved_mask = CR4Flags::from_bits_truncate(get_cr4_reserved_mask());

                    let mut val = unsafe { __cpuid_count(leaf, subleaf_val) };

                    // Keep the L2 xstate model conservative until XSAVE/TDVPS
                    // virtualization is complete. AVX is enough for Linux boot, while
                    // AVX512/AMX/PKU/MPX require larger state components that were
                    // causing early guest crashes.
                    mask_hyc3_extended_features(&mut val);

                    if cr4_reserved_mask.contains(CR4Flags::FSGSBASE) {
                        val.ebx &= !Cpuid07_0Ebx::FSGSBASE.bits();
                    }

                    if cr4_reserved_mask.contains(CR4Flags::SMEP) {
                        val.ebx &= !Cpuid07_0Ebx::SMEP.bits();
                    }

                    if cr4_reserved_mask.contains(CR4Flags::SMAP) {
                        val.ebx &= !Cpuid07_0Ebx::SMAP.bits();
                    }

                    if cr4_reserved_mask.contains(CR4Flags::UMIP) {
                        val.ecx &= !Cpuid07_0Ecx::UMIP.bits();
                    }

                    if cr4_reserved_mask.contains(CR4Flags::PKE) {
                        val.ecx &= !Cpuid07_0Ecx::PKU.bits();
                    }

                    if cr4_reserved_mask.contains(CR4Flags::LA57) {
                        val.ecx &= !Cpuid07_0Ecx::LA57.bits();
                    }

                    if cr4_reserved_mask.contains(CR4Flags::PKS) {
                        val.ecx &= !Cpuid07_0Ecx::PKS.bits();
                    }
                    val
                } else {
                    unsafe { __cpuid_count(leaf, subleaf_val) }
                }
            }
            0x00000021 => {
                if subleaf_val == 0 {
                    // Signature "IntelTDX    "
                    let ebx = u32::from_le_bytes([b'I', b'n', b't', b'e']);
                    let edx = u32::from_le_bytes([b'l', b'T', b'D', b'X']);
                    let ecx = u32::from_le_bytes([b' ', b' ', b' ', b' ']);

                    CpuidResult {
                        eax: 1,
                        ebx,
                        ecx,
                        edx,
                    }
                } else if subleaf_val == 1 {
                    CpuidResult {
                        eax: 0,
                        ebx: 1,
                        ecx: 0,
                        edx: 0,
                    }
                } else {
                    CpuidResult {
                        eax: 0,
                        ebx: 0,
                        ecx: 0,
                        edx: 0,
                    }
                }
            }
            0x40000000 => {
                let mut val = unsafe { __cpuid_count(leaf, subleaf_val) };
                if val.eax < HYC3_CPUID_LEAF {
                    val.eax = HYC3_CPUID_LEAF;
                }
                val
            }
            0x40000001 => {
                let mut val = unsafe { __cpuid_count(leaf, subleaf_val) };
                // Mask KVM_FEATURE_PV_SEND_IPI
                val.eax &= !Cpuid40000001Eax::KVM_FEATURE_PV_SEND_IPI.bits();
                val
            }
            _ => unsafe { __cpuid_count(leaf, subleaf_val) },
        },
    )
}

#[derive(Debug)]
pub struct Vcpuid {
    vm_id: TdpVmId,
    entries: BTreeMap<VcpuidKey, CpuidResult>,
    vcpuid_level: u32,
    vcpuid_xlevel: u32,
}

fn is_percpu_related(leaf: u32) -> bool {
    matches!(
        leaf,
        0x00000001 | 0x00000002 | 0x0000000b | 0x0000000d | 0x00000019 | 0x0000001a | 0x0000001f | 0x80000001
    )
}

impl Vcpuid {
    pub fn new(vm_id: TdpVmId) -> Self {
        Self {
            vm_id,
            entries: BTreeMap::new(),
            vcpuid_level: 0,
            vcpuid_xlevel: 0,
        }
    }

    pub fn init(&mut self) {
        let entries = &mut self.entries;

        let (key, entry) = init_vcpuid_entry(0, None);
        entries.insert(key, entry);
        self.vcpuid_level = entry.eax;

        for leaf in 1..=self.vcpuid_level {
            if is_percpu_related(leaf) {
                continue;
            }
            match leaf {
                0x00000004 => {
                    let mut subleaf = 0;
                    loop {
                        let (key, mut entry) = init_vcpuid_entry(leaf, Some(subleaf));

                        if entry.eax == 0 {
                            break;
                        }
                        entry.eax = (entry.eax & !CPUID04_EAX_CORES_PER_PACKAGE_MASK)
                            | ((HYC3_L2_ACTIVE_VCPUS_PER_VM - 1) << CPUID04_EAX_CORES_PER_PACKAGE_SHIFT);
                        entries.insert(key, entry);
                        subleaf += 1;
                    }
                }
                0x00000006 => {
                    let (key, mut entry) = init_vcpuid_entry(leaf, Some(0));
                    entry.eax &= !Cpuid06Eax::HWP.bits();
                    entry.ecx &= !Cpuid06Ecx::HCF.bits();
                    entries.insert(key, entry);
                }
                0x00000007 => {
                    let (key, mut entry) = init_vcpuid_entry(leaf, Some(0));

                    if entry.eax != 0 {
                        log::info!("Only support subleaf 0 for leaf 07h");
                        entry.eax = 0;
                    }
                    entry.ebx |= Cpuid07_0Ebx::TSC_ADJUST.bits();
                    mask_hyc3_extended_features(&mut entry);
                    entries.insert(key, entry);
                }
                // Not supported CPU leaives are not saved into entries
                0x00000005 => continue,
                0x0000000a => continue,
                0x0000000f => continue,
                0x00000010 => continue,
                0x00000014 => continue,
                0x0000001b => continue,
                0x0000001d => continue,
                0x0000001f => continue,
                0x00000021 => {
                    let (key, entry) = init_vcpuid_entry(leaf, Some(0));
                    entries.insert(key, entry);
                    let (key, entry) = init_vcpuid_entry(leaf, Some(1));
                    entries.insert(key, entry);
                }
                // The other CPU leavies are just reuse L1's VCPUID
                _ => {
                    let (key, entry) = init_vcpuid_entry(leaf, None);
                    entries.insert(key, entry);
                }
            }
        }

        let (key, mut entry) = init_vcpuid_entry(0x40000000, None);
        if entry.eax < HYC3_CPUID_LEAF {
            entry.eax = HYC3_CPUID_LEAF;
        }
        entries.insert(key, entry);
        let last_leaf = entry.eax;

        let (key, entry) = init_vcpuid_entry(0x40000001, None);
        entries.insert(key, entry);

        if last_leaf >= 0x40000010 {
            let (key, entry) = init_vcpuid_entry(0x40000010, None);
            entries.insert(key, entry);
        }

        let (key, entry) = init_vcpuid_entry(0x80000000, None);
        entries.insert(key, entry);
        self.vcpuid_xlevel = entry.eax;

        for leaf in 0x80000002..=self.vcpuid_xlevel {
            let (key, entry) = init_vcpuid_entry(leaf, None);
            entries.insert(key, entry);
        }
    }

    fn lookup_vcpuid_entries(&self, leaf: u32, subleaf: u32) -> Option<&CpuidResult> {
        let key = VcpuidKey::new(leaf, Some(subleaf));
        self.entries.get(&key)
    }

    fn find_vcpuid_entry(&self, leaf: u32, subleaf: u32) -> Option<&CpuidResult> {
        self.lookup_vcpuid_entries(leaf, subleaf).map_or_else(
            || {
                let limit = if (leaf & 0x80000000) != 0 {
                    self.vcpuid_xlevel
                } else {
                    self.vcpuid_level
                };

                //SDM Vol. 2A - Instruction Set Reference - CPUID
                //If a value entered for CPUID.EAX is higher than
                //the maximum input value for basic or extended
                //function for that processor then the data for the
                //highest basic information leaf is returned
                if leaf > limit {
                    self.lookup_vcpuid_entries(self.vcpuid_level, subleaf)
                } else {
                    None
                }
            },
            Some,
        )
    }

    fn get_vcpuid_01h(&self, subleaf: u32) -> Option<CpuidResult> {
        let cr4_reserved_mask = CR4Flags::from_bits_truncate(get_cr4_reserved_mask());
        let mut vcpuid = unsafe { __cpuid_count(0x00000001, subleaf) };

        // Patch with XAPIC ID
        let apicid = this_vcpu(self.vm_id).get_vlapic().apic_id();
        let mut v_apicid = apicid;
        // Map L2-2's APIC IDs (2, 3) to virtual APIC IDs (0, 1)
        if apicid >= 2 {
            v_apicid = apicid - 2;
        }
        vcpuid.ebx &= !CPUID01_EBX_APICID_MASK;
        vcpuid.ebx |= v_apicid << CPUID01_EBX_APICID_SHIFT;
        vcpuid.ebx &= !CPUID01_EBX_LOGICAL_CPUS_MASK;
        vcpuid.ebx |= HYC3_L2_ACTIVE_VCPUS_PER_VM << CPUID01_EBX_LOGICAL_CPUS_SHIFT;

        // The current HYC3 virtual LAPIC uses the physical CPU's
        // IA32_TSC_DEADLINE as a backing timer. Hide TSC-deadline from L2 so
        // Linux uses the less fragile LAPIC/HPET clockevent path.
        vcpuid.ecx &= !Cpuid01Ecx::TSC_DEADLINE.bits();

        if (vcpuid.ecx & Cpuid01Ecx::XSAVE.bits()) != 0
            && (this_vcpu(self.vm_id).get_ctx().get_cr4() & CR4Flags::OSXSAVE.bits()) != 0
        {
            vcpuid.ecx |= Cpuid01Ecx::OSXSAVE.bits();
        } else {
            vcpuid.ecx &= !Cpuid01Ecx::OSXSAVE.bits();
        }

        // Mask SDBG for silicon debug
        vcpuid.ecx &= !Cpuid01Ecx::SDBG.bits();
        // Mask PDCM: Perfmon and Debug Capability
        vcpuid.ecx &= !Cpuid01Ecx::PDCM.bits();

        // Mask Debug Store
        vcpuid.ecx &= !(Cpuid01Ecx::DTES64 | Cpuid01Ecx::DS_CPL).bits();
        vcpuid.edx &= !Cpuid01Edx::DTES.bits();

        let misc = MsrIa32MiscEnable::from_bits_truncate(
            this_vcpu(self.vm_id)
                .get_ctx()
                .get_msrs()
                .read(MSR_IA32_MISC_ENABLE)
                .unwrap(),
        );
        if !misc.contains(MsrIa32MiscEnable::EN_EIST) {
            vcpuid.ecx &= !Cpuid01Ecx::EIST.bits();
        }

        if cr4_reserved_mask.contains(CR4Flags::PCIDE) {
            vcpuid.ecx &= !Cpuid01Ecx::PCID.bits();
        }

        if cr4_reserved_mask.contains(CR4Flags::VME) {
            vcpuid.edx &= !Cpuid01Edx::VME.bits();
        }

        if cr4_reserved_mask.contains(CR4Flags::DE) {
            vcpuid.edx &= !Cpuid01Edx::DE.bits();
        }

        if cr4_reserved_mask.contains(CR4Flags::PSE) {
            vcpuid.edx &= !Cpuid01Edx::PSE.bits();
        }

        if cr4_reserved_mask.contains(CR4Flags::PAE) {
            vcpuid.edx &= !Cpuid01Edx::PAE.bits();
        }

        if cr4_reserved_mask.contains(CR4Flags::PGE) {
            vcpuid.edx &= !Cpuid01Edx::PGE.bits();
        }

        if cr4_reserved_mask.contains(CR4Flags::OSFXSR) {
            vcpuid.edx &= !Cpuid01Edx::FXSR.bits();
        }

        Some(vcpuid)
    }

    fn get_vcpuid_0bh(&self, leaf: u32, subleaf: u32) -> Option<CpuidResult> {
        let mut vcpuid = unsafe { __cpuid_count(leaf, subleaf) };

        // Patch with X2APIC ID
        let apicid = this_vcpu(self.vm_id).get_vlapic().apic_id();
        let mut v_apicid = apicid;
        // Map L2-2's APIC IDs (2, 3) to virtual APIC IDs (0, 1)
        if apicid >= 2 {
            v_apicid = apicid - 2;
        }
        vcpuid.edx = v_apicid;
        let level_type = (vcpuid.ecx >> 8) & 0xff;
        let logical_count = vcpuid.ebx & 0xffff;
        if level_type != 0 && logical_count > HYC3_L2_ACTIVE_VCPUS_PER_VM {
            vcpuid.ebx = (vcpuid.ebx & !0xffff) | HYC3_L2_ACTIVE_VCPUS_PER_VM;
        }

        Some(vcpuid)
    }

    fn get_hyc3_vcpuid(&self) -> CpuidResult {
        CpuidResult {
            eax: self.vm_id.index() as u32,
            ebx: HYC3_L2_APIC_STRIDE,
            ecx: self.vm_id.index() as u32 * HYC3_L2_APIC_STRIDE,
            edx: 0,
        }
    }

    fn get_vcpuid_0dh(&self, subleaf: u32) -> Option<CpuidResult> {
        let cr4 = read_cr4();
        if !cr4.contains(CR4Flags::OSXSAVE) {
            return None;
        }

        let mut vcpuid = unsafe { __cpuid_count(0x0000000d, subleaf) };
        match subleaf {
            0 => {
                vcpuid.eax &= HYC3_L2_XSTATE_ALLOWED_MASK;
                vcpuid.edx = 0;
                vcpuid.ebx = HYC3_L2_XSTATE_SIZE;
                vcpuid.ecx = HYC3_L2_XSTATE_SIZE;
            }
            1 => {
                // Keep only the base XSAVE instruction capabilities and no
                // supervisor xstate. This prevents Linux from sizing state for
                // components that L2 cannot safely use yet.
                vcpuid.eax &= 0x0000_000f;
                vcpuid.ebx = HYC3_L2_XSTATE_SIZE;
                vcpuid.ecx = 0;
                vcpuid.edx = 0;
            }
            2 => {
                vcpuid.eax = 0x100;
                vcpuid.ebx = 0x240;
                vcpuid.ecx = 0;
                vcpuid.edx = 0;
            }
            _ => {
                vcpuid.eax = 0;
                vcpuid.ebx = 0;
                vcpuid.ecx = 0;
                vcpuid.edx = 0;
            }
        }

        Some(vcpuid)
    }

    fn get_vcpuid_19h(&self, _subleaf: u32) -> Option<CpuidResult> {
        // Not support
        None
    }

    fn get_vcpuid_80000001h(&self, subleaf: u32) -> Option<CpuidResult> {
        if let Some(e) = self.find_vcpuid_entry(0x80000000, subleaf) {
            if e.eax >= 0x80000001 {
                let mut vcpuid = unsafe { __cpuid_count(0x80000001, subleaf) };
                let vmsr = MsrIa32MiscEnable::from_bits_truncate(
                    this_vcpu(self.vm_id)
                        .get_ctx()
                        .get_msrs()
                        .read(MSR_IA32_MISC_ENABLE)
                        .unwrap(),
                );
                if vmsr.contains(MsrIa32MiscEnable::XD_DISABLE) {
                    vcpuid.edx &= !Cpuid80000001Edx::NX.bits();
                }
                Some(vcpuid)
            } else {
                None
            }
        } else {
            None
        }
    }

    fn limit_vcpuid(&self, leaf: u32, src: CpuidResult) -> CpuidResult {
        let vmsr = MsrIa32MiscEnable::from_bits_truncate(
            this_vcpu(self.vm_id)
                .get_ctx()
                .get_msrs()
                .read(MSR_IA32_MISC_ENABLE)
                .unwrap(),
        );
        let mut vcpuid = src;

        if vmsr.contains(MsrIa32MiscEnable::LIMIT_CPUID) {
            if leaf == 0x00000000 {
                vcpuid.eax = 2;
            } else if leaf > 0x00000002 {
                vcpuid.eax = 0;
                vcpuid.ebx = 0;
                vcpuid.ecx = 0;
                vcpuid.edx = 0;
            }
        }

        vcpuid
    }

    pub fn get(&self, leaf: u32, subleaf: u32) -> CpuidResult {
        if leaf == HYC3_CPUID_LEAF {
            return self.get_hyc3_vcpuid();
        }

        let cpuid = if is_percpu_related(leaf) {
            match leaf {
                0x00000001 => self.get_vcpuid_01h(subleaf),
                0x0000000b => self.get_vcpuid_0bh(leaf, subleaf),
                0x0000001f => self.get_vcpuid_0bh(leaf, subleaf),
                0x0000000d => self.get_vcpuid_0dh(subleaf),
                0x00000019 => self.get_vcpuid_19h(subleaf),
                0x80000001 => self.get_vcpuid_80000001h(subleaf),
                _ => Some(unsafe { __cpuid_count(leaf, subleaf) }),
            }
        } else {
            self.find_vcpuid_entry(leaf, subleaf).copied()
        };

        if let Some(v) = cpuid {
            self.limit_vcpuid(leaf, v)
        } else {
            CpuidResult {
                eax: 0,
                ebx: 0,
                ecx: 0,
                edx: 0,
            }
        }
    }
}
