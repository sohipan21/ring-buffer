# Design Notes

How the ring buffer works and why it's built this way. The MPMC protocol is the
interesting part, so it gets most of the doc; the SPSC baseline is at the end.

## 1. What this is

`MpmcRingBuffer<T, N>` is a bounded multi-producer / multi-consumer FIFO built on
Dmitry Vyukov's sequence-number design: a fixed array of N slots, any number of
threads pushing and popping concurrently, no mutexes anywhere.

One claim up front, because "lock-free queue" gets thrown around loosely: this
queue is lockless and non-blocking — `try_push` / `try_pop` never wait, they
return false when the buffer is full or empty — but it is *not* formally
lock-free. Section 5 explains the difference and exactly where this design gives
up the formal guarantee.

## 2. Design decisions

| Decision | Choice | Why |
|---|---|---|
| Capacity | Bounded, power of two | Wrap is a bitmask instead of a modulo; nothing allocates on the hot path. Bounded is also a feature: backpressure instead of OOM. |
| Storage | One contiguous slot array | Cache-friendly; capacity fixed at compile time. |
| Element type (v1) | Trivially copyable only | Slots stay plain memory — no object lifetimes racing with concurrent access. |
| Slot state | Per-slot atomic sequence number | Encodes empty/full *and* which lap — that's the ABA defense (§3). |
| Head / tail | Monotonic counters, masked only when indexing | No wrap ambiguity; full/empty falls out of seq comparisons. |
| Claiming | CAS loop, not fetch_add | fetch_add can't fail, so push couldn't either (§4). |

## 3. The protocol

Each slot's seq starts at its own index (`slots[i].seq = i`). Producers and
consumers advance monotonic counters (`enqueue_pos` / `dequeue_pos`); position
`pos` uses slot `pos & (N-1)`. The seq says whose turn the slot is:

- `seq == pos` — free, the producer at `pos` may claim it
- `seq == pos + 1` — holds data, the consumer at `pos` may claim it
- anything else — someone else got here first, or the buffer is full/empty

Enqueue:

```
pos = enqueue_pos.load(relaxed)
loop:
    slot = &slots[pos & MASK]
    seq  = slot->seq.load(acquire)
    diff = (intptr_t)seq - (intptr_t)pos
    if diff == 0:                        // free, our lap
        if enqueue_pos.CAS_weak(pos, pos+1): break   // claimed
    else if diff < 0:
        return false                     // full — nothing claimed
    else:
        pos = enqueue_pos.load(relaxed)  // lost the race, retry
slot->data = item
slot->seq.store(pos + 1, release)        // publish
```

Dequeue mirrors it: wait for `seq == pos + 1`, CAS `dequeue_pos`, read the data,
then store `seq = pos + N` so the slot is free for the producer one lap later.

The signed difference matters because the counters eventually wrap. A plain
`seq < pos` breaks at the wrap point; the sign of the difference doesn't, as long
as the two values are within half the integer range of each other — and they can
never be more than one lap apart.

This is also the ABA defense. A slot reused ten laps later has a seq that's
advanced by 10·N, so a stale thread's comparison just fails and it retries. No
tags, no epochs.

## 4. Why not fetch_add

`fetch_add(1)` on the position would be simpler and cheaper. But fetch_add can't
fail: the increment claims a slot whether or not one is free. When the buffer is
full there's no way to give the claim back — the producer *has* to wait for a
consumer, push silently becomes blocking, and one stalled consumer wedges every
producer behind it. The CAS loop checks the slot first and only then advances the
counter, so a full buffer is just `return false`.

(A fetch_add "ticket" queue is a real design with lower claim contention, for
when blocking is acceptable. Might show up here later as a benchmark comparison.)

## 5. Not lock-free, and why

Lock-freedom means: at every moment, *some* thread completes its operation in a
bounded number of its own steps, no matter how the others are scheduled. This
queue misses that in one window: a producer that wins the CAS and gets
descheduled before its release-store leaves the slot claimed but unpublished, and
the consumer whose turn lands there can't proceed until that producer wakes up.
Nobody holds a lock, but progress depends on one specific thread getting
scheduled — which is what the definition forbids.

So the phrasing used everywhere in this repo is "lockless / non-blocking, not
formally lock-free". For an in-process queue with cooperating threads that's a
normal trade; surviving a thread that dies mid-operation would need epochs or
timeouts, and is out of scope.

## 6. Memory ordering

Intended orders — the full audit gets finished against the real code:

| Op | Order | Why |
|---|---|---|
| seq load before claiming | acquire | Pairs with the release store below; makes the previous owner's writes visible first |
| seq store after write/consume | release | Publishes the new state plus everything written before it |
| position CAS | relaxed | Positions only arbitrate who gets a slot; the seqs carry the data synchronisation |
| position reloads on retry | relaxed | A stale value costs one extra retry, nothing worse |

seq_cst everywhere would also be correct, just slower. I plan to add a toggle
that forces seq_cst so the difference is a measurement rather than a guess.

## 7. False sharing

`enqueue_pos` and `dequeue_pos` are hammered by different sets of threads; on a
shared cache line they'd ping-pong on every claim. Both get aligned to
`std::hardware_destructive_interference_size`, with a fallback in
`cache_line.hpp` for toolchains that don't provide it.

The fallback turned out to be the path that actually runs on my dev machine:
AppleClang's libc++ doesn't ship the constant, and Apple Silicon cache lines are
128 bytes (`sysctl hw.cachelinesize`), not 64 — the usual `alignas(64)` would put
both counters inside one 128-byte boundary. Whether each *slot* also gets padded
out is a density-vs-interference trade I'll decide from benchmarks.

## 8. Batch operations

`try_push_batch` / `try_pop_batch` move up to `count` elements for one CAS on the
position — the most contended operation, amortised over the whole batch.

The claim is `min(count, what's actually free)`: ask for 32 with 5 free and you
get 5. Claiming all 32 up front would be the fetch_add trap again, just bigger.
And the reason one CAS can safely take k slots at all: consumers advance in
order, so readiness is contiguous — if slot `pos + k - 1` is free for this lap,
everything before it is too.

## 9. Limitations (v1)

- Trivially copyable T only; move-only types would need placement-new lifetime
  management in the slots. Future work.
- Power-of-two capacity, minimum 2. Static-asserted.
- Bounded — a full buffer fails the push, on purpose.
- A thread dying mid-operation wedges its slot (§5).

## 10. The SPSC baseline

`spsc_ring_buffer.hpp` is the single-producer / single-consumer version. It came
first because it exercises the shared machinery — masking, monotonic indices,
wraparound, the release/acquire publish — with failures that are deterministic
instead of scheduling-dependent. It also stays: comparing it against the MPMC
queue on a 1-producer/1-consumer workload measures what the MPMC generality
costs.

With one thread per side there are no claim races and no ABA, so no sequence
numbers needed: two owned indices are enough. Empty is `head == tail`, full is
`head - tail == N`. Each index sits on its own interference-size line next to the
data only its owner touches.

The optimisation worth knowing about is the cached index. Naively, every push
acquire-loads `tail_` — a line the consumer writes constantly, so that's a
coherence miss whenever the consumer has moved. Instead the producer keeps a
plain non-atomic copy of the last `tail_` it saw. That's safe because `tail` only
advances: a stale copy can only *underestimate* the free space, never invent it.
Only when the cached copy says "full" does the producer do a real acquire load
and re-check. Steady-state, that's one shared-line read per N pushes instead of
one per push. The consumer mirrors it with a cached `head_`.

Synchronisation is two release/acquire pairs. Producer's release store of `head_`
against the consumer's acquire load: the element is fully written before the
consumer can see the new head. Consumer's release store of `tail_` against the
producer's acquire load: the slot is fully read before the producer can overwrite
it. A thread's loads of its *own* index are relaxed — it's the only writer. The
two-thread test runs under TSan to keep all of this honest.
