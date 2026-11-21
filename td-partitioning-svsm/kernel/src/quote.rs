use crate::mm::alloc::allocate_pages;
use crate::mm::address_space::virt_to_phys;
use crate::tdx::tdcall::{tdcall_tdreport, tdvmcall_get_quote};
use core::sync::atomic::{AtomicUsize, Ordering};
use crate::address::Address;
use crate::cpu::percpu::this_cpu_mut;

static QUOTE_VADDR: AtomicUsize = AtomicUsize::new(0);
static QUOTE_LEN: AtomicUsize = AtomicUsize::new(0);

pub fn fetch_quote() -> bool {
    let page1 = allocate_pages(0).expect("Failed to allocate report pages");
    let page2 = allocate_pages(1).expect("Failed to allocate quote pages"); // order 1 = 2 pages (8KB)
    
    let reportdata_vaddr = page1.bits();
    let tdreport_vaddr = reportdata_vaddr + 64;
    
    let quote_buf_vaddr = page2.bits(); // 4KB aligned
    
    // Fill 64 bytes reportdata
    let reportdata = unsafe { core::slice::from_raw_parts_mut(reportdata_vaddr as *mut u8, 64) };
    for i in 0..32 {
        reportdata[i] = 0xAA;
    }
    for i in 32..64 {
        reportdata[i] = 0xBB;
    }
    
    let reportdata_paddr = virt_to_phys(crate::address::VirtAddr::from(reportdata_vaddr)).bits() as u64;
    let tdreport_paddr = virt_to_phys(crate::address::VirtAddr::from(tdreport_vaddr)).bits() as u64;
    let quote_buf_paddr = virt_to_phys(crate::address::VirtAddr::from(quote_buf_vaddr)).bits() as u64;
    
    // Call tdcall_tdreport
    tdcall_tdreport(reportdata_paddr, tdreport_paddr).expect("tdcall_tdreport failed");
    
    // Ensure the quote buffer is shared memory
    let shared_mask = crate::tdx::tdcall::tdcall_get_shared_mask().expect("Failed to get shared mask");
    let quote_buf_shared_paddr = quote_buf_paddr | shared_mask;

    // Get the physical address BEFORE applying the shared mask for any page table ops
    // We must use phys_to_virt backwards or calculate it directly since virt_to_phys 
    // requires a valid mapped address and quote_buf_vaddr might not be fully mapped in 
    // the way virt_to_phys expects yet.
    let phys_addr_base = quote_buf_paddr; // We already calculated this correctly earlier!

    // We must accept it and make it shared in Guest page table as well
    crate::tdx::gmem::convert_guest_mem(crate::address::GuestPhysAddr::from(phys_addr_base), crate::address::GuestPhysAddr::from(phys_addr_base + 8192), true).expect("Failed to make quote buffer shared");

    // NOW MAP IT AS SHARED IN SVSM PAGE TABLES so we can write unencrypted data!
    this_cpu_mut().get_pgtable().set_shared_4k(crate::address::VirtAddr::from(quote_buf_vaddr)).expect("Failed to set shared 4k");
    this_cpu_mut().get_pgtable().set_shared_4k(crate::address::VirtAddr::from(quote_buf_vaddr + 4096)).expect("Failed to set shared 4k");
    // SVSM TDX doesn't support generic TLB flush, so we use flush_address_local for our 2 pages
    crate::cpu::tlb::flush_address_local(crate::address::VirtAddr::from(quote_buf_vaddr));
    crate::cpu::tlb::flush_address_local(crate::address::VirtAddr::from(quote_buf_vaddr + 4096));

    // Copy tdreport to quote_buf (which will be shared)
    let tdreport = unsafe { core::slice::from_raw_parts(tdreport_vaddr as *const u8, 1024) };
    let quote_buf = unsafe { core::slice::from_raw_parts_mut(quote_buf_vaddr as *mut u8, 8192) };
    
    // Clear the buffer
    quote_buf.fill(0);

    // Setup GHCI quote buffer format:
    // [0..8] - version, status, in_len, out_len
    // [24..1048] - TDREPORT
    
    // Version 1
    quote_buf[0..8].copy_from_slice(&1u64.to_le_bytes());
    // Status 0 (must be 0 on request)
    quote_buf[8..16].copy_from_slice(&0u64.to_le_bytes());
    // in_len 1024 (size of TDREPORT)
    quote_buf[16..20].copy_from_slice(&1024u32.to_le_bytes());
    // out_len 0 (must be 0 on request to avoid information leak)
    quote_buf[20..24].copy_from_slice(&0u32.to_le_bytes());

    // Write TDREPORT exactly after the 24-byte header
    quote_buf[24..1048].copy_from_slice(tdreport);
    
    // Call tdvmcall
    tdvmcall_get_quote(quote_buf_shared_paddr, 8192).expect("tdvmcall_get_quote failed");

    let status = u64::from_le_bytes([
        quote_buf[8], quote_buf[9], quote_buf[10], quote_buf[11],
        quote_buf[12], quote_buf[13], quote_buf[14], quote_buf[15],
    ]);
    let out_len = u32::from_le_bytes([
        quote_buf[20], quote_buf[21], quote_buf[22], quote_buf[23],
    ]) as usize;
    let total_len = core::cmp::min(24 + out_len, 8192);

    if status != 0 || out_len == 0 {
        log::error!("TD quote failed: status=0x{:x}, out_len={}", status, out_len);
        QUOTE_LEN.store(0, Ordering::SeqCst);
        QUOTE_VADDR.store(0, Ordering::SeqCst);
        false
    } else {
        QUOTE_LEN.store(total_len, Ordering::SeqCst);
        QUOTE_VADDR.store(quote_buf_vaddr, Ordering::SeqCst);
        true
    }
}

pub fn get_global_quote_vaddr() -> usize {
    QUOTE_VADDR.load(Ordering::Acquire)
}

pub fn get_global_quote_len() -> usize {
    QUOTE_LEN.load(Ordering::Acquire)
}
