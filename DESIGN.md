# Design notes

How the queue works and why. Most of this is the MPMC core; the SPSC version is
at the end.

## 1. What this is

`MpmcRingBuffer<T, N>` is a bounded MPMC FIFO using Vyukov's sequence-number
scheme: a fixed array of N slots, any number of threads pushing and popping, no
mutexes. `try_push` / `try_pop` return false instead of waiting. It's lockless
but not formally lock-free — §5 says why. v1 takes trivially copyable `T` and a
power-of-two `N`, both `static_assert`ed.

## 2. Design decisions

| Decision | Choice | Why |
|---|---|---|
| Capacity | Bounded, power of two | Wrap is a bitmask, not a modulo; nothing allocates on the hot path. Bounded also gives backpressure instead of OOM. |
| Storage | One contiguous slot array | Cache-friendly; size known at compile time. |
| Element type | Trivially copyable only | Slots stay plain memory — no object lifetimes racing with concurrent access. |
| Slot state | Per-slot atomic sequence number | Encodes empty/full *and* which lap — the ABA defense (§3). |
| Head / tail | Monotonic counters, masked when indexing | No wrap ambiguity; full/empty falls out of the seq comparison. |
| Claiming | CAS loop, not `fetch_add` | `fetch_add` can't fail, so push couldn't either (§4). |

## 3. The protocol

Each slot's seq starts at its index (`slots[i].seq = i`). Producers and consumers
advance monotonic counters; position `pos` maps to slot `pos & (N-1)`, and the seq
says whose turn it is: `seq == pos` is free for the producer at `pos`,
`seq == pos + 1` holds data for the consumer at `pos`, anything else means someone
beat you to it or the queue is full/empty.

```
pos = enqueue_pos.load(relaxed)
loop:
    slot = &slots[pos & MASK]
    seq  = slot->seq.load(acquire)
    diff = (intptr_t)seq - (intptr_t)pos
    if diff == 0:                        // free, our lap
        if enqueue_pos.CAS_weak(pos, pos+1): break
    else if diff < 0: return false       // full
    else: pos = enqueue_pos.load(relaxed) // lost the race, reload
slot->data = item
slot->seq.store(pos + 1, release)        // publish
```

Dequeue mirrors it: wait for `seq == pos + 1`, CAS `dequeue_pos`, read, then store
`seq = pos + N` to free the slot for a lap later. The header follows this almost
line for line.

The comparison is a signed difference because the counters eventually wrap; `seq <
pos` breaks at the wrap point, the sign of the difference doesn't (the two are
never more than a lap apart). That's also the ABA defense — a slot reused ten laps
on has a seq that's moved by 10·N, so a stale thread's check just fails and it
retries. No tags, no epochs.

## 4. Why not fetch_add

`fetch_add(1)` would be simpler, but it can't fail: it claims a slot whether one is
free or not. On a full queue there's no way to give the claim back, so push turns
into a blocking wait and one stuck consumer wedges every producer. The CAS checks
the slot first and only then advances the counter, so a full queue is just `return
false`. (A `fetch_add` ticket queue is a real design where blocking is fine — lower
claim contention — but that's not this.)

## 5. Not lock-free, and why

Lock-freedom means some thread always makes progress in a bounded number of steps,
whatever the scheduler does. This queue misses that in one spot: a producer that
wins the CAS and gets descheduled before its release-store leaves the slot claimed
but unpublished, and the consumer whose turn lands there is stuck until it wakes.
No lock is held, but progress now depends on one specific thread — which is what
the definition rules out. For in-process cooperating threads that's a fine trade;
surviving a thread that dies mid-op would need epochs or timeouts, which is out of
scope. Hence the phrasing everywhere: lockless / non-blocking, not formally
lock-free.

## 6. Memory ordering

Every atomic op, its order, and what breaks if you weaken it. Single and batch ops
use the same operations.

| MPMC operation | Order | Pairs with | Breaks if weaker |
|---|---|---|---|
| position load / retry reload | relaxed | — | nothing (stale → one retry) |
| slot `seq` load (probe) | acquire | the prior owner's release store | data race on the slot's `data` |
| position CAS (claim) | relaxed | — (the RMW is atomic) | nothing — see below |
| slot `seq` store (publish/free) | release | the other side's acquire load | reader sees the new seq but stale `data` |
| constructor `seq` stores | relaxed | — | nothing — see below |

SPSC is the same shape: a thread's load of its own index is relaxed (it's the only
writer), the cross-thread index load is acquire, and the publishing store is
release.

The two that look too weak: the position CAS is relaxed because it only decides who
owns a run — the RMW is atomic and the counter has one modification order, which is
all mutual exclusion needs; the seqs carry the data. The constructor's seq stores
are relaxed because construction finishes before the buffer reaches any thread.

The claim is `compare_exchange_weak` — it can fail spuriously, but it's already in a
retry loop, weak skips the inner loop strong compiles to on ARM, and a failed CAS
reloads the position for free. `RINGBUFFER_FORCE_SEQ_CST` upgrades every order to
seq_cst; §12 has the measured cost (none, on Apple Silicon).

## 7. False sharing

`enqueue_pos` and `dequeue_pos` are written by different threads; on one cache line
they'd ping-pong every claim. Both are aligned to
`std::hardware_destructive_interference_size`, with a fallback in `cache_line.hpp`.
The fallback is the live path on my dev machine — AppleClang's libc++ doesn't ship
the constant, and Apple Silicon lines are 128 bytes (`sysctl hw.cachelinesize`),
not the 64 most code hard-codes. Each slot is padded to its own line too, since
neighbours are written by different threads; `RINGBUFFER_PACKED_SLOTS` turns that
off, and §12 has the padded-vs-packed numbers. Tests assert the two positions and
the slot array land on separate lines.

## 8. Batch operations

`try_push_batch` / `try_pop_batch` move up to `count` elements under one CAS on the
position. That CAS is the contended part; the rest (k copies, k publishes on their
own lines) scales with the batch anyway, so one CAS instead of k is the whole win.
The claim is `min(count, what's free)` — ask for 32 with 5 free and you get 5.

Finding the run is the subtle bit. The tempting version probes only the last slot
(`pos + k - 1`) and assumes everything before it is free too. That's wrong here:
consumers claim positions in order but finish in any order, so the free region can
have holes. Say N=8 and consumers hold 92–95; if the one for 95 finishes first,
`seq[95 & 7] = 103` makes slot 103 look free while slot 100 still holds 92's
unconsumed item — a probe of only 103 would overwrite it. So the run is found by
scanning forward from `pos` and stopping at the first slot that isn't ready. Those
are k acquire-loads, but on the per-slot lines the batch writes anyway, not the
contended counter. The single CAS is still safe: if it succeeds, `pos` didn't move,
so nobody claimed into the scanned slots, and consumers only push seq forward.
Publishing runs forward so a consumer can start on the early items while the later
ones are still being written.

## 9. Limitations (v1)

- Trivially copyable `T` only; move-only types would need in-slot construction.
- Power-of-two capacity, minimum 2. `static_assert`ed.
- Bounded — a full queue fails the push, by design.
- A thread that dies mid-op wedges its slot (§5).

## 10. The SPSC baseline

`spsc_ring_buffer.hpp` is the single-producer / single-consumer version. It came
first because it exercises the same machinery — masking, monotonic indices,
wraparound, release/acquire publish — with bugs that are deterministic instead of
scheduling-dependent, and it stays as a benchmark for what MPMC generality costs.

With one thread per side there are no claim races and no ABA, so it needs no
sequence numbers — two owned indices are enough (empty is `head == tail`, full is
`head - tail == N`). The one trick worth noting is the cached index: a push would
otherwise acquire-load `tail_` every call, a line the consumer keeps writing.
Instead the producer keeps a plain copy of the last `tail_` it saw, which is safe
because `tail` only advances — a stale copy underestimates free space, never
overshoots. It only re-reads the real `tail_` when the cached value says full.
Steady state, that's one shared-line read per N pushes instead of one per push; the
consumer mirrors it. Synchronisation is the two release/acquire pairs from §6.

## 11. How correctness is tested

Single-threaded tests pin down the protocol: full/empty edges, FIFO, multi-lap
wraparound, and a white-box check that each slot's seq advances one lap per reuse.

The stress suite earns the concurrency guarantees. Producers push items tagged with
(producer id, sequence number); afterward each producer's popped count must match
what went in and its numbers must sum to M(M-1)/2 — count catches loss and
duplication, the sum catches corruption — and each consumer checks that a producer's
numbers only increase as it sees them, catching reordering. Configs go to 4P/4C, a
tiny N=4 buffer under 8 threads (every op on a boundary), and random batches mixed
with single ops. Everything runs under TSan and ASan+UBSan in CI with no
suppressions; locally the stress test runs repeatedly under TSan, since the races
depend on scheduling.

## 12. Reading the numbers

Data is in `results/`; reproduce with `scripts/run_benchmarks.sh`. Taken on an
Apple M2 Pro (6 performance + 4 efficiency cores), threads unpinned, on an active
machine — directional, not a controlled benchmark.

| Measure (MPMC) | Result | Baseline |
|---|---|---|
| Throughput, 4P/4C, single op | ~5.0M ops/s | mutex ~16M |
| Throughput, 4P/4C, batch 64 | ~91M ops/s | mutex ~62M |
| Ping-pong latency, p50 / p99.9 | 169 ns / ~700 ns | mutex 2.7 µs / ~190 µs |
| End-to-end latency, saturated | ~61 µs | moodycamel ~6 ms |

Batching is where the throughput comes from: at 4P/4C, single ops manage ~5M/s and
batches of 64 reach ~91M — one contended CAS spread over k items, and the gain is
biggest where contention is worst. On latency the median beats the mutex ~16×, but
the tail is the real gap: p99.9 ~700 ns here vs the mutex's ~190 µs (1.6 ms worst
case), since a stalled lock blocks everyone while a stalled slot blocks only its own
consumer. moodycamel is faster at single ops but unbounded — under saturation it
climbs to ~6 ms end-to-end while this queue stays near 60 µs.

It loses one case: 4P/4C single ops, mutex ~16M vs ~5M here, because eight threads
oversubscribe six performance cores and a thread parked mid-claim stalls its
consumer (§5). At 1P/1C it's ahead ~2×. macOS won't pin threads, so a pinned Linux
run is the fair contention test — not done yet.

From the toggles: forcing seq_cst makes no measurable difference (the queue is
coherence-bound, not fence-bound), so the tuned orders stay default without a
speedup claim. Padding each slot to its own line wins the contended single-op case
(2P/2C: 8.0 vs 6.5M packed) but loses when batches dominate (4P/4C batch 64: ~150 vs
~91M); padded stays default because it protects the worse case.
