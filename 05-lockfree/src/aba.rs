//! The ABA problem, made reproducible.
//!
//! ## Refresher: stack, linked list, arena
//!
//! A **stack** is LIFO (last in, first out), like a pile of plates: `push` puts
//! an item on top, `pop` takes the top one off.
//!
//! A **singly linked list** stores each item in a *node* that also holds a link
//! ("next") to the node below it. The list itself is just a pointer to the top
//! node, called `head`. The bottom node's next is a special "nothing" value
//! (`NIL` here, `null` in C):
//!
//! ```text
//!   head ──▶ [A | next] ──▶ [B | next] ──▶ [C | next = NIL]
//!   top of stack                            bottom of stack
//! ```
//!
//! * push(X): set `X.next = head`, then `head = X`.
//! * pop():   remember `top = head`, then `head = top.next`, return `top`.
//!
//! Both are O(1) and only ever change *one* shared variable, `head`. That is
//! what makes a lock-free version possible.
//!
//! An **arena** is a fixed array (`Vec<Node>`) that owns all the nodes. Instead
//! of pointers we use **indices** into it: "node 2" means `nodes[2]`. Nothing
//! is ever allocated or freed, which keeps the demo memory-safe.
//!
//! ## Refresher: the CAS retry loop
//!
//! With several threads, "read `head`, then write `head`" is broken: another
//! thread may change `head` in between, and our write would erase its work.
//! The lock-free pattern is:
//!
//! ```text
//!   loop {
//!       old = head.load()                 // 1. look
//!       new = compute from old            // 2. prepare, privately
//!       if CAS(head, old, new) succeeds:  // 3. "if head is STILL old, set it to new"
//!           done
//!       // else someone changed head since step 1: start over with fresh data
//!   }
//! ```
//!
//! A **Treiber stack** (R. K. Treiber, 1986) is exactly that loop applied to a
//! linked-list stack.
//!
//! ## The bug
//!
//! CAS checks the **value**, not the **history**. If X goes A → B → A between
//! your read and your CAS, the CAS succeeds — but the world it was justified by
//! no longer exists.
//!
//! ```text
//!   pop():  old = head            // A
//!           next = old.next       // B          <-- decided based on THIS
//!           ...another thread runs here...
//!           CAS(head, old, next)  // "head is still A? then head = B"
//! ```
//!
//! If, meanwhile, another thread pops A, pops B, and pushes A back, `head` is A
//! again and the CAS succeeds — installing **B**, a node that is no longer in
//! the stack. Node C is lost, B is double-owned. Step by step:
//!
//! ```text
//!   start:                 head ─▶ A ─▶ B ─▶ C
//!   T1 reads:              old = A, next = B       ... T1 pauses
//!   T2 pops A:             head ─▶ B ─▶ C
//!   T2 pops B:             head ─▶ C               (T2 now owns B, reuses it:
//!                                                   B.next = NIL)
//!   T2 pushes A:           head ─▶ A ─▶ C
//!   T1 resumes: CAS(head, A, B): head IS A, so it succeeds!
//!   result:                head ─▶ B ─▶ NIL        C is lost, B belongs to T2
//! ```
//!
//! With raw pointers this is also a use-after-free (B's memory may be freed and
//! reused). Here we use an index-based arena, so it is a pure *logic* error, but
//! the mechanics are identical and — unlike the real thing — perfectly
//! deterministic: [`TreiberStack::pop_with_hook`] runs a closure between the
//! read and the CAS so a "second thread" can be injected at exactly that point.
//! Rust's type system does not catch this: every access is a well-defined
//! atomic operation, so there is no *data race*, only a wrong algorithm.
//!
//! ## The fix: tagged (versioned) heads
//!
//! Pack a generation counter next to the index and bump it on every successful
//! CAS. Then A→B→A is really (A,1)→(B,2)→(A,3); the stale CAS expects (A,1)
//! and fails. Other cures: hazard pointers, epoch-based reclamation (crossbeam),
//! or never reusing nodes while readers may hold them.
//! (A 32-bit tag can in principle wrap around after 2^32 successful operations
//! during one stalled pop; in practice that window is considered negligible.
//! With real 64-bit pointers there is no room for a tag in one word, so
//! implementations either steal unused pointer bits or use a double-width CAS
//! such as x86-64's `cmpxchg16b`.)
//!
//! Why pack both into ONE `u64` instead of two separate atomics? CAS compares
//! and swaps one memory word. Index and tag must be checked *together*, in a
//! single indivisible step; two separate CASes would reopen a gap in between.

// `Ordering::*` imports `Relaxed`, `Acquire`, `Release`, `AcqRel` by short name.
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering::*};

/// "No node" marker: `u32::MAX` = 0xFFFF_FFFF = 4_294_967_295. No arena here is
/// that large, so it can never collide with a real index.
pub const NIL: u32 = u32::MAX;

/// One list node. Only the link matters for the demo; a real stack would also
/// carry a payload. `next` is atomic because one thread may read it while
/// another rewrites it (exactly what happens in the ABA scenario).
struct Node {
    next: AtomicU32,
}

/// What a single pop *attempt* did.
#[derive(Debug, PartialEq, Eq)]
pub enum Pop {
    Popped(u32),
    Empty,
    /// The CAS failed: someone changed the head. A real caller retries.
    Retry,
}

/// A Treiber stack over a fixed arena of nodes identified by index.
pub struct TreiberStack {
    nodes: Vec<Node>,
    /// `head` = `(tag << 32) | index`. In untagged mode the tag stays 0, so this
    /// is just an index — the vulnerable version.
    head: AtomicU64,
    tagged: bool,
}

// Bit layout of the 64-bit `head` word:
//
//   bit 63                      32 31                        0
//      +--------------------------+---------------------------+
//      |        tag (u32)         |        index (u32)        |
//      +--------------------------+---------------------------+
//
// Example: index 2, tag 5  ->  0x0000_0005_0000_0002

/// Combine a node index (low 32 bits) and a tag (high 32 bits) into one word,
/// so both can be compared-and-swapped by a single 64-bit CAS.
///
/// Step by step for `pack(2, 5)`:
/// * `tag as u64`         = `0x0000_0000_0000_0005` (widen to 64 bits first,
///   or the shift below would push every bit out of a 32-bit value)
/// * `<< 32`              = `0x0000_0005_0000_0000` (tag moves to the top half)
/// * `idx as u64`         = `0x0000_0000_0000_0002`
/// * `|` combines them    = `0x0000_0005_0000_0002` (the halves don't overlap,
///   so OR just glues them together)
fn pack(idx: u32, tag: u32) -> u64 {
    ((tag as u64) << 32) | idx as u64
}
/// Low 32 bits: the node index. Casting `u64` -> `u32` keeps the low 32 bits
/// and discards the top half: `0x0000_0005_0000_0002 as u32` = `2`.
fn idx_of(w: u64) -> u32 {
    w as u32
}
/// High 32 bits: the tag. Shift the top half down into the bottom half (the old
/// bottom half falls off), then truncate: `0x0000_0005_0000_0002 >> 32` = `5`.
fn tag_of(w: u64) -> u32 {
    (w >> 32) as u32
}

impl TreiberStack {
    /// `n` nodes numbered `0..n`, none on the stack yet.
    pub fn new(n: usize, tagged: bool) -> Self {
        Self {
            nodes: (0..n).map(|_| Node { next: AtomicU32::new(NIL) }).collect(),
            head: AtomicU64::new(pack(NIL, 0)),
            tagged,
        }
    }

    /// Next tag to install: bumped in tagged mode, constant otherwise.
    /// `wrapping_add` so that tag `u32::MAX` + 1 wraps to 0 instead of
    /// panicking (see the wrap-around caveat in the module docs).
    fn next_tag(&self, old: u64) -> u32 {
        if self.tagged { tag_of(old).wrapping_add(1) } else { 0 }
    }

    /// Push node `idx` (which the caller owns, i.e. it is not on the stack).
    pub fn push(&self, idx: u32) {
        // The CAS retry loop from the module docs.
        loop {
            let old = self.head.load(Acquire);
            // Link the new node to the current top, then try to make it the top.
            self.nodes[idx as usize].next.store(idx_of(old), Relaxed);
            let new = pack(idx, self.next_tag(old));
            // Release: the `next` store above must be visible to whoever pops us.
            // (On failure — `head` changed under us — just retry; the Relaxed
            // failure ordering is fine because we reload `head` anyway.)
            if self.head.compare_exchange(old, new, Release, Relaxed).is_ok() {
                return;
            }
        }
    }

    /// One pop attempt. `between` runs after we read `head` and `head.next`
    /// but before the CAS — the window in which ABA happens. In production code
    /// there is no such hook; the "hook" is another CPU.
    ///
    /// (`impl FnOnce()` means "any closure taking no arguments that we call at
    /// most once". The demos pass a closure that plays "thread 2".)
    pub fn pop_with_hook(&self, between: impl FnOnce()) -> Pop {
        let old = self.head.load(Acquire);
        let idx = idx_of(old);
        if idx == NIL {
            return Pop::Empty;
        }
        let next = self.nodes[idx as usize].next.load(Relaxed); // decision based on this read
        between();
        let new = pack(next, self.next_tag(old));
        // AcqRel on success: Acquire to synchronise with the pusher's Release,
        // Release so our own changes are published to the next thread.
        // In tagged mode `old` includes the tag we saw, so if anyone changed
        // head in the meantime — even back to the same index — the tag differs
        // and this fails.
        match self.head.compare_exchange(old, new, AcqRel, Acquire) {
            Ok(_) => Pop::Popped(idx),
            Err(_) => Pop::Retry,
        }
    }

    /// Ordinary pop: retry until it succeeds or the stack is empty.
    pub fn pop(&self) -> Option<u32> {
        loop {
            match self.pop_with_hook(|| {}) {
                Pop::Popped(i) => return Some(i),
                Pop::Empty => return None,
                Pop::Retry => continue,
            }
        }
    }

    /// Overwrite a node's `next` — simulates its new owner reusing the node
    /// after popping it (e.g. as part of some other list).
    pub fn scribble_next(&self, idx: u32, next: u32) {
        self.nodes[idx as usize].next.store(next, Relaxed);
    }

    /// Walk the stack top-down (bounded, so a corrupt list cannot loop us forever).
    /// A corrupted list can contain a cycle (A -> B -> A -> ...); a list of `n`
    /// nodes can never legitimately be longer than `n`, so we stop there.
    pub fn snapshot(&self) -> Vec<u32> {
        let mut out = vec![];
        let mut cur = idx_of(self.head.load(Acquire));
        while cur != NIL && out.len() < self.nodes.len() + 1 {
            out.push(cur);
            cur = self.nodes[cur as usize].next.load(Relaxed);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Sanity check: last in, first out.
    #[test]
    fn lifo_order() {
        let s = TreiberStack::new(3, true);
        for i in 0..3 {
            s.push(i);
        }
        assert_eq!((s.pop(), s.pop(), s.pop(), s.pop()), (Some(2), Some(1), Some(0), None));
    }

    /// Bit-packing round trip, including the extreme values.
    #[test]
    fn pack_roundtrip() {
        assert_eq!(pack(2, 5), 0x0000_0005_0000_0002);
        for (idx, tag) in [(0, 0), (2, 5), (NIL, 0), (0, u32::MAX), (NIL, u32::MAX)] {
            let w = pack(idx, tag);
            assert_eq!((idx_of(w), tag_of(w)), (idx, tag));
        }
    }

    /// Runs the A-B-A interleaving; returns the stack contents afterwards.
    /// Tag trace in tagged mode: pushes of C, B, A leave head = (A, 3). T1 reads
    /// (A, 3). T2's pop, pop, push produce (B, 4), (C, 5), (A, 6). T1's CAS
    /// expects (A, 3) and fails. Untagged, every tag is 0: T1 expects (A, 0),
    /// finds (A, 0), and wrongly succeeds.
    fn scenario(tagged: bool) -> (Pop, Vec<u32>) {
        const A: u32 = 0;
        const B: u32 = 1;
        const C: u32 = 2;
        let s = TreiberStack::new(3, tagged);
        s.push(C);
        s.push(B);
        s.push(A); // stack: A -> B -> C
        let r = s.pop_with_hook(|| {
            // "Thread 2" runs while "thread 1" is paused mid-pop:
            assert_eq!(s.pop(), Some(A));
            assert_eq!(s.pop(), Some(B));
            s.scribble_next(B, NIL); // B now belongs to someone else
            s.push(A); //             A is back on top: A -> C
        });
        (r, s.snapshot())
    }

    #[test]
    fn untagged_stack_suffers_aba() {
        let (r, stack) = scenario(false);
        assert_eq!(r, Pop::Popped(0)); // the stale CAS wrongly SUCCEEDED
        assert_eq!(stack, vec![1]); //   head = B; C and A are lost
    }

    #[test]
    fn tagged_stack_detects_aba() {
        let (r, stack) = scenario(true);
        assert_eq!(r, Pop::Retry); //         the stale CAS correctly FAILED
        assert_eq!(stack, vec![0, 2]); //     stack intact: A -> C
    }
}
