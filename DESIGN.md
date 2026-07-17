# Design Notes

This document explains how the ring buffer works and why it is built the way it is.
It reads top to bottom: the guarantees first, then the protocol, then the trade-offs
behind it. Sections tied to code that hasn't landed yet say so explicitly.

## 1. What this is — and what it actually guarantees

`MpmcRingBuffer<T, N>` is a **bounded multi-producer / multi-consumer FIFO queue**:
a fixed array of `N` slots that any number of threads may push to and pop from
concurrently, based on Dmitry Vyukov's sequence-number design.

The progress guarantee is worth stating precisely, because the common shorthand
("lock-free queue") overclaims:

- **Lockless / non-blocking:** no operation ever takes a mutex or spins on a lock.
  `try_push` and `try_pop` return `false` immediately when the buffer is full or
  empty — the caller decides whether to retry, back off, or drop.
- **Not formally lock-free:** lock-freedom requires that *some* thread always makes
  progress in a bounded number of steps, no matter what the others do. This design
  does not meet that bar — §5 explains exactly why. It behaves as non-blocking under
  load in practice, but the formal claim would be false, so it isn't made.

## 2. Design decisions at a glance

| Decision | Choice | Why |
|---|---|---|
| Bounded vs unbounded | Bounded, power-of-two capacity | Index wrap is a bitmask (`& (N-1)`), not a modulo; footprint is fixed; nothing allocates on the hot path. Backpressure is a feature — an unbounded queue hides overload until it OOMs. |
| Storage | One contiguous slot array, `MpmcRingBuffer<T, N>` template | Cache-friendly layout; capacity known at compile time. |
| Element type (v1) | `static_assert(std::is_trivially_copyable_v<T>)` | Slots stay plain memory — no placement-new / destructor lifetimes racing with concurrent access. A real limitation, documented in §9. |
| Slot state | Per-slot `std::atomic<std::size_t> seq` — a sequence number, not a full/empty flag | One variable encodes empty/full *and which lap the slot is on*, which is what defeats ABA (§3). |
| Head / tail | Monotonic `enqueue_pos` / `dequeue_pos`, masked only when indexing | Counters never wrap ambiguously; full/empty falls out of comparing a slot's seq against your position. |
| Claim protocol | CAS loop (Vyukov), not `fetch_add` | `fetch_add` cannot fail, which silently turns push into a blocking call (§4). |
| API style | `try_push` / `try_pop` return `bool`; batch variants return the count actually moved | Non-blocking semantics made explicit in the API itself. |

## 3. The core protocol

Every slot carries a sequence number, initialised to its own index:

```
slots[i].seq = i        // at construction
```

Producers and consumers each advance a monotonically increasing position counter
(`enqueue_pos`, `dequeue_pos`). Position `pos` maps to slot `pos & (N-1)`, and the
slot's seq says whose turn it is:

- `seq == pos` → slot is free for the producer whose turn is `pos`
- `seq == pos + 1` → slot holds data for the consumer whose turn is `pos`
- anything else → another thread got there first, or the buffer is full/empty

**Enqueue:**

```
pos = enqueue_pos.load(relaxed)
loop:
    slot = &slots[pos & MASK]
    seq  = slot->seq.load(acquire)
    diff = (intptr_t)seq - (intptr_t)pos
    if diff == 0:                        // free, our lap
        if enqueue_pos.CAS_weak(pos, pos + 1): break   // slot claimed
        // CAS failed → pos was reloaded, retry
    else if diff < 0:
        return false                     // FULL — clean failure, nothing claimed
    else:
        pos = enqueue_pos.load(relaxed)  // lost a race; reload and retry
slot->data = item
slot->seq.store(pos + 1, release)        // publish: consumers may now see it
```

**Dequeue** is symmetric: the consumer at `pos` needs `seq == pos + 1`, claims via
CAS on `dequeue_pos`, reads the data, then stores `seq = pos + N` — marking the slot
free for the producer that arrives one full lap later.

**Why the signed difference?** Positions and sequence numbers increase forever and
eventually wrap the integer. A direct `<` comparison breaks at the wrap point;
comparing the *difference* as a signed value stays correct as long as the true gap
is under half the integer range — trivially true here, since a position and the seq
it inspects can never be more than one lap apart.

**Why this defeats ABA:** a slot reused ten laps later does not look "free" again to
a stale thread — its seq has advanced by `10 * N`. State that is ambiguous with a
full/empty flag is unambiguous with a lap-encoding counter, and no separate
tag/epoch bookkeeping is needed.

## 4. Why claim with CAS instead of fetch_add

The tempting implementation is `enqueue_pos.fetch_add(1)`: one wait-free atomic, no
retry loop. The problem: **fetch_add cannot fail.** The increment claims a slot
unconditionally — if the buffer is full, there is no way to un-claim it, so the
producer has no choice but to wait for a consumer to free that exact slot. `push`
silently becomes a *blocking* call, and one stalled consumer wedges every producer
behind it.

The CAS loop makes the claim conditional: a producer only advances `enqueue_pos`
after observing that the slot is actually free, and when the buffer is full it
returns `false` having touched nothing. That clean failure path is the entire point
of a `try_` API.

(A fetch_add "ticket" design is legitimate where blocking is acceptable — it has
lower claim-side contention. It may appear here later as a benchmark experiment; it
is not the core queue.)

## 5. Progress guarantees, precisely

**Lock-free**, formally: at every instant, *some* thread completes its operation in
a bounded number of its own steps, regardless of how other threads are scheduled —
including any of them being suspended indefinitely.

This queue fails that definition in one specific window. A producer that wins the
CAS and is then descheduled *before* its release-store to `seq` leaves a slot
claimed but unpublished. The consumer whose turn lands on that slot cannot complete
`try_pop` for that position until the producer wakes up and publishes. No thread
holds a lock — but system-wide progress now depends on one particular thread being
scheduled, which is exactly what lock-freedom forbids.

So the phrasing used throughout this repo is deliberate: **"lockless / non-blocking
bounded MPMC queue (Vyukov-style); per-operation progress is not formally
lock-free."** For an in-process queue with cooperating threads this is a standard,
well-understood trade. Surviving a thread that dies mid-operation would require
epochs or timeouts and is out of scope (§9).

## 6. Memory ordering

The intended ordering for each atomic operation, with the reasoning in one line. The
full audit — which release each acquire pairs with, and a witness for what breaks at
anything weaker — will be completed against the real code as the implementation
lands.

| Operation | Order | Why |
|---|---|---|
| `seq.load` before claiming | `acquire` | Pairs with the publishing `release` store below: it makes the previous owner's writes (the data, or its consumption) visible before this thread touches the slot |
| `seq.store` after write/consume | `release` | Publishes the slot's new state *and* everything this thread wrote before it |
| `enqueue_pos` / `dequeue_pos` CAS | `relaxed` | The position counters only arbitrate *who* gets a slot; the seq accesses do all the synchronisation of the slot's contents |
| Position reloads in the retry loop | `relaxed` | A stale value costs one extra retry, never correctness |

`seq_cst` everywhere would also be correct — just slower. A compile-time toggle
forcing `seq_cst` is planned so that the cost of the tuned orderings is measured,
not asserted.

## 7. False sharing and padding

`enqueue_pos` and `dequeue_pos` are the two hottest words in the structure, hammered
by disjoint sets of threads. If they share a cache line, every producer claim
invalidates that line in every consumer's cache and vice versa — false sharing that
can dominate the runtime of a queue like this.

Both counters are therefore aligned to `std::hardware_destructive_interference_size`
(with a guarded fallback for toolchains that don't ship it). Notably, that size is
**128 bytes on Apple Silicon**, not the 64 that most code hard-codes — an
`alignas(64)` pair of counters can still land within one interference boundary
there. Whether each *slot* also gets padded out (less interference) or packed tight
(better density) is a measurable trade, and will be decided by benchmark rather than
folklore.

## 8. Batch operations

`try_push_batch` / `try_pop_batch` move up to `count` elements per call while paying
for **one** position CAS, amortising the most contended atomic operation across the
whole batch.

Two semantics worth calling out:

- **Partial batches.** The claim is for `min(count, slots actually available)`: ask
  for 32 when 5 are free and you get 5 — never a blocked wait for 32, and never a
  reservation of slots that aren't free yet (that would reintroduce the fetch_add
  trap at batch scale, §4).
- **Contiguity.** Consumers advance in order, so slot readiness is contiguous from
  the current position: verifying that slot `pos + k - 1` is free for this lap
  proves all `k` slots are. That is what makes a single-CAS batch claim sound.

The concrete probe strategy (how the largest available `k` is found) is documented
here when the implementation lands.

## 9. Limitations (v1, by design)

- **Trivially copyable elements only** (`static_assert`ed). Move-only and
  non-trivial types need placement-new lifetime management inside slots — planned
  as future work, not silently unsupported.
- **Power-of-two capacity, at least 2** (`static_assert`ed). Buys mask-based
  indexing and simple lap arithmetic.
- **Bounded by design.** When full, `try_push` fails and backpressure becomes the
  caller's policy decision. Deliberate (§2), not a missing feature.
- **A thread that dies mid-operation can wedge the slot it claimed** (§5).
  Acceptable for in-process queues with cooperating threads; recovery is out of
  scope for v1.
