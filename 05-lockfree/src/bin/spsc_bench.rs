//! Throughput: our SPSC ring vs std::sync::mpsc vs `Mutex<VecDeque>`.
//!
//! One producer thread sends 0..N; one consumer thread receives and checks the
//! sum (0 + 1 + ... + N-1 = N(N-1)/2), which proves nothing was lost or
//! duplicated.
//!
//! How each one waits when the queue is full/empty:
//!   * SPSC ring and `Mutex<VecDeque>`: spin (busy-wait with a CPU pause hint).
//!   * std::sync::mpsc::sync_channel: a real blocking channel — it spins
//!     briefly, then parks the thread and is woken by the other side. That
//!     wake-up machinery is part of what its number includes. (Since Rust 1.67
//!     std's channel is based on crossbeam-channel, whose bounded flavour is a
//!     lock-free array queue — which is why it comes so close to the ring.)
//!
//! Run:  cargo run --release --bin spsc_bench
//! Caveat: numbers depend heavily on core placement (same core pair? SMT
//! siblings? same CCX?). Treat ratios as indicative, not absolute.
use lockfree::spsc;
use std::collections::VecDeque;
use std::sync::{mpsc, Mutex};
use std::time::Instant;

const N: u64 = 10_000_000;
const CAP: usize = 1024; // every queue is bounded to this many items
                         // Gauss's formula: 0 + 1 + ... + (N-1) = N(N-1)/2. Pair the first and last
                         // terms (0 + (N-1)), the second and second-to-last (1 + (N-2)), ...: each pair
                         // sums to N-1 and there are N/2 pairs. For N = 10^7 that's ~5 * 10^13, which
                         // needs u64 (u32 tops out at ~4.3 * 10^9).
                         // Comparing sums is a cheap check: a lost item would make the sum too small,
                         // a duplicated one too large. (It would not catch reordering; the ring's
                         // unit tests check strict FIFO order.)
const EXPECT: u64 = N * (N - 1) / 2;

/// Check the checksum and print time + throughput.
/// Throughput = items / seconds / 10^6 ("M items/s"). `{name:<28}` pads the
/// name to 28 characters, left-aligned; `{:>8.1}` right-aligns in 8 columns
/// with 1 decimal place.
fn report(name: &str, secs: f64, sum: u64) {
    assert_eq!(sum, EXPECT, "{name}: lost or duplicated items!");
    println!("{name:<28} {:>8.1} ms   {:>8.1} M items/s", secs * 1e3, N as f64 / secs / 1e6);
}

/// Our wait-free ring. `move` closures hand each thread its own end.
fn bench_spsc() {
    let (mut p, mut c) = spsc::channel::<u64>(CAP);
    let t = Instant::now();
    let sum = std::thread::scope(|s| {
        s.spawn(move || {
            for i in 0..N {
                while p.push(i).is_err() {
                    std::hint::spin_loop();
                } // full: spin
            }
        });
        s.spawn(move || {
            let (mut got, mut sum) = (0, 0u64);
            while got < N {
                if let Some(v) = c.pop() {
                    sum += v;
                    got += 1;
                } else {
                    std::hint::spin_loop();
                } // empty: spin
            }
            sum
        })
        .join()
        .unwrap()
    });
    report("lock-free SPSC ring", t.elapsed().as_secs_f64(), sum);
}

/// The standard library's bounded channel (send blocks when full).
fn bench_mpsc() {
    let (tx, rx) = mpsc::sync_channel::<u64>(CAP);
    let t = Instant::now();
    let sum = std::thread::scope(|s| {
        // When the producer finishes, `tx` is dropped, which ends `rx.iter()`.
        s.spawn(move || {
            for i in 0..N {
                tx.send(i).unwrap();
            }
        });
        s.spawn(move || rx.iter().sum::<u64>()).join().unwrap()
    });
    report("std::sync::mpsc (bounded)", t.elapsed().as_secs_f64(), sum);
}

/// The naive approach: one lock around a double-ended queue. Every push and
/// pop contends on the same mutex (and the same cache line).
///
/// A bug that used to live here: the consumer spun while holding the lock.
/// Its time jumped between ~1 s and 30+ s from run to run, even pinned to the
/// same two P-cores. How it happened:
///   * The guard from `if let Some(v) = q.lock().unwrap().pop_front()` lives
///     until the end of the whole `if let ... else` (edition 2021), so the
///     "queue empty → spin_loop()" branch ran with the lock held.
///   * `std::sync::Mutex` is not fair: the thread that just unlocked usually
///     wins the next lock too, because the lock's cache line is still in its
///     core. So an empty-queue consumer could unlock and relock hundreds of
///     millions of times while the producer rarely got in — and without the
///     producer getting in, the queue stayed empty. A self-sustaining loop.
///   * Whether a run fell into that loop depended on timing noise early on.
///     Instrumented: fast runs had ~1–10 M empty polls; slow ones 60–300 M.
///     Context switches were a few hundred either way — nobody slept; the
///     time was all lock traffic.
///
/// Now the item is taken in its own statement, so the lock is released
/// before spinning. The lessons: hold a lock only for the critical section,
/// and watch where Rust drops temporaries. See the README section "Why the
/// mutex time jumped around" for the measurements.
fn bench_mutex() {
    let q = Mutex::new(VecDeque::<u64>::with_capacity(CAP));
    let t = Instant::now();
    let sum = std::thread::scope(|s| {
        s.spawn(|| {
            for i in 0..N {
                loop {
                    let mut g = q.lock().unwrap();
                    if g.len() < CAP {
                        g.push_back(i);
                        break;
                    }
                    drop(g); // release the lock before spinning, or the consumer can't drain
                    std::hint::spin_loop();
                }
            }
        });
        s.spawn(|| {
            let (mut got, mut tsum) = (0, 0u64);
            while got < N {
                // Take the item in its own statement so the lock is released at
                // the `;`. Writing `if let Some(v) = q.lock().unwrap().pop_front()`
                // instead would keep the guard (a temporary) alive until the end
                // of the whole `if let ... else` in edition 2021 — so the else
                // branch would spin *while holding the lock*. See bench_mutex's
                // doc comment for what that did to the timings.
                let item = q.lock().unwrap().pop_front();
                if let Some(v) = item {
                    tsum += v;
                    got += 1;
                } else {
                    std::hint::spin_loop();
                }
            }
            tsum
        })
        .join()
        .unwrap()
    });
    report("Mutex<VecDeque> (spinning)", t.elapsed().as_secs_f64(), sum);
}

fn main() {
    println!("{N} u64 items, ring capacity {CAP}\n");
    bench_mutex();
    bench_mpsc();
    bench_spsc();
}
