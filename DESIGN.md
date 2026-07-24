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
The implementation in `mpmc_ring_buffer.hpp` is this pseudocode almost line for
line, with the memory orders written out.

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

Every atomic operation, the order it uses, and what would go wrong with anything
weaker. MPMC single and batch ops share the same operations.

**MPMC**

| Operation | Order | Pairs with | Breaks if weaker |
|---|---|---|---|
| position load + retry reload | relaxed | — | nothing (stale → one retry) |
| slot `seq` load (probe) | acquire | the previous owner's release store | data race on the slot's `data` |
| position CAS (claim) | relaxed | — (RMW is atomic) | nothing — see note |
| slot `seq` store (publish / free) | release | the other side's acquire load | reader sees the new seq but stale `data` |
| constructor `seq` stores | relaxed | — | nothing — see note |
| `size_approx` loads | relaxed | — | racy by design |

**SPSC** (details in §10)

| Operation | Order | Pairs with | Breaks if weaker |
|---|---|---|---|
| own index load | relaxed | — | nothing (sole writer) |
| other index load | acquire | the other thread's release store | read/overwrite races the other side |
| own index store (publish) | release | the other thread's acquire load | reader sees the index move but stale data |

Two that look too weak but aren't. The **position CAS is relaxed** because it only
decides *who* owns a run of slots; the RMW is atomic and the counter has a single
modification order, which is all mutual exclusion needs — the seqs carry every data
hand-off. The **constructor's seq stores are relaxed** because construction finishes
before the buffer is handed to any thread, and that hand-off is itself a
synchronisation point.

The claim uses `compare_exchange_weak`: it can fail spuriously, but it's already in a
retry loop, weak avoids the inner loop that strong compiles to on LL/SC machines like
ARM, and a failed CAS reloads the position for free.

`RINGBUFFER_FORCE_SEQ_CST` collapses every order above to seq_cst — still correct, just
slower. Phase 6 measures what the tuned ordering actually buys.

## 7. False sharing

`enqueue_pos` and `dequeue_pos` are hammered by different sets of threads; on a
shared cache line they'd ping-pong on every claim. Both get aligned to
`std::hardware_destructive_interference_size`, with a fallback in
`cache_line.hpp` for toolchains that don't provide it.

The fallback turned out to be the path that actually runs on my dev machine:
AppleClang's libc++ doesn't ship the constant, and Apple Silicon cache lines are
128 bytes (`sysctl hw.cachelinesize`), not 64 — the usual `alignas(64)` would put
both counters inside one 128-byte boundary. Each *slot* is currently padded to
its own line too — neighbouring slots get written by different threads, so I'm
starting from the no-false-sharing layout. `RINGBUFFER_PACKED_SLOTS` drops that
padding for the density-vs-interference A/B in Phase 6. The tests assert the two
positions and the slot array each land on their own line.

## 8. Batch operations

`try_push_batch` / `try_pop_batch` move up to `count` elements for one CAS on the
position — the most contended operation, amortised over the whole batch. Everything
else scales with the batch (k data copies, k slot publishes on their own lines);
the single contended RMW on the position counter is what batching saves.

The claim is `min(count, what's actually free)`: ask for 32 with 5 free and you
get 5. Claiming all 32 up front would be the fetch_add trap again, just bigger.

### Finding the run: forward scan, not a last-slot probe

The obvious optimisation is to probe just the last slot — if `pos + k - 1` is free,
assume `pos … pos + k - 1` all are, and binary-search for the largest such k. That
would be wrong here. It assumes the free region is contiguous, and with several
consumers it isn't.

Consumers claim positions in order (the counter is monotonic) but can *finish* in
any order. Say N=8, four consumers have claimed positions 92–95, and the one for 95
finishes first: it stores `seq[95 & 7] = 103`, so the slot for position 103 reads
free while the slot for position 100 still holds position 92's unconsumed item. A
producer probing only slot 103 would claim 100–103 and overwrite that item.

So the free run has to be found by scanning forward from `pos` and stopping at the
first slot that isn't ready — `k` is the length of that contiguous run. It's k
acquire-loads, but on the per-slot lines the batch is about to write anyway, not on
the contended counter. Pop is symmetric (a slot is ready when its producer has
published).

The single CAS is still safe: if it succeeds, `pos` was unchanged, so no other
producer claimed into the slots we scanned, and consumers only ever move seq
forward — a slot seen free stays free until we write it. Publishing runs forward
(`pos`, `pos+1`, …) so a consumer can start on the early items while the later ones
are still being written.

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

## 11. How correctness is tested

The single-threaded suites pin down protocol behaviour first: full/empty edges,
FIFO order, wraparound over many laps, and a white-box check that each slot's
sequence number advances exactly one lap per reuse.

The stress suite is where the guarantees get earned. Producers push tagged items
(producer id and per-producer sequence number packed into 64 bits) while
consumers drain. Afterwards, per producer, the popped count must equal what was
pushed and the sequence numbers must sum to exactly M(M-1)/2 — count catches
loss and duplication, the sum catches corrupted values. While popping, each
consumer also asserts that any one producer's sequence numbers only ever
increase as it sees them; that's the per-producer FIFO guarantee, and it catches
reorderings the totals would miss. Configurations go up to 4 producers / 4
consumers, plus a deliberately tiny N=4 buffer under 8 threads, where every
operation lands on a full/empty boundary and claim races are constant.

A separate configuration runs the same checks with producers pushing and
consumers popping in random-sized batches mixed with single ops, so the batch
claim and its partial-return path get the same contention coverage.

All of it runs under ThreadSanitizer and under AddressSanitizer+UBSan in CI
(separate builds — TSan doesn't combine with ASan), with no suppressions. Races
in this kind of code are scheduling-dependent, so locally the stress test gets
run repeatedly under TSan rather than once.

## 12. Reading the numbers

Full data is in `results/`; reproduce with `scripts/run_benchmarks.sh`. These were
taken on an Apple M2 Pro (6 performance + 4 efficiency cores), unpinned, on an
active machine — directional, not a pinned-server benchmark.

| Measure (MPMC) | Result | Baseline |
|---|---|---|
| Throughput, 4P/4C, single op | ~5.0 M ops/s | mutex ~16 M |
| Throughput, 4P/4C, batch 64 | ~91 M ops/s | mutex ~62 M |
| Ping-pong latency, p50 / p99.9 | 169 ns / ~700 ns | mutex 2.7 µs / ~190 µs |
| End-to-end latency, saturated | ~61 µs | moodycamel ~6 ms |

**Batching is the throughput story.** At 4P/4C, single ops do ~5 M/s; batching lifts
that to ~91 M at batch 64 — one CAS amortised over k items, ~18×, and the gain is
largest exactly where contention is worst.

**Latency: lower median, bounded tail.** The tail matters more than the median here —
MPMC's p99.9 round-trip is ~700 ns against the mutex's ~190 µs (max 1.6 ms). A lock's
worst case is unbounded; a slot's isn't.

**Bounded is a feature.** Saturated, the unbounded moodycamel queue shows ~6 ms
end-to-end — items pile up with no backpressure — versus ~61 µs here. A different
guarantee, not a slower one.

**The honest part: single ops under oversubscription.** With 8 threads (4P/4C) on 6
performance cores, single-op MPMC (~5 M) trails the mutex (~16 M): a thread
descheduled mid-claim stalls its consumer (§5), and the mutex degrades more
gracefully. At 1P/1C, MPMC leads ~2×. This is macOS-specific — no thread pinning — so
a pinned Linux run with more cores is the fair contention test, and is future work.

**seq_cst costs nothing measurable here.** Forcing every order to seq_cst
(`RINGBUFFER_FORCE_SEQ_CST`) lands within run-to-run noise of the tuned
acquire/release. At these sizes the queue is bound by coherence traffic on the shared
counters, not by fences. The tuned orders stay the default — they can't be slower, and
matter more on other microarchitectures — but there's no speedup here worth claiming.

**Padding vs packing.** Padding each slot to its own line wins where it should — the
false-sharing case of single-op multiple producers (2P/2C: 8.0 vs 6.5 M/s padded vs
packed). Packing wins for batch and low-thread runs, sometimes a lot (4P/4C batch 64:
~150 vs ~91 M/s), where contiguous-slot runs make density pay. Padded stays the
default because it protects the worst case; `RINGBUFFER_PACKED_SLOTS` is there for
batch-heavy uses.
