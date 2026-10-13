//! Message-passing litmus test: why acquire/release exist.
//!
//! A "litmus test" is a tiny concurrent program whose possible outcomes reveal
//! what a memory model allows. This is the most important one:
//!
//! Writer:  data = 1;  flag = 1;
//! Reader:  wait until flag == 1;  then read data.   Must it see data == 1?
//!
//! Intuition says "obviously yes: data was written first". But "first" only
//! holds *inside the writer thread*. The CPU may make the two stores visible
//! to other cores in a different order (e.g. `data` still sits in the
//! writer's store buffer when `flag` reaches memory), and the compiler may
//! swap two stores that look independent to it. The orderings are how you
//! forbid that:
//!
//!   flag stored Release / loaded Acquire  -> YES, guaranteed by the language.
//!   flag stored Relaxed / loaded Relaxed  -> NO guarantee. The reader may see
//!                                            flag == 1 and stale data == 0.
//!
//! HONEST CAVEAT: x86-64 has Total Store Order — the hardware never reorders
//! two stores or two loads — so on x86 you will almost certainly see 0
//! violations even for Relaxed. The bug is still real: (a) the compiler may
//! reorder Relaxed accesses, and (b) on ARM/POWER/RISC-V the CPU does. Run this
//! on an AArch64 machine (e.g. an Arm cloud server) to see Relaxed fail; to check
//! algorithms without such hardware use a model checker like `loom`.
//! (The one reordering x86 does perform is a store followed by a load of a
//! *different* address; that is what `SeqCst` fences exist for.)
//!
//! Run:  cargo run --release --bin litmus
use std::sync::atomic::{AtomicU32, Ordering, Ordering::Relaxed};

/// Number of independent message-passing rounds per configuration.
const CELLS: usize = 4_000_000;

/// Run the test with the given ordering for the flag's store and load.
/// Returns how many times the reader saw the flag set but the data not yet.
fn run(store: Ordering, load: Ordering) -> usize {
    // One independent (data, flag) pair per iteration, so reader and writer
    // chase each other through memory without any synchronisation but the flag.
    // Everything is atomic, so even the "wrong" configuration has no undefined
    // behaviour — it is allowed to give a stale answer, not to corrupt memory.
    let data: Vec<AtomicU32> = (0..CELLS).map(|_| AtomicU32::new(0)).collect();
    let flag: Vec<AtomicU32> = (0..CELLS).map(|_| AtomicU32::new(0)).collect();

    // `thread::scope` lets the threads borrow `data`/`flag` from this function
    // and joins them all before returning. The value of the scope is the value
    // of its last expression: here, the reader's stale count.
    std::thread::scope(|s| {
        // Writer thread.
        s.spawn(|| {
            for i in 0..CELLS {
                data[i].store(1, Relaxed); // (1) write payload
                flag[i].store(1, store);   // (2) publish
            }
        });
        // Reader thread; its return value is the stale count.
        s.spawn(|| {
            let mut stale = 0;
            for i in 0..CELLS {
                // Busy-wait for the writer to reach cell i. `spin_loop` is a
                // hint to the CPU (x86 `pause`) that we are spinning; it saves
                // power and helps a sibling hyper-thread.
                while flag[i].load(load) == 0 { std::hint::spin_loop(); }
                if data[i].load(Relaxed) == 0 { stale += 1; } // saw flag but not data!
            }
            stale
        })
        .join()
        .unwrap()
    })
}

fn main() {
    println!("arch = {}", std::env::consts::ARCH);
    println!("{CELLS} message-passing rounds per configuration\n");
    let a = run(Ordering::Release, Ordering::Acquire);
    println!("Release store / Acquire load : {a} stale reads   (must always be 0)");
    let r = run(Relaxed, Relaxed);
    println!("Relaxed store / Relaxed load : {r} stale reads   (allowed to be > 0; 0 is normal on x86)");
}
