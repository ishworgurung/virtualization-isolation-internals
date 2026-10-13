//! # lockfree
//!
//! Small, heavily-commented lock-free building blocks:
//!
//! * [`spsc`]   — bounded single-producer/single-consumer ring (acquire/release,
//!                cached indices, cache-line padding). Same shape as a virtqueue
//!                or an io_uring ring.
//! * [`aba`]    — a Treiber stack that makes the ABA problem reproducible on demand,
//!                and the tagged-pointer fix.
//! * [`padded`] — `CachePadded<T>`, the cure for false sharing.
//!
//! The binaries in `src/bin/` exercise each one:
//! `spsc_bench`, `aba_demo`, `false_sharing`, `litmus`.
//!
//! ## Suggested reading order
//!
//! 1. This file: vocabulary + the bits refresher below.
//! 2. `padded.rs` (short) — what a cache line is.
//! 3. `spsc.rs` — the ring buffer. The most important file.
//! 4. `bin/litmus.rs` — why the orderings in `spsc.rs` matter.
//! 5. `aba.rs` + `bin/aba_demo.rs` — what goes wrong once you need CAS.
//! 6. `bin/false_sharing.rs`, `bin/spsc_bench.rs` — measure it.
//!
//! ## Vocabulary used throughout
//!
//! * **Thread**: an independent flow of execution. Two threads may run at the
//!   *same instant* on two CPU cores, or take turns on one core. Either way, you
//!   cannot predict where one thread is when another looks at shared memory.
//! * **Lock / mutex**: the classic way to share data: "only one thread may be in
//!   here at a time". Simple and correct, but a thread that is paused while
//!   holding the lock (descheduled, page-faulting) stalls everyone waiting on it.
//! * **Lock-free**: threads coordinate through atomic instructions instead of
//!   mutexes, and the system as a whole always makes progress — a thread that
//!   is paused can never block the others.
//! * **Wait-free** (stronger): *every* operation finishes in a bounded number of
//!   its own steps, no matter what other threads do. The SPSC ring is wait-free;
//!   the Treiber stack is only lock-free (a CAS may have to retry).
//! * **Atomic**: a memory access the CPU performs indivisibly (no torn reads or
//!   writes — you never see half of an old value and half of a new one) and that
//!   the compiler may not split, merge or elide. In Rust: `AtomicUsize`,
//!   `AtomicU32`, `AtomicU64`, ... with methods `load`, `store`, `fetch_add`,
//!   `compare_exchange`. (The word means other things in databases and
//!   distributed systems; see [Atomicity in other
//!   contexts](#atomicity-in-other-contexts).)
//! * **Memory ordering** (`Relaxed`, `Acquire`, `Release`, `AcqRel`, `SeqCst`):
//!   rules about which *other* memory accesses become visible to another thread
//!   together with an atomic one. See `spsc.rs` and the `litmus` binary.
//!   - `Relaxed`: "just make this one access atomic; no promises about others".
//!   - `Release` (on a store): "everything I wrote before this is published too".
//!   - `Acquire` (on a load): "if I see a Released value, I also see everything
//!     written before it".
//!   - `AcqRel`: both, for read-modify-write operations such as CAS.
//!   - `SeqCst`: Acquire/Release plus one global order all threads agree on.
//! * **CAS** (compare-and-swap, `compare_exchange` in Rust): "if X still equals
//!   OLD, set it to NEW; tell me whether it worked" — the basic lock-free
//!   read-modify-write building block. It is ONE hardware instruction
//!   (`lock cmpxchg` on x86), so no other thread can sneak in between the
//!   compare and the swap.
//! * **Happens-before**: if A happens-before B, then B sees A's effects.
//!   Within one thread, program order gives you this for free. Across threads,
//!   you only get it through a synchronising pair such as Release → Acquire.
//!
//! ## Atomicity in other contexts
//!
//! "Atomic" is an overloaded word. The common core is always **no observer
//! ever sees a half-done state**; what changes is *who* the observer is and
//! *what* could interrupt the operation halfway.
//!
//! | Context | "Atomic" means | Protects against | Scope |
//! |---|---|---|---|
//! | **CPU / Rust atomics** (this crate) | a single memory access is indivisible; no torn values | other threads running at the same time | one memory location, a few bytes |
//! | **Databases** (the A in ACID) | a transaction's writes happen all-or-nothing | a **crash** mid-transaction | many rows, one database |
//! | **Distributed atomic commit** (two-phase commit, 2PC) | every node commits, or every node aborts | node crashes, lost messages | many machines |
//! | **Atomic broadcast** (a.k.a. total-order broadcast) | every node delivers the same messages in the same order | nodes seeing events in different orders | many machines; equivalent to consensus (Raft, Paxos) |
//! | **Filesystems** ("atomic rename") | readers see the old file or the new one, never a partial one | crash during a write, concurrent readers | one path |
//!
//! **Trap: atomic is not isolated.** ACID atomicity is about *crashes*, not
//! concurrency; concurrency is the **I** (isolation). An "atomic" transaction
//! can still suffer race anomalies, just as a Rust atomic does not order your
//! *other* memory accesses (that is what `Acquire`/`Release` add). In both
//! worlds atomicity is the weaker guarantee, and ordering/isolation is a
//! separate one you must ask for.
//!
//! **Where the worlds meet: linearizability.** Distributed-systems theory has
//! the *atomic register* (Lamport): a shared variable whose every read and
//! write appears to take effect at a single instant somewhere between its call
//! and its return. That property is **linearizability**, which Jepsen calls
//! *atomic consistency*. Operations on a single CPU atomic are linearizable,
//! so an `AtomicU64` is effectively the one-machine, nanosecond-scale version
//! of what etcd or ZooKeeper provide across a cluster in milliseconds. The
//! ring in [`spsc`] and a Raft log are the same idea at very different scales.
//!
//! In one line: on a CPU, "atomic" = indivisible for other threads; in a
//! database, all-or-nothing across a crash; across a cluster, all nodes agree,
//! which needs consensus.
//!
//! ## Refresher: bits, hex, shifts and masks
//!
//! Lock-free code uses a handful of bit tricks. Here they are with numbers.
//!
//! **Binary and hex.** A `u8` has 8 bits; `0b0000_1101` is 13. Hex groups four
//! bits per digit: `0xD` = `0b1101` = 13, `0xFF` = `0b1111_1111` = 255.
//! `u32::MAX` is `0xFFFF_FFFF` (32 ones). Underscores are only for readability.
//!
//! **AND `&`** keeps a bit only if it is 1 in *both* inputs. Used to *extract*
//! or *test* bits:
//!
//! ```text
//!     13 = 0b1101
//!   &  7 = 0b0111
//!   -------------
//!      5 = 0b0101      (the top bit was "masked off")
//! ```
//!
//! **OR `|`** sets a bit if it is 1 in *either* input. Used to *combine* fields
//! that occupy different bits:
//!
//! ```text
//!     0b1100_0000
//!   | 0b0000_0011
//!   -------------
//!     0b1100_0011      (neither side overwrote the other)
//! ```
//!
//! **Left shift `<<`** moves bits towards the high end, filling with zeros.
//! `x << n` equals `x * 2^n` (as long as nothing falls off the top):
//! `1 << 3 = 0b1000 = 8`, `5 << 32 = 0x0000_0005_0000_0000`.
//!
//! **Right shift `>>`** moves bits towards the low end; the low bits fall off.
//! `x >> n` equals `x / 2^n` rounded down: `13 >> 2 = 3`, and
//! `0x0000_0005_0000_0002 >> 32 = 5`.
//!
//! **Casting to a narrower type truncates**: `0x0000_0005_0000_0002_u64 as u32`
//! keeps only the low 32 bits → `2`. Casting to a wider type zero-extends.
//!
//! **Powers of two and masks.** If `cap` is a power of two (2, 4, 8, 16...),
//! then `mask = cap - 1` is all ones below it (`8 - 1 = 7 = 0b0111`), and
//! `i & mask == i % cap` for every `i`. `%` (division) costs tens of cycles;
//! `&` costs one. That is why ring buffers insist on power-of-two sizes. It only
//! works for powers of two: `cap = 6` gives `mask = 5 = 0b101`, and `2 & 5 = 0`,
//! not `2`.
//!
//! **Wrapping arithmetic.** Integers have a fixed number of bits, so they wrap:
//! for `u8`, `255 + 1` → `0` and `0 - 1` → `255` (arithmetic mod 256). In
//! debug builds Rust panics on overflow, so code that *wants* wrapping says so:
//! `wrapping_add`, `wrapping_sub`. The useful property: `b.wrapping_sub(a)`
//! is still the true distance from `a` to `b` even if `b` has wrapped past zero
//! (see `spsc.rs` for a worked example).
//!
//! ### Unsigned vs signed integers
//!
//! **The one idea.** A number type is a fixed number of bits, so it has a fixed
//! number of *bit patterns*. 8 bits give 256 patterns, from `0000_0000` to
//! `1111_1111`. The type only decides **which number each pattern means**:
//!
//! * unsigned (`u8`): the patterns mean 0, 1, 2, ..., 255
//! * signed (`i8`): the patterns mean -128, ..., -1, 0, 1, ..., 127
//!
//! The bits in memory are identical. Only the *reading* differs.
//!
//! **A tiny 3-bit world.** 256 rows is too many to draw, so pretend numbers had
//! only 3 bits (8 patterns). Everything below works the same way for 8, 32 or
//! 64 bits.
//!
//! ```text
//!   bits   unsigned   signed
//!   000       0          0
//!   001       1          1
//!   010       2          2
//!   011       3          3
//!   100       4         -4     <- top bit is 1: signed reads it as negative
//!   101       5         -3
//!   110       6         -2
//!   111       7         -1
//! ```
//!
//! **Where the negative numbers come from: place values.** In ordinary binary
//! each bit is worth a power of two. Signed changes just one thing: **the top
//! bit is worth minus** its usual value.
//!
//! ```text
//!                 top
//!   bit worth:    4s   2s   1s
//!   unsigned:    +4   +2   +1
//!   signed:      -4   +2   +1      <- only the top bit's sign changes
//!
//!   101  unsigned:  4 + 0 + 1 =  5
//!        signed:   -4 + 0 + 1 = -3
//!
//!   111  unsigned:  4 + 2 + 1 =  7
//!        signed:   -4 + 2 + 1 = -1
//! ```
//!
//! Same for 8 bits, where the top bit is worth −128 instead of +128:
//!
//! ```text
//!   bit worth:   -128  64  32  16   8   4   2   1
//!   1111_1011:     1    1   1   1   1   0   1   1
//!                -128 +64 +32 +16  +8      +2  +1  = -5
//! ```
//!
//! That gives a quick shortcut. If the top bit is 0, both readings agree. If
//! the top bit is 1, it swings from +4 to −4 (a difference of 8, the number of
//! patterns), so signed = unsigned − 8. For 8-bit types, − 256:
//!
//! ```text
//!   1111_1111 (0xFF):   as u8 = 255   as i8 = 255 - 256 = -1
//!   1111_1011 (0xFB):   as u8 = 251   as i8 = 251 - 256 = -5
//!   1000_0000 (0x80):   as u8 = 128   as i8 = 128 - 256 = -128
//! ```
//!
//! (This scheme is called *two's complement*. Every modern CPU uses it.)
//!
//! **Think of a circle, not a line.** The patterns sit on a circle like the
//! numbers on a clock. Adding 1 moves one step clockwise; after the last
//! pattern you are back at the first:
//!
//! ```text
//!                     000
//!                    0 / 0
//!          111                  001
//!        7 / -1                1 / 1
//!
//!      110                          010
//!     6 / -2                       2 / 2
//!
//!          101                  011
//!        5 / -3                3 / 3
//!                     100
//!                   4 / -4
//!
//!    (each label is  unsigned / signed;  +1 = one step clockwise)
//! ```
//!
//! Unsigned and signed walk the **same circle**; they only print different
//! labels. That is why the CPU needs just one "add" instruction for both. It is
//! also why -1 is all ones (`111`, `0xFF`): it is one step back from zero.
//!
//! **The seam: overflow.** Somewhere on the circle the labels jump:
//!
//! * unsigned: between 7 and 0 (for `u8`, between 255 and 0)
//! * signed: between 3 and -4 (for `i8`, between 127 and -128)
//!
//! Crossing the seam is called **overflow**: `255_u8 + 1`, `0_u8 - 1`,
//! `127_i8 + 1`. The bits just keep going round the circle; the only question
//! is whether you *wanted* that.
//!
//! Rust's default: debug builds **panic** when you cross the seam, release builds
//! silently **wrap** (`127_i8 + 1` → -128). Don't depend on the build mode;
//! say what you want:
//!
//! | Method | `250_u8 + 10` gives | Use when |
//! |---|---|---|
//! | `wrapping_add` | `4` | going round the circle is intended (ring counters, hashes, tags) |
//! | `checked_add` | `None` | overflow is an error you want to handle |
//! | `saturating_add` | `255` | stop at the edge instead of going round |
//! | `overflowing_add` | `(4, true)` | you need the result *and* whether it wrapped |
//!
//! (In C and C++, crossing the seam with a *signed* type is undefined behaviour:
//! the compiler may assume it never happens. Rust has no such trap.)
//!
//! **Converting with `as`: same bits, new label.** `as` never checks anything.
//! It only relabels or resizes:
//!
//! * **Same size**: keep the bits, read them the other way.
//!   `-1_i8 as u8` = 255, `200_u8 as i8` = -56 (200 - 256).
//! * **To a bigger type**: add bits at the top. Unsigned adds 0s, so
//!   `255_u8 as i32` = 255. Signed adds copies of its top bit, so negatives stay
//!   negative: `-1_i8 as i32` = -1 (all 32 bits are 1: `0xFFFF_FFFF`).
//! * **To a smaller type**: chop off the top bits. `300_u32 as u8` = 44, because
//!   300 = `1_0010_1100` and the low 8 bits `0010_1100` are 44.
//!
//! Want a check instead? `u8::try_from(300)` returns an error rather than chopping.
//!
//! **Right shift keeps the sign for signed types.** `>>` slides every bit one
//! place right, which leaves an empty spot at the top. Unsigned fills it with 0.
//! Signed fills it with a copy of the top bit, so a negative number stays
//! negative (and it still halves the number):
//!
//! ```text
//!   u8:  1111_1000 (248)  >> 1  =  0111_1100 (124)
//!   i8:  1111_1000 (-8)   >> 1  =  1111_1100 (-4)
//! ```
//!
//! **Negating by hand** (rarely needed): flip every bit, then add 1.
//!
//! ```text
//!    5  = 0000_0101
//!   flip  1111_1010
//!   +1    1111_1011  = -5
//! ```
//!
//! **Two signed surprises.**
//!
//! * There is one more negative number than positive: `i8` goes down to -128 but
//!   only up to +127 (in the 3-bit world: down to -4, up to +3). So -128 has no
//!   positive partner, and `(-128_i8).abs()` and `-128_i8 / -1` overflow.
//! * Division rounds toward zero, and `%` keeps the sign of the left number:
//!   `-7 / 2 = -3` and `-7 % 2 = -1`. For a remainder that is never negative
//!   (e.g. to use as an array index), use `rem_euclid`: `(-7_i32).rem_euclid(2) = 1`.
//!
//! **Why this crate uses unsigned types** (`usize`, `u32`, `u64`): indexes,
//! counters and bit patterns are never negative. With unsigned types, `&` masks,
//! `>>` shifts and wrap-around all behave exactly like the pictures above, with
//! no sign bit to worry about.

// Each `pub mod x;` pulls in src/x.rs as a public module of the library.
pub mod aba;
pub mod padded;
pub mod spsc;

/// Checks the real-Rust-type numbers quoted in the "Unsigned vs signed integers" refresher.
#[cfg(test)]
mod refresher_tests {
    #[test]
    fn signed_unsigned_examples() {
        assert_eq!((0xFF_u8 as i8, 0x80_u8 as i8), (-1, -128));
        assert_eq!(0b1000_0001_u8 as i8, -127);
        assert_eq!(-5_i8 as u8, 0xFB);
        assert_eq!(0xFB_u8.wrapping_add(0x07), 0x02);
        assert_eq!((-1_i8 as u8, 200_u8 as i8), (255, -56));
        assert_eq!((255_u8 as i32, -1_i8 as i32), (255, -1));
        assert_eq!(-1_i8 as i32 as u32, 0xFFFF_FFFF);
        assert_eq!(300_u32 as u8, 44);
        assert_eq!(300_u32, 0b1_0010_1100);
        assert!(u8::try_from(300_u32).is_err());
        assert_eq!(0x80_u8, 128);
        assert_eq!(!5_i8 + 1, -5);
        assert_eq!((0xF8_u8 >> 1, -8_i8 >> 1), (0x7C, -4));
        assert_eq!(127_i8.wrapping_add(1), -128);
        assert_eq!(250_u8.wrapping_add(10), 4);
        assert_eq!(250_u8.checked_add(10), None);
        assert_eq!(250_u8.saturating_add(10), 255);
        assert_eq!(250_u8.overflowing_add(10), (4, true));
        assert_eq!((-128_i8).checked_abs(), None);
        assert_eq!((-128_i8).checked_div(-1), None);
        assert_eq!((-7 / 2, -7 % 2, (-7_i32).rem_euclid(2)), (-3, -1, 1));
    }
}
