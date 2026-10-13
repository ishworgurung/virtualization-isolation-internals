//! False sharing, measured.
//!
//! Two threads each hammer THEIR OWN counter. Logically there is zero sharing.
//! Case 1 places the counters adjacent (same 64-byte cache line); case 2 pads
//! them onto separate lines. Same work, same instructions, different speed.
//!
//! ## What is a cache line?
//!
//! The unit of memory a CPU moves between RAM and its caches: **64 bytes** on
//! x86-64 and most ARM (128 on Apple M-series). Check yours with
//! `getconf LEVEL1_DCACHE_LINESIZE`.
//!
//! RAM is slow (~100 ns per access), so each core keeps copies of recently used
//! memory in small, fast caches (L1 ~1 ns, then L2, L3). Caches never hold
//! individual variables, only whole 64-byte blocks. The line is the unit of
//! *transfer*, not the size of your data: reading a 1-byte `u8` still fetches
//! the whole 64-byte block of memory it sits in, i.e. your byte plus its 63
//! neighbouring bytes (other variables, array elements, struct fields), on the
//! bet that nearby data will be needed soon:
//!
//! ```text
//!   line at 0x1000 (64 bytes):
//!   [ neighbours ... | your u8 at 0x1008 | ... more neighbours ]
//! ```
//!
//! So a line holds whatever happens to be in that 64-byte block, and any
//! two variables in the same block share a line.
//!
//! Which line an address lives on: divide by 64 (same as `addr >> 6`):
//!
//! ```text
//!   0x1000..0x103F            -> line 0x40
//!   0x1040..0x107F            -> line 0x41
//!   a at 0x1008, b at 0x1010  -> both on line 0x40
//! ```
//!
//! That is what the `line` closure in `main` computes.
//!
//! ## Why case 1 is slow
//!
//! A core may write a line only while it holds the **only** copy (cache
//! coherence, the MESI protocol). Before writing, it invalidates every other
//! core's copy. With `a` and `b` (8 bytes each) on the same line:
//!
//! 1. Core 0 increments `a` -> the line is invalidated in core 1's cache.
//! 2. Core 1 increments `b` -> it must fetch the line back (tens of ns),
//!    invalidating core 0's copy.
//! 3. Repeat ITERS times.
//!
//! Each thread only touches its own counter, but the hardware tracks ownership
//! per *line*, not per variable, so the line ping-pongs between the cores.
//! That is **false sharing**: nothing is logically shared, yet the cores behave
//! as if it were. `CachePadded` gives each counter its own 64-byte line, so each
//! core keeps its line in L1 and never waits for the other (~4x faster on the
//! machine in the README). More background and diagrams: `src/padded.rs`.
//!
//! Run:  cargo run --release --bin false_sharing
use lockfree::padded::CachePadded;
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::time::Instant;

const ITERS: u64 = 5_000_000;

/// Memory layout (addresses illustrative):
///
/// ```text
///   Adjacent:  0x1000 [ a: 8 bytes ][ b: 8 bytes ] ...       one 64-byte line
///   Padded:    0x1000 [ a: 8 bytes ][ 56 bytes padding ]      line 0x40
///              0x1040 [ b: 8 bytes ][ 56 bytes padding ]      line 0x41
/// ```
///
/// Both counters sit next to each other (16 bytes total), so they almost
/// always share a cache line. Not guaranteed — the struct is only 8-byte
/// aligned and could straddle a line boundary — which is why `main` prints it.
///
/// Note these are already `AtomicU64`s and still false-share: an atomic is
/// 8 bytes, the same as a `u64`, so atomicity does nothing to separate
/// variables onto different lines. Only padding (`Padded` below) does. See
/// "Why not just use `AtomicU64` everywhere" in `src/padded.rs`.
struct Adjacent {
    a: AtomicU64,
    b: AtomicU64,
}

/// Each counter owns its cache line.
struct Padded {
    a: CachePadded<AtomicU64>,
    b: CachePadded<AtomicU64>,
}

/// Increment `x` ITERS times.
///
/// `x: &AtomicU64` is a shared reference: several threads may hold one at
/// once. That is fine because atomics are designed to be modified through a
/// shared `&` (a plain `u64` could not be). `for _ in 0..ITERS` repeats ITERS
/// times; `_` means "I don't need the loop counter".
fn hammer(x: &AtomicU64) {
    for _ in 0..ITERS {
        // Relaxed fetch_add: no ordering needed, we only want raw write traffic
        // on that line. (On x86 it compiles to a `lock`-prefixed add; being
        // atomic, the compiler may not merge the ITERS increments into one.)
        x.fetch_add(1, Relaxed);
    }
}

/// Run `f` and return how long it took, in milliseconds.
///
/// `<F: FnOnce()>` makes this a GENERIC function: `F` stands for "whatever
/// type the caller passes", with the constraint that it must be callable once
/// with no arguments (every closure has its own unique type, so we can't name
/// it; generics let us accept any of them). `-> f64` is the return type.
/// `Instant::now()` reads a monotonic clock; `elapsed()` is the time since
/// then; `* 1e3` converts seconds to milliseconds. The last expression without
/// a `;` is the function's return value (no `return` keyword needed).
fn time<F: FnOnce()>(f: F) -> f64 {
    let t0 = Instant::now();
    f();
    t0.elapsed().as_secs_f64() * 1e3
}

fn main() {
    let (aa, bb) = (AtomicU64::new(0), AtomicU64::new(0));
    let (pa, pb) = (AtomicU64::new(0), AtomicU64::new(0));
    let adj = Adjacent {a: aa, b: bb};
    let pad = Padded { a: CachePadded(pa), b: CachePadded(pb) };

    // ---------------------------------------------------------------------
    // Step 1: show WHERE the counters live in memory.
    // ---------------------------------------------------------------------
    //
    // `line` is a CLOSURE: a small unnamed function stored in a variable.
    //   |p: *const AtomicU64|   <- parameter list between the pipes, with type
    //   p as usize / 64         <- body; its value is the return value
    // Called like a normal function: `line(x)`.
    //
    // `*const AtomicU64` is a RAW POINTER: a plain memory address, like a C
    // pointer. We never read through it (that would need `unsafe`); we only
    // want the number. `p as usize` reinterprets the address as an integer
    // (`as` is Rust's explicit cast; Rust never converts types silently).
    //
    // Dividing by 64 gives the cache-line number: integer division drops the
    // remainder, i.e. the byte's offset within its line (same as `>> 6`, since
    // 64 = 2^6). E.g. 0x1008 / 64 = 0x40 and 0x1010 / 64 = 0x40 -> same line.
    //
    // `&adj.a` makes a REFERENCE (`&AtomicU64`, a safe, borrow-checked
    // pointer). Rust automatically converts a reference into the raw pointer
    // `line` asks for.
    let line = |p: *const AtomicU64| (p as usize) >> 6;

    // `println!` is a MACRO (the `!` marks macros): it checks the format string
    // at compile time. Each `{}` is filled by the next argument in order;
    // `{:#x}` means "hexadecimal with a 0x prefix". The third value is a
    // `bool` from `==`, printed as `true`/`false`.
    println!("adjacent: a and b on cache lines {:#x} / {:#x}  (same line: {})",
        line(&adj.a), line(&adj.b), line(&adj.a) == line(&adj.b));

    // `&*pad.a`, read right to left:
    //   pad.a     the CachePadded<AtomicU64> wrapper
    //   *pad.a    `*` dereferences; our `Deref` impl (src/padded.rs) makes it
    //             yield the AtomicU64 inside
    //   &*pad.a   `&` takes a reference to that inner AtomicU64
    // Plain `&pad.a` would be a `&CachePadded<AtomicU64>` (same address here,
    // since the counter sits at the start of the wrapper): the wrong type for
    // `line`, and Rust won't auto-unwrap when converting to a raw pointer.
    // The trailing `\n` in the string prints an extra blank line.
    println!("padded  : a and b on cache lines {:#x} / {:#x}  (same line: {})\n",
        line(&*pad.a), line(&*pad.b), line(&*pad.a) == line(&*pad.b));

    // ---------------------------------------------------------------------
    // Step 2: a helper that runs a measurement 3 times and keeps the fastest.
    // ---------------------------------------------------------------------
    //
    // Why the fastest: noise (other programs, CPU frequency changes) only ever
    // makes a run SLOWER, so the minimum is the cleanest measurement.
    //
    // The parameter type `&dyn Fn() -> f64` means "a reference to any function
    // or closure that takes no arguments and returns an f64". `dyn` = the
    // concrete type is decided at run time; we just need something callable.
    //
    // The body is an ITERATOR chain, read left to right:
    //   (0..3)                  the numbers 0, 1, 2 (a RANGE: end excluded)
    //   .map(|_| f())           for each number, ignore it (`_`) and call f()
    //                           -> three timings, e.g. 812.0, 790.5, 801.2
    //   .fold(f64::MAX, f64::min)
    //                           combine them into one value: start from the
    //                           biggest possible f64, then for each timing do
    //                           `acc = f64::min(acc, timing)`:
    //                             MAX   -> min(MAX, 812.0)   = 812.0
    //                             812.0 -> min(812.0, 790.5) = 790.5
    //                             790.5 -> min(790.5, 801.2) = 790.5  <- result
    //                           (`f64::min` is passed as a function value,
    //                           without calling it; fold calls it for us.)
    // Iterators are lazy: nothing runs until `fold` pulls the values through.
    //
    // Why not simply `.min()`? It does not compile for f64. `.min()` requires
    // `Ord` (a TOTAL order: any two values compare), and floats have NaN, which
    // is neither less than, equal to, nor greater than anything, itself
    // included. So f64 is only `PartialOrd`, and Rust makes you choose a NaN
    // policy explicitly. `f64::min` ignores NaN. Alternatives:
    //   .reduce(f64::min).unwrap()        // no seed; None if the list is empty
    //   .min_by(f64::total_cmp).unwrap()  // IEEE total order: NaN sorts last
    //
    // Cost: zero. Range, map, fold and the closure are all inlined in a release
    // build into the same loop you'd write by hand:
    //   let mut m = f64::MAX; for _ in 0..3 { m = m.min(f()); }
    // The one indirection is `&dyn Fn`: `f()` is a call through a function
    // pointer (~1 ns, irrelevant next to multi-second runs). A generic
    // `fn best<F: Fn() -> f64>(f: F)` would remove it, but closures cannot be
    // generic, hence `dyn` here.
    let best = |f: &dyn Fn() -> f64| (0..3).map(|_| f()).fold(f64::MAX, f64::min);

    // ---------------------------------------------------------------------
    // Step 3: time both layouts, two threads each.
    // ---------------------------------------------------------------------
    //
    // This packs three closures inside each other; unpacked from the inside:
    //
    //   std::thread::scope(|s| { s.spawn(..); s.spawn(..); })
    //       Starts two threads and WAITS for both to finish before returning.
    //       `s` is the scope handle; `s.spawn(|| hammer(&adj.a))` starts a
    //       thread that runs `hammer` on counter `a`, the other on `b`.
    //       Why `scope`? The threads borrow `adj`, a local variable of `main`.
    //       A plain `std::thread::spawn` thread could outlive `main`'s stack
    //       frame, so Rust would reject the borrow. `scope` guarantees the
    //       threads are joined before `adj` can go away, so borrowing is safe.
    //
    //   time(|| ...)
    //       Our helper above: runs the closure and returns how long it took,
    //       in milliseconds.
    //
    //   best(&|| ...)
    //       Runs the timing 3 times and keeps the fastest. `&` because `best`
    //       takes a *reference* to a closure (`&dyn Fn`).
    //
    // `||` with nothing between the pipes is a closure with no parameters.
    // All these closures capture `adj`/`pad` BY REFERENCE (they borrow them),
    // which is why no `move` keyword is needed.
    let t_adj = best(&||
        time(||
            std::thread::scope(|s| {
                s.spawn(|| hammer(&adj.a));
                s.spawn(|| hammer(&adj.b));
            })
        )
    );
    let t_pad = best(&||
        time(||
            std::thread::scope(|s| {
                s.spawn(|| hammer(&pad.a));
                s.spawn(|| hammer(&pad.b));
            })
        )
    );

    // Both threads run at the same time, each doing ITERS increments, so:
    //   ns/op  = latency of ONE increment as seen by one thread
    //          = wall time / ITERS            (ms -> ns is * 1e6)
    //   ops/ns = throughput of BOTH threads together
    //          = (2 * ITERS) / wall time in ns
    // Padded, each increment is an uncontended `lock add` (~3.5 ns on an i9-12900K);
    // adjacent, each one also waits for the line to come back (~16 ns there).
    let row = |label: &str, ms: f64| {
        let ns = ms * 1e6;
        println!("  {label:<28} {ms:8.1} {:8.2} {:8.3}", ns / ITERS as f64, 2.0 * ITERS as f64 / ns);
    };
    println!("2 threads x {ITERS} increments of their OWN counter:\n");
    println!("  {:<28} {:>8} {:>8} {:>8}", "", "ms", "ns/op", "ops/ns");
    row("adjacent (false sharing)", t_adj);
    row("padded (separate lines)", t_pad);
    println!("\n  slowdown from false sharing: {:.1}x", t_adj / t_pad);
    println!("\n(Needs >= 2 physical cores; on 1 core there's nothing to bounce between.)");
}
