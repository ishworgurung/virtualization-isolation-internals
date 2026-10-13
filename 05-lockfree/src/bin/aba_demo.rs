//! Replay the ABA interleaving against an untagged and a tagged Treiber stack.
//!
//! There is only ONE real thread here. "T1" is the `pop_with_hook` call that
//! reads `head`, then pauses; "T2" is the closure it runs in that pause. This
//! reproduces, deterministically, an interleaving that with real threads would
//! happen only rarely (T1 would have to be descheduled at exactly that point).
//!
//! Nodes are numbered 0, 1, 2 and shown as A, B, C. The step-by-step picture
//! of what happens is in the module docs of `src/aba.rs`.
//!
//! Run:  cargo run --release --bin aba_demo
use lockfree::aba::{Pop, TreiberStack, NIL};

/// Node index -> letter (0 -> 'A', 1 -> 'B', ...) for readable output.
/// `b'A'` is the ASCII code of 'A' as a byte (65). Letters are consecutive in
/// ASCII, so 65 + 1 = 66 = 'B', 65 + 2 = 67 = 'C'; `as char` turns the number
/// back into a character.
fn name(i: u32) -> char {
    (b'A' + i as u8) as char
}
/// Render a stack snapshot top-down as "A -> B -> C".
/// Read the chain left to right: iterate the indices, turn each into a
/// one-letter `String`, collect them into a `Vec`, and join with " -> ".
/// (`|&i|` destructures the `&u32` the iterator yields into a plain `u32`.)
fn show(v: &[u32]) -> String {
    v.iter().map(|&i| name(i).to_string()).collect::<Vec<_>>().join(" -> ")
}

/// Run the scenario once, with or without the generation tag.
fn run(tagged: bool) {
    const A: u32 = 0;
    const B: u32 = 1;
    const C: u32 = 2;

    println!("--- {} head ---", if tagged { "TAGGED (index + generation)" } else { "UNTAGGED (index only)" });
    let treiber_stack = TreiberStack::new(3, tagged);
    treiber_stack.push(C);
    treiber_stack.push(B);
    treiber_stack.push(A);
    println!("initial stack            : {}", show(&treiber_stack.snapshot()));

    // Thread 1 starts pop(): reads head=A and A.next=B, then is descheduled.
    let outcome = treiber_stack.pop_with_hook(|| {
        println!("T1 read head=A, next=B ... T1 is now paused before its CAS");
        // Thread 2 runs to completion in the gap.
        let a = treiber_stack.pop().unwrap();
        let b = treiber_stack.pop().unwrap();
        println!("T2 popped {} then {}", name(a), name(b));
        treiber_stack.scribble_next(b, NIL); // T2 keeps using B for something else
        println!("T2 is now using {} elsewhere (its next pointer changed)", name(b));
        treiber_stack.push(a);
        println!("T2 pushed {} back        : stack is {}", name(a), show(&treiber_stack.snapshot()));
        println!("T1 resumes and executes CAS(head, A -> B)");
    });

    match outcome {
        // Untagged: head is A again, so the CAS "A? then B" succeeds wrongly.
        Pop::Popped(i) => println!(
            "T1's CAS SUCCEEDED and returned {}.\n  stack now              : {}\n  EXPECTED               : (A already re-pushed) A -> C\n  => CORRUPTION: node C is lost, B is both 'in use by T2' and on the stack.",
            name(i),
            show(&treiber_stack.snapshot())
        ),
        // Tagged: head is A with a newer generation, so the CAS fails.
        Pop::Retry => println!(
            "T1's CAS FAILED (generation changed), so T1 would retry.\n  stack still            : {}\n  => correct.",
            show(&treiber_stack.snapshot())
        ),
        Pop::Empty => unreachable!(), // the stack held three nodes
    }
    println!();
}

fn main() {
    run(false);
    run(true);
}
