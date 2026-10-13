//! Cache-line padding.
//!
//! ## Background: caches and cache lines
//!
//! RAM is slow (~100 ns per access); each CPU core therefore keeps small, fast
//! private copies of recently used memory in its caches (L1 ~1 ns, L2 ~4 ns).
//! Caches never hold single bytes or single variables. They hold fixed-size
//! blocks called **cache lines**: 64 bytes on x86-64 and most ARM (Apple
//! M-series and some POWER use 128). Reading one byte loads the whole 64-byte
//! line that contains it.
//!
//! Which line does an address belong to? Divide by 64 and drop the remainder
//! (equivalently `addr >> 6`, since 64 = 2^6):
//!
//! ```text
//!   address 0x1000 .. 0x103F  -> line 0x40   (4096 / 64 = 64 = 0x40)
//!   address 0x1040 .. 0x107F  -> line 0x41
//!   an AtomicU64 at 0x1008 and one at 0x1010 -> both in line 0x40
//! ```
//!
//! ## Cache coherence
//!
//! With several cores each holding copies, someone must keep them consistent.
//! The hardware does it with a **coherence protocol** (the MESI family: each
//! line in each cache is Modified, Exclusive, Shared or Invalid). The rule that
//! matters here: **a core may write a line only while it holds the only copy.**
//! Before writing, it must invalidate every other core's copy; if another core
//! then wants the line, it must fetch it back (tens of nanoseconds).
//!
//! ## False sharing
//!
//! If thread A writes variable `x` and thread B writes variable `y`, and `x` and
//! `y` sit on the SAME line, every write by A invalidates B's copy and vice
//! versa. The line bounces ("ping-pongs") between the two cores' caches on
//! every write — even though the threads never touch each other's data. The
//! program is logically parallel and physically serialised. "False" because
//! nothing is really shared; the hardware just can't tell `x` from `y` within
//! one line.
//!
//! ```text
//!   one line:  [ x x x x x x x x | y y y y y y y y | (48 unused bytes) ]
//!                 ^ core 0 writes    ^ core 1 writes  -> line ping-pongs
//!
//!   padded:    line N:   [ x x x x x x x x | (56 bytes padding) ]  <- core 0 keeps it
//!              line N+1: [ y y y y y y y y | (56 bytes padding) ]  <- core 1 keeps it
//! ```
//!
//! Fix: force hot, independently-written variables onto different lines.
//! `#[repr(align(64))]` makes the type start on a 64-byte boundary (its address
//! is a multiple of 64) *and* pads its size to a multiple of 64, so neighbours
//! can never share its line. (Rust always rounds a type's size up to a multiple
//! of its alignment, so that every element of an array stays aligned.)
//!
//! `bin/false_sharing.rs` measures the difference (several times slower).
//!
//! ## "Why not just use `AtomicU64` everywhere instead of `u64`?"
//!
//! Because **atomics do nothing about cache lines; padding does.**
//! `AtomicU64` has the same size and alignment as `u64`: 8 bytes, no padding.
//! Eight of them fit in one 64-byte line, exactly like eight `u64`s. "Atomic"
//! changes the *instructions* used to access the value, not its memory layout.
//! Proof: the slow case in `bin/false_sharing.rs` is two `AtomicU64`s side by
//! side. It is `CachePadded` (`#[repr(align(64))]`) that pushes each value
//! onto its own line, which is why `CachePadded<AtomicU64>` is 64 bytes to hold 8.
//! (The `sizes` test below checks these numbers.)
//!
//! Making everything atomic would actually be *slower*:
//!
//! * **Pricier writes.** `fetch_add` compiles to `lock add` on x86, roughly
//!   20 cycles instead of about 1 for a plain `add`, even with `Relaxed`.
//! * **Optimisations stop.** A plain `u64` in a loop can live in a register;
//!   the compiler can merge repeated writes or use SIMD. Every atomic access
//!   must really touch memory. (`false_sharing.rs` relies on this: its ITERS
//!   atomic increments cannot be collapsed into one.)
//! * **More cache traffic.** Each of those memory accesses touches the cache
//!   line, which is the very thing you were trying to avoid.
//!
//! Choosing:
//!
//! | Data | Use |
//! |---|---|
//! | Touched by only one thread (the vast majority) | plain `u64`. No other core writes the line, so false sharing can't happen |
//! | Read and written by several threads | an atomic or a `Mutex`. Rust forces this: you can't mutate a plain `u64` through a shared `&` from several threads |
//! | Hot data that *different* threads write at the same time (per-thread counters, `head`/`tail` in `spsc.rs`) | atomic **and** `CachePadded` |
//!
//! False sharing needs two cores writing the same line *at the same time*. The
//! fix is to lay out that small set of hot, shared values carefully (padding,
//! or giving each thread its own data), not to change the type of everything.

use std::ops::{Deref, DerefMut};

/// Wraps `T` so it occupies its own cache line(s).
///
/// `CachePadded(x)` is a *tuple struct* with one field; `.0` accesses it.
/// `std::mem::size_of::<CachePadded<AtomicU64>>()` is 64, not 8, and its
/// address is always a multiple of 64.
///
/// (We align to 128 in the real world when prefetchers pull line *pairs*;
/// crossbeam does exactly that on x86-64 — Intel's "adjacent/spatial line"
/// prefetcher fetches 64-byte lines in 128-byte pairs. 64 is enough to show the
/// effect.)
///
/// The cost is memory: a `CachePadded<AtomicU64>` is 64 bytes to hold 8. Use it
/// only for a few hot, contended variables, never for bulk data.
#[repr(align(64))]
#[derive(Default)]
pub struct CachePadded<T>(pub T);

/// `Deref` lets a `CachePadded<T>` be used like a `&T`: `padded.load(..)` calls
/// the method on the inner value automatically. Without it you would have to
/// write `padded.0.load(..)` everywhere. (`*padded` also gives you the `T`.)
impl<T> Deref for CachePadded<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.0
    }
}

/// `DerefMut` does the same for `&mut T` (used by `Shared::drop` in `spsc.rs`
/// to call `AtomicUsize::get_mut`).
impl<T> DerefMut for CachePadded<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, size_of};
    use std::sync::atomic::AtomicU64;

    /// Atomics don't change layout; padding does.
    #[test]
    fn sizes() {
        assert_eq!(size_of::<AtomicU64>(), size_of::<u64>()); // 8 == 8
        assert_eq!(size_of::<CachePadded<AtomicU64>>(), 64);
        assert_eq!(align_of::<CachePadded<AtomicU64>>(), 64);
    }
}
