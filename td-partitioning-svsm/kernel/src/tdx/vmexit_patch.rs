use core::sync::atomic::{AtomicU32, Ordering};
pub static SYNC_FLAG: AtomicU32 = AtomicU32::new(0);
