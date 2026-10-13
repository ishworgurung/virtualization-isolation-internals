//! A bounded, wait-free single-producer / single-consumer ring buffer.
//!
//! This is the data structure behind virtio virtqueues, io_uring's SQ/CQ and
//! most audio/network fast paths.
//!
//! ## What problem it solves
//!
//! One thread (the **producer**) creates items; another (the **consumer**)
//! processes them, in the same order (**FIFO**: first in, first out). They run
//! at different speeds, so we need a waiting room between them: a **queue**.
//! "Bounded" means the queue has a fixed maximum size; when it is full, `push`
//! fails instead of allocating more memory. "Single-producer/single-consumer"
//! (SPSC) means exactly one thread pushes and exactly one thread pops.
//!
//! ## Refresher: the ring buffer (circular buffer)
//!
//! A queue stored in a fixed array. Two positions: **head** (where the next
//! item will be read) and **tail** (where the next item will be written).
//! Push writes at tail and moves tail forward; pop reads at head and moves
//! head forward. When a position reaches the end of the array, it wraps to
//! slot 0 — the array is used as if it were a circle:
//!
//! ```text
//!   capacity 8, after pushing 1..=6 and popping 1..=2:
//!
//!   slot:   0   1   2   3   4   5   6   7
//!         [ .   .   3   4   5   6   .   . ]
//!                   ^               ^
//!                  head=2          tail=6          fill = 6 - 2 = 4
//!
//!   push 7, 8, 9: tail passes the end and wraps to slot 0
//!
//!   slot:   0   1   2   3   4   5   6   7
//!         [ 9   .   3   4   5   6   7   8 ]
//!               ^   ^
//!          tail=9  head=2      (tail as a counter is 9; its slot is 9 % 8 = 1)
//! ```
//!
//! Nothing is ever shifted or copied around; only the two positions move.
//! Both push and pop are O(1).
//!
//! ## Why this needs no lock and no CAS
//!
//! Each index has exactly **one writer**:
//!
//! ```text
//!   slots:   [ . . X X X X . . ]
//!                  ^       ^
//!                head      tail
//!         consumer writes   producer writes
//!         (pop advances)    (push advances)
//! ```
//!
//! * `tail` — next slot the producer will fill. Only the producer stores it.
//! * `head` — next slot the consumer will read. Only the consumer stores it.
//!
//! Each side only *reads* the other side's index, to learn how far it may go.
//! Two threads never race to write the same variable, so plain atomic loads
//! and stores are enough; no compare-and-swap is needed.
//!
//! ## Free-running counters, masks and wrap-around
//!
//! `head` and `tail` are **free-running** `usize` counters: they count every
//! item ever pushed/popped and are never reduced mod capacity. The actual slot
//! is `counter & mask`, where `mask = capacity - 1` (capacity is a power of two;
//! see the bits refresher in `lib.rs`):
//!
//! ```text
//!   capacity 8 -> mask 7 = 0b0111
//!   counter 13 = 0b1101;  13 & 0b0111 = 0b0101 = 5   (same as 13 % 8)
//! ```
//!
//! * fill level = `tail.wrapping_sub(head)`
//! * empty ⇔ `tail == head`
//! * full  ⇔ `tail - head == capacity`
//!
//! This removes the classic "keep one slot empty to tell full from empty"
//! trick: if you stored slot numbers (0..cap) instead of counters, an empty
//! ring and a full ring would both have `head == tail`.
//!
//! **Why `wrapping_sub` is still right after the counter overflows.** A
//! `usize` counter wraps back to 0 after 2^64 increments (at a billion pushes a
//! second, that takes ~584 years, but correctness should not depend on that).
//! The same thing, shrunk to a `u8` so the numbers are readable (u8 wraps at
//! 256):
//!
//! ```text
//!   head = 254, then 3 pushes: tail = 254 + 3 = 257 -> wraps to 1
//!   tail - head = 1 - 254 = -253 -> as u8 (mod 256): 256 - 253 = 3   correct!
//! ```
//!
//! Subtraction mod 2^N gives the right distance as long as the true distance is
//! below 2^N, which it always is (it is at most the capacity).
//!
//! ## Memory ordering — the whole game
//!
//! Modern CPUs and compilers reorder memory accesses whenever a *single* thread
//! cannot tell the difference. Another thread can. Orderings tell both what
//! must not be reordered:
//!
//! Push:  write the slot, then `tail.store(.., Release)`.
//! Pop:   `tail.load(Acquire)`, then read the slot.
//!
//! Analogy: the producer puts a parcel in a locker (writes the slot), then
//! raises a flag (stores tail). The consumer waits for the flag, then opens the
//! locker. Without ordering, the flag could be raised before the parcel is in
//! the locker, and the consumer would find it empty.
//!
//! The Release store "publishes" every write that came before it in program
//! order (the slot contents). The Acquire load that observes the new `tail`
//! *synchronises-with* it, guaranteeing the slot contents are visible. With
//! `Relaxed` on both sides the consumer could see the new `tail` but stale slot
//! bytes — on ARM/POWER for real, on x86 only via compiler reordering.
//!
//! The symmetric edge protects the *reuse* of a slot: the consumer finishes
//! reading, then `head.store(.., Release)`; the producer's `head.load(Acquire)`
//! guarantees it will not overwrite a slot still being read.
//!
//! ```text
//!   producer                              consumer
//!   --------                              --------
//!   buf[t & mask] = value
//!   tail.store(t+1, Release)  ─────────▶  tail.load(Acquire) sees t+1
//!                                         value = buf[h & mask]
//!   head.load(Acquire) sees h+1  ◀──────  head.store(h+1, Release)
//!   (now slot h may be overwritten)
//! ```
//!
//! On x86-64 an Acquire load and a Release store compile to ordinary `mov`
//! instructions (the hardware already provides that ordering); they only stop
//! the *compiler* from reordering. On ARM64 they become `ldar`/`stlr`.
//!
//! ## Two performance details
//!
//! 1. **Cache-line padding** of `head` and `tail` (see [`crate::padded`]): they
//!    are written by different cores, so sharing a line would false-share.
//! 2. **Cached remote index.** Loading the other side's index pulls its cache
//!    line across cores. Each side keeps a private copy and only re-reads the
//!    real one when the ring *looks* full/empty. Under steady load this cuts
//!    cross-core traffic to about one line transfer per *batch* instead of per item.
//!
//! ## Rust types used here, briefly
//!
//! * `Arc<T>` — a reference-counted pointer that can be shared between
//!   threads. `Producer` and `Consumer` each hold one; the `Shared` state is
//!   freed when the *last* of them is dropped.
//! * `Box<[T]>` — a fixed-size heap array (like `Vec<T>` without the ability to
//!   grow).
//! * `UnsafeCell<T>` — the only legal way in Rust to mutate something through a
//!   shared reference `&`. Normally `&` means "read-only"; `UnsafeCell` opts out
//!   and makes *you* responsible for preventing data races.
//! * `MaybeUninit<T>` — room for a `T` that may or may not currently hold a
//!   value. Rust never reads or drops it on its own; we track which slots are
//!   full (the ones in `[head, tail)`).
//! * `unsafe { .. }` — "compiler, I have checked the rules you can't check".
//!   Every `unsafe` block below has a `SAFETY:` comment saying why it holds.
//! * `Result<(), T>` / `Option<T>` — Rust's way to return "success or failure"
//!   and "a value or nothing" without exceptions or null.

use crate::padded::CachePadded;
use std::cell::UnsafeCell;
use std::mem::MaybeUninit;
// `Ordering::*` imports `Relaxed`, `Acquire`, `Release`, ... by their short names.
use std::sync::atomic::{AtomicUsize, Ordering::*};
use std::sync::Arc;

/// State shared by the two ends.
struct Shared<T> {
    /// Slot storage. `UnsafeCell` because producer and consumer both touch the
    /// buffer through `&Shared`; `MaybeUninit` because most slots hold no value.
    /// (`UnsafeCell` is Rust's only legal way to mutate through a shared `&`;
    /// `MaybeUninit<T>` is storage for a `T` that may not be initialised, and
    /// that Rust therefore never drops or reads on its own.)
    buf: Box<[UnsafeCell<MaybeUninit<T>>]>,
    /// `capacity - 1`; capacity is a power of two so `i & mask == i % capacity`.
    /// Example: capacity 1024 -> mask 1023 = 0b11_1111_1111 (ten 1-bits), so
    /// `i & mask` keeps the low 10 bits of `i`: a slot number in 0..=1023.
    mask: usize,
    /// Written only by the consumer.
    head: CachePadded<AtomicUsize>,
    /// Written only by the producer.
    tail: CachePadded<AtomicUsize>,
}

// SAFETY: the protocol guarantees a slot is accessed by at most one side at a
// time (producer owns slots in [tail, head+cap), consumer owns [head, tail)),
// with acquire/release edges between hand-overs. `T: Send` because values move
// across threads.
// (Without these impls the compiler would refuse to share `Shared` between
// threads, because it contains `UnsafeCell`, which is not `Sync`.
// `Send` = "may be moved to another thread"; `Sync` = "may be accessed through
// `&` from several threads at once".)
unsafe impl<T: Send> Sync for Shared<T> {}
unsafe impl<T: Send> Send for Shared<T> {}

impl<T> Drop for Shared<T> {
    /// Drop any items still in the queue (both ends are gone, so we have `&mut`).
    /// Runs when the last `Arc` — i.e. the later of Producer/Consumer — is dropped.
    /// `get_mut` reads the atomics without any synchronisation, which is sound
    /// because `&mut self` proves no other thread can access them any more.
    /// (Without this, items still queued would leak: `MaybeUninit` never drops
    /// its contents by itself.)
    fn drop(&mut self) {
        let head = *self.head.get_mut();
        let tail = *self.tail.get_mut();
        let mut i = head;
        // Walk the occupied counters head, head+1, ..., tail-1. `!=` rather than
        // `<` because the counters may have wrapped (tail can be numerically
        // smaller than head).
        while i != tail {
            // SAFETY: slots in [head, tail) are initialised and unread.
            unsafe { (*self.buf[i & self.mask].get()).assume_init_drop() };
            i = i.wrapping_add(1);
        }
    }
}

/// The sending half. Not `Clone` — exactly one producer is the invariant that
/// makes this lock-free without CAS. `push` takes `&mut self`, so the type
/// system also prevents two threads from pushing through the same handle.
pub struct Producer<T> {
    shared: Arc<Shared<T>>,
    /// Our private, always-current tail (we are its only writer).
    tail: usize,
    /// Stale copy of the consumer's head; refreshed only when the ring looks full.
    cached_head: usize,
}

/// The receiving half. Also not `Clone`.
pub struct Consumer<T> {
    shared: Arc<Shared<T>>,
    /// Our private, always-current head (we are its only writer).
    head: usize,
    /// Stale copy of the producer's tail; refreshed only when the ring looks empty.
    cached_tail: usize,
}

/// Create a ring holding at least `capacity` items (rounded up to a power of two
/// so that `& mask` replaces `%`; minimum 2).
///
/// `next_power_of_two`: 2 -> 2, 3 -> 4, 5 -> 8, 1000 -> 1024.
pub fn channel<T>(capacity: usize) -> (Producer<T>, Consumer<T>) {
    let cap = capacity.max(2).next_power_of_two();
    // Build `cap` empty slots. `(0..cap).map(..)` produces one value per number;
    // `.collect()` gathers them into the `Box<[_]>` asked for by the type.
    let buf: Box<[_]> = (0..cap).map(|_| UnsafeCell::new(MaybeUninit::uninit())).collect();
    let shared = Arc::new(Shared {
        buf,
        mask: cap - 1,
        head: CachePadded(AtomicUsize::new(0)),
        tail: CachePadded(AtomicUsize::new(0)),
    });
    (
        // `shared.clone()` copies the Arc *pointer* (refcount 1 -> 2), not the ring.
        Producer { shared: shared.clone(), tail: 0, cached_head: 0 },
        Consumer { shared, head: 0, cached_tail: 0 },
    )
}

impl<T> Producer<T> {
    /// Try to enqueue. Returns the value back if the ring is full. Never blocks.
    /// (Giving the value back in `Err(value)` lets the caller retry without
    /// losing it; the ring never holds a value it did not accept.)
    pub fn push(&mut self, value: T) -> Result<(), T> {
        let cap = self.shared.mask + 1;

        // Fast path uses the stale head: it can only UNDER-estimate free space
        // (the consumer only ever advances), so "not full" here is always true.
        //
        // Example, cap = 8: we last saw head = 10, so cached_head = 10; tail = 18.
        // 18 - 10 = 8 == cap: looks full. Meanwhile the consumer has popped up
        // to head = 13. Reloading gives 18 - 13 = 5 < 8: room for 3 more, and
        // we won't need to look at the consumer's cache line again for 3 pushes.
        if self.tail.wrapping_sub(self.cached_head) == cap {
            // Looks full: pay for one real load. Acquire pairs with the
            // consumer's Release store of `head`, so the consumer's read of the
            // slot happens-before we overwrite it.
            self.cached_head = self.shared.head.load(Acquire);
            if self.tail.wrapping_sub(self.cached_head) == cap {
                return Err(value); // genuinely full
            }
        }

        // SAFETY: the slot at `tail` is outside [head, tail), so the consumer
        // is not touching it, and we are the only producer.
        // `write` stores without dropping the old (uninitialised) contents.
        // `.get()` turns `&UnsafeCell<X>` into a raw `*mut X`; `(*ptr)` then
        // dereferences it (the unsafe part).
        unsafe { (*self.shared.buf[self.tail & self.shared.mask].get()).write(value) };

        self.tail = self.tail.wrapping_add(1);
        // RELEASE: publish the slot write above together with the new tail.
        self.shared.tail.store(self.tail, Release);
        Ok(())
    }
}

impl<T> Consumer<T> {
    /// Try to dequeue. Returns `None` if the ring is empty. Never blocks.
    pub fn pop(&mut self) -> Option<T> {
        // Mirror image of `push`: the stale tail can only UNDER-estimate how many
        // items are ready, so "not empty" according to the cache is always true.
        if self.head == self.cached_tail {
            // Looks empty: refresh. Acquire pairs with the producer's Release
            // store of `tail` => slot contents are visible after this load.
            self.cached_tail = self.shared.tail.load(Acquire);
            if self.head == self.cached_tail {
                return None;
            }
        }

        // SAFETY: slot at `head` is inside [head, tail): initialised by the
        // producer, and we are the only consumer. `assume_init_read` moves the
        // value out, leaving the slot logically uninitialised again.
        let value = unsafe { (*self.shared.buf[self.head & self.shared.mask].get()).assume_init_read() };

        self.head = self.head.wrapping_add(1);
        // RELEASE: tell the producer this slot may be reused (our read of it
        // above must not be reordered after this store).
        self.shared.head.store(self.head, Release);
        Some(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    /// Single-threaded basics: empty, fill to capacity, full, FIFO drain.
    #[test]
    fn fifo_and_full_empty() {
        let (mut p, mut c) = channel::<u32>(4);
        assert_eq!(c.pop(), None);
        for i in 0..4 {
            p.push(i).unwrap();
        }
        assert_eq!(p.push(99), Err(99)); // full
        for i in 0..4 {
            assert_eq!(c.pop(), Some(i));
        }
        assert_eq!(c.pop(), None);
    }

    /// Capacity 2, 1000 items: the slot index wraps around ~500 times.
    #[test]
    fn wraps_around_many_times() {
        let (mut p, mut c) = channel::<usize>(2);
        for i in 0..1000 {
            p.push(i).unwrap();
            assert_eq!(c.pop(), Some(i));
        }
    }

    /// Real concurrency: a producer thread and this (consumer) thread. Any
    /// ordering bug would show up as a lost, duplicated or out-of-order value.
    #[test]
    fn two_threads_preserve_order_and_sum() {
        const N: usize = 200_000;
        let (mut p, mut c) = channel::<usize>(64);
        // `move` hands the producer end to the new thread (ownership moves).
        let t = std::thread::spawn(move || {
            for i in 0..N {
                while p.push(i).is_err() {
                    std::hint::spin_loop(); // CPU hint: "busy-waiting" (x86 `pause`)
                }
            }
        });
        let mut expect = 0;
        while expect < N {
            if let Some(v) = c.pop() {
                assert_eq!(v, expect); // strict FIFO
                expect += 1;
            }
        }
        t.join().unwrap();
    }

    /// Items still queued when both ends are dropped must be dropped exactly once.
    #[test]
    fn leftover_items_are_dropped() {
        static DROPS: AtomicUsize = AtomicUsize::new(0);
        struct D;
        impl Drop for D {
            fn drop(&mut self) {
                DROPS.fetch_add(1, Relaxed);
            }
        }
        let (mut p, c) = channel::<D>(8);
        for _ in 0..5 {
            assert!(p.push(D).is_ok());
        }
        drop((p, c));
        assert_eq!(DROPS.load(Relaxed), 5);
    }
}
