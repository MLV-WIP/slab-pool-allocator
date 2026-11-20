# Concepts Guide

Learning-oriented companion to the library. This document explains *what*
problems **pools**, **slabs**, size classes, bitmaps, locks, and intrusive
lifetime tracking are solving — and *why* those ideas show up in production
systems — with room for an educational tone that the more operations-focused
[IMPLEMENTATION.md](IMPLEMENTATION.md) keeps lighter on.

**Pool and slab are different ideas.** A pool is a *lifetime / ownership /
isolation* boundary for a set of allocations. A slab is a *layout and recycling*
strategy for fixed-size slots. This library’s `Pool` type *uses* many `Slab`s
(plus a large-allocation path); neither concept subsumes the other.

**Terminology:** Pool → **Slab** (size-class engine) → **Block** (mapped chunk) →
**Slot**. See [terminology-glossary.md](terminology-glossary.md).

For signatures, parameters, and contracts, see [API.md](API.md). Build and run
instructions stay in the [root README](../README.md).

**Framing:** The code is aimed at production services first. Clear structure and
solid defaults also make it a strong base for teaching allocator and concurrency
fundamentals. Teaching is a byproduct of the same design choices — not a reason
to weaken them.

---

## Table of contents

- [How to read this guide](#how-to-read-this-guide)
- [What is a pool, and why use one?](#what-is-a-pool-and-why-use-one)
- [What is a slab, and why use one?](#what-is-a-slab-and-why-use-one)
- [How this library composes them](#how-this-library-composes-them)
- [Size classes & fragmentation](#size-classes--fragmentation)
- [Headers beside the payload](#headers-beside-the-payload)
- [Slots, bitsets, and growth](#slots-bitsets-and-growth)
- [Finding a pointer’s owning Block](#finding-a-pointers-owning-block)
- [Locking & contention](#locking--contention)
- [RAII on a custom pool](#raii-on-a-custom-pool)
- [Observing lifetime without owning](#observing-lifetime-without-owning)
- [Putting the pieces together](#putting-the-pieces-together)
- [Concepts illustrated](#concepts-illustrated)
- [Choosing a memory strategy](#choosing-a-memory-strategy)
- [Terminology](#terminology)
- [Further reading](#further-reading)

---

## How to read this guide

Suggested path:

1. [Root README](../README.md) — build, test, demos; get something running  
2. **This doc** — build intuition for the moving parts  
3. [API.md](API.md) — call the types correctly  
4. [IMPLEMENTATION.md](IMPLEMENTATION.md) — invariants and trade-offs when you
   change or audit the code  

You can also skim sections out of order. Each section tries to stand alone with
a short “why it matters” and a pointer into API or IMPLEMENTATION when you want
mechanics.

---

## What is a pool, and why use one?

A **memory pool** is a container for allocations that share a common
**lifetime, ownership, or isolation boundary**. You allocate *from* the pool;
tearing down the pool tears down (or otherwise ends) that whole set of
resources as a unit.

That is a different question from “how are the bytes laid out inside?” Layout
strategies (including slabs) are optional machinery *inside* a pool. Historically
many pools were simple arenas (bump pointers); others used freelists or slabs.
The pool idea stands on its own.

### Why pools show up in servers

**Disposable lifetime (arena / region style).**  
Create a pool for a request, a connection, or a job; allocate freely while that
unit of work runs; destroy the pool and reclaim everything without chasing each
pointer. Apache’s APR pools popularized this model for decades: pools nested by
lifetime, cleared or destroyed when the request or process phase ended. The win
is operational — bulk reclaim and fewer leaked “forgot to free” paths — not
necessarily faster individual `malloc`s. In this library that is the **arena**
path: `~Pool` frees slabs while `unique_pool_ptr` deleters no-op (and skip
`~T()`). Prefer **orderly** teardown (destroy smart pointers first) whenever
objects need real destructors.

**Isolation.**  
Separate pools keep one subsystem’s memory (and failures) from silently sharing
fate with another: per-tenant caches, per-thread or per-shard heaps, “scratch”
vs “long-lived” data. Isolation can be about *correctness* (who owns what),
*blast radius* (OOM or corruption confined), or *accounting* (how much did this
feature retain?).

**Contention and locality.**  
Many threads hammering one global heap contend on shared structures. Giving each
worker, CPU, or service shard its own pool shrinks that sharing. Even when the
backend is a slab allocator, the *pool* is what you instantiate multiple times
to partition traffic.

**A single place to policy.**  
Limits, alignment defaults, trimming, and “return RSS now” (`trim`) attach
naturally to a pool object your code can name and pass around — not to every
individual allocation site.

### When a pool is the right abstraction

- Work has a clear begin/end (request, session, batch) and you want bulk
  teardown or a named owner for all related buffers  
- You want multiple independent allocators (shards) rather than one process-wide
  heap  
- You need a handle to pass into factories / STL allocators (`Pool&`,
  `PoolHandle`, or `PoolLifetimeToken`)  

### When you may not need a named pool

- A single global heap with careful unique/shared ownership is enough  
- Objects outlive any convenient “phase” and must be freed individually forever  

In this library, `spallocator::Pool` is that named boundary: construct it (or
`PoolHandle::create()` for a heap domain that auto-retires when idle), hand it
to `allocate` / `make_pool_*` / `PoolAllocator`, destroy it when the unit of
work (or the service component) is done. What happens *inside* each size class
is the slab story below.

API: [API.md — Pool](API.md#pool).

---

## What is a slab, and why use one?

A **slab** is a storage strategy: take one or more large **Blocks** (mapped
chunks), carve each into equal **Slots** of one size, and recycle those Slots.
Allocation is “find a free Slot”; free is “mark that Slot free again.”

The classic references are kernel object caches (Bonwick’s SLAB; Linux
SLAB/SLUB) and user-space size-class caches (for example memcached). Games and
servers often reinvent a “pool of widgets” that is, under the hood, one slab (or
a few) for a hot type.

**Mental model:** instead of asking the general heap for every object, ask a
specialist that already owns a tray of identical cells.

### What problem slabs solve

The general-purpose heap (`new` / `delete`, `malloc` / `free`) must accept
almost any size. That flexibility costs:

- Searching freelists or bins for a fit  
- Coalescing on free to fight fragmentation  
- Lock or metadata contention under many threads  
- **External fragmentation** over long runs: free memory exists but not in a
  usable shape  

If the hot path allocates and frees the *same few sizes* millions of times, you
pay general-purpose overhead for a special-case workload. A slab turns that into
bit flips (or freelist pops) inside a pre-sized Block.

### When slabs help

- Many short-lived objects of a handful of discrete sizes  
- Profiles show allocator time on those sizes  
- You want more predictable per-alloc latency than a general heap guarantees  

### When slabs are the wrong tool

- Rare, huge, or wildly variable sizes — bookkeeping can cost more than a direct
  system allocation (this library’s `SlabProxy` path exists for oversized
  requests)  
- One-shot allocations with no reuse  
- Adversarial or unknown size distributions where a general heap’s flexibility
  wins  

**Takeaway:** slabs are a *workload-shaped layout* optimization. The art is
choosing which sizes deserve a class and when to fall back to the system
allocator. That is independent of whether those slabs live inside one pool or
many.

API: [API.md — Slab & SlabProxy](API.md#slab--slabproxy).

---

## How this library composes them

```text
  Pool                            ← lifetime / routing / isolation boundary
    ├── Slab<ElemSize, Align>     ← size-class engine (alignment class 16 / 32 / 64)
    │     └── Block               ← one mapped chunk
    │           └── Slot          ← one ElemSize cell (freemap unit)
    └── SlabProxy                 ← oversized: system new[] / delete[] (no Block/Slot recycle)

  Item (allocateItem) = backend allocation at the slab layer (= slot base for Slab).
  User pointer (Pool::allocate) = slot base + header.
```

| Concept | Role here |
|---------|-----------|
| **`Pool`** | Public facade: choose a backend by footprint + alignment, write the allocation header, own the set of slabs, provide `trim` / accounting, synchronize routing |
| **`Slab`** | Per–size-class engine: zero or more **Blocks**, freemap bits, address map, per-slab lock, growth / reclaim |
| **`SlabProxy`** | Same *interface* as a slab backend, different *strategy* (no Block/Slot recycling) |

Destroying a `Pool` destroys its slabs and drops that whole allocation domain.
Using several `Pool` instances isolates domains (for example per shard). Speeding
up alloc/free of 64-byte objects is primarily a **slab** concern; naming “the
memory for this request handler” is primarily a **pool** concern.

The sections below (size classes, bitsets, address lookup) describe slab
mechanics and how the pool *routes* into them — not a single blended “slab pool”
abstraction.

---

## Size classes & fragmentation

### Rounding up instead of exact fit

Maintaining a separate freelist for every possible byte size is impractical.
Instead, each request is rounded up to a **size class** — a discrete slot size
such as 16, 32, 48, 64, … — and handed to the `Slab` that owns that class. The
`Pool` picks which class (and which slab object); the slab owns the slots.

A pure power-of-two ladder (16, 32, 64, 128, …) is simple but can waste a lot of
space (a 33-byte need may jump to 64). **Intermediate classes** (48, 96, 192,
384, 768) reduce that waste while keeping the number of classes manageable.

```
Alignment class 16 (eager): 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024
Alignment class 32:  32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
Alignment class 64:  64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
```

### Internal vs external fragmentation

| Term | Meaning | In this design |
|------|---------|----------------|
| **Internal fragmentation** | Unused bytes *inside* an allocated slot | Slot size − (user bytes + header). You “over-buy” a cell. |
| **External fragmentation** | Free memory exists but cannot satisfy the next request | Largely avoided *within* a size class: every free Slot is the right shape for that class. Cross-class unused Blocks are a different RSS concern (`trim`). |

### Footprint, not bare user size

This library’s `Pool` stores a small **header** in front of your payload (see
next section) so free does not need a size argument. Class selection uses:

```text
footprint = user_size + header_size
```

not `user_size` alone.

**Example (default alignment 8 → 8-byte header):**

| User asks for | Footprint | Lands in |
|---------------|-----------|----------|
| 100 bytes | 108 | **128B** class (108 > 96) |
| 128 bytes | 136 | **192B** class |
| 16 bytes | 24 | **32B** class |

If you compare “100 vs 128” without the header, you mis-predict the class and
mis-estimate waste. Always reason in footprints when studying behavior or
writing tests.

### Alignment classes

Alignment is part of the size-class story:

- Request align **4 or 8** shares the native **16-byte** alignment class (slots may be
  over-aligned; that is fine).  
- Align **32** or **64** uses a **separate ladder** so the user pointer can meet
  a stricter align while the header still fits in front.  
- Higher alignment-class **engines** are installed with the Pool; **Blocks** on
  those ladders often map on first use (prealloc knobs control warm start).  

Contracts for `allocate(size, alignment)`: [API.md — Pool](API.md#pool).

### Why this ladder shape (short)

Real allocators see the same skew repeatedly: **most calls** are small objects;
**most bytes** sit in fewer large ones. A process-global malloc (jemalloc /
TCMalloc style) answers with a dense small-size class table. This library is a
**named domain** pool: a coarser jemalloc-shaped geometric ladder on the hot
alignment class, then a large fallback, is intentional — not a goal to match TCMalloc’s
class count.

Practical takeaways:

- Keep the shipped alignment-class-16 shape unless a **client size histogram** says
  otherwise; densifying without data buys RSS and engine count for little gain.
- If you do retune spacing, bias the **bottom** of alignment class 16 (header-inflated
  small footprints), not the rare large end.
- Default cold-start prealloc (alignment class ≤16, ElemSize ≤256) warms where call rate
  concentrates; raising it is an RSS vs first-touch tradeoff, not a research
  mandate.

Full empirical notes and knobs:
[IMPLEMENTATION.md — Ladder and prealloc sizing](IMPLEMENTATION.md#ladder-and-prealloc-sizing-empirical-context).

---

## Headers beside the payload

### The problem headers solve

`delete p` does not take a size. Allocators that require `deallocate(p, size)`
invite size-mismatch bugs. Storing metadata *with* the allocation — in bytes the
user never sees — matches the `new`/`delete` mental model: the pointer you hold
is enough to free.

In this codebase the **pool** owns that header protocol (write on allocate,
validate on free). The **slab** returns a slot base; the pool advances the
pointer past the header before giving it to you.

### What “beside the payload” means

```text
[ slot base | hidden header |.#### user payload #### ]
              ▲               ▲
              │               └── pointer returned to you
              └── magic, header width, total footprint
```

On free, the pool reads fixed offsets *behind* your pointer, validates them,
recovers the slot base, and asks the owning slab backend to free that base (or
`delete[]`s via `SlabProxy`).

### What to remember as a user

- Do not write into the bytes before the returned pointer.  
- Do not free interior pointers (`p + 1`) — free must see a real user pointer
  from `allocate` / a factory.  
- Header width grows with alignment (`max(8, alignment)`), which is why
  high-align requests change footprint and sometimes alignment class.  

Byte-level layout: [IMPLEMENTATION.md — Memory layout](IMPLEMENTATION.md#memory-layout).

**Teaching angle:** metadata placement is a recurring systems theme — allocator
headers, network packet headers, and database row headers all trade a few hidden
bytes for a simpler public operation.

---

## Slots, bitsets, and growth

### One Block, many Slots

A size-class **Slab** owns zero or more **Blocks** (mapped chunks, often on the
order of 4–8 KiB for small classes). Each Block is a row of equal **Slots**.
Tracking which Slots are free is the core data-structure problem.

### Why bitsets?

A **bitset** stores one bit per Slot: free or in use.

Compare metadata cost for a 4 KiB Block of 16-byte Slots (256 cells):

| Scheme | Rough metadata |
|--------|----------------|
| Bitset | 256 bits = **32 bytes** |
| `bool` array | 256 bytes |
| Intrusive freelist of pointers | Often 8 bytes × free Slots, plus poorer locality |

Bits are tiny and scan well on modern CPUs. “Find a free Slot” walks those bits
(with a next-fit cursor and word/CTZ search in this project) instead of popping
an O(1) freelist node.

### Two-level filtering

Naïvely scanning every Slot in every Block would be wasteful. A coarse
**availability map** answers: “does this Block still have *any* free Slot?”
Fully full Blocks are skipped. Only then does the allocator search bits inside
a candidate Block.

That pattern — coarse filter, then fine structure — shows up often (page tables,
OS allocators, databases).

### Growth and reclaim

When every Block for a class is full, that **Slab** allocates another **Block**.
When a Block becomes completely empty, the Slab **keeps it mapped by default**
(monotonic growth):

- Pros: alloc/free avoid mmap/munmap thrash; the next burst is warm  
- Cons: RSS stays at the high-water mark until you ask to shrink  

Reclaim is exposed on the **Pool** as `Pool::trim()`, which asks each size-class
Slab to drop empty Blocks — a pool-level policy knob over slab-level storage.
Large (`SlabProxy`) allocations already return memory on each free.

### Next-fit cursor vs freelist

| Strategy | Idea | Cost |
|----------|------|------|
| **Next-fit + bitset** (this project) | Resume near the last alloc; word/CTZ find clear bits | Compact metadata; amortized O(1) under local churn |
| **Intrusive freelist** | Explicit list of free slots | Strict O(1) pop; more metadata / trim complexity |

Bitsets remain the occupancy source of truth; the cursor only changes where the
search starts. A freelist is the usual next step if profiles still show scan
cost — see IMPLEMENTATION Future Work.

---

## Finding a pointer’s owning Block

### The question free must answer

Given a user pointer (after the Pool recovers the **slot base**), which **Block**
owns it? Blocks are address **intervals** `[base, base + length)`. Looking up
“which interval contains this address?” is a standard problem. That is distinct
from “which **Slab** (size-class engine) handles this class?” — routing answers
the latter; address lookup answers the former.

### Approaches

| Approach | Intuition | Typical cost |
|----------|-----------|--------------|
| Ordered map of bases | `upper_bound` then step back to the owning base | O(log m) |
| Hash of bases | Fast when you only ever free exact bases | O(1) average; awkward for interior checks |
| Linear scan of bases | Simple | O(m) — fine only for tiny m |

This project uses an ordered map keyed by Block base addresses. The *concept*
matters more than the container choice: free must identify a **slot base**, not
an arbitrary interior address. Interior frees are rejected so a misaligned free
cannot flip a neighboring object’s freemap bit. (`findBlockForSlot` answers “which Block owns this slot base?”)

**Teaching angle:** ownership maps and interval trees appear in kernels (vma
trees), JITs, and sanitizers — same shape of question.

---

## Locking & contention

### Why locks appear at both layers

Multiple threads may allocate and free concurrently. The **pool** has shared
routing state (which slab object serves a class). Each **slab** has shared
slot state (which bits are free). Both need mutual exclusion (or a proven
lock-free protocol). This library uses locks with a deliberate **granularity**
story — and that granularity mirrors the pool-vs-slab split.

### Two levels

1. **Pool lock** — short critical sections for routing (“which slab handles this
   footprint?”) and size-class routing (including high-align 32/64-class
   slabs).  
2. **Per-slab lock** — protects that size class’s bitsets and address map.  

Once routing finishes, different size-class slabs can proceed in parallel. That
is the main scalability idea: do not hold the pool lock across every bitmap
update. Separately, using **multiple Pool instances** can reduce contention
further by isolating entire domains (see [pools](#what-is-a-pool-and-why-use-one)).

Lock **ordering** is fixed so thread A cannot hold lock 1 and wait for lock 2
while thread B does the reverse (deadlock). Exact order belongs in
IMPLEMENTATION; the concept is “always acquire in one global sequence.”

### What a spinlock is optimizing for

A mutex can park the thread immediately. A **spinlock** assumes the critical
section is *short*: it is often cheaper to busy-wait briefly than to sleep and
wake.

This project’s `SpinLock` escalates:

1. **Test-and-test-and-set (TTAS)** — read until the lock *looks* free, then try
   to acquire. Blind test-and-set makes every waiter write the cache line;
   TTAS lets waiters mostly read (shared state) until a chance appears.  
2. **Escalating backoff** — sleep a little longer after repeated failure so
   threads do not stampede when the lock opens.  
3. **Block** — `atomic_flag::wait` after enough failures, so prolonged contention
   does not burn a core forever.  

**RAII:** prefer `std::scoped_lock` / `std::unique_lock` over bare `lock()` /
`unlock()` so an exception cannot leave the lock held.

API: [mlvlib API — SpinLock](../mlvlib/docs/API.md#spinlock).  
Correctness deep dive: [SpinLock Analysis](../mlvlib/docs/deepdive/SpinLock_Analysis.md).

### Happens-before (intuition)

When thread A unlocks and thread B later locks the same mutex, B must *see*
A’s writes inside the critical section. Languages and CPUs need acquire/release
semantics (and, for custom locks under TSan, explicit annotations) to make that
guarantee real. You do not need the full memory model to use `scoped_lock` /
`unique_lock` correctly — but it is why unlock-before-lock is not “just a flag.”
Debug builds also record the owning thread id so a mismatched `unlock` aborts
early instead of corrupting waiters.

---

## RAII on a custom pool

### The raw-pointer trap

```cpp
std::byte* p = pool.allocate(sizeof(Widget));
Widget* w = new (p) Widget(args...);
// if something throws before deallocate, or a path forgets free → leak / UAF
```

Matching every `allocate` with a `deallocate`, and every placement `new` with a
destructor call, is easy to get wrong once real control flow appears.

### RAII restores the pairing

**Resource Acquisition Is Initialization:** the object’s lifetime *is* the
resource lifetime.

| Helper | Role |
|--------|------|
| `make_pool_unique` | Exclusive ownership; `PoolDeleter` runs `~T()` and returns memory (or no-ops if the pool already died — arena) |
| `make_pool_shared` | Shared ownership; pool object + heap CB via `PoolDeleter` |
| `PoolAllocator<T>` | Lets STL containers (`vector`, maps’ nodes, etc.) draw from the pool (preferred type-based path) |
| `PoolMemoryResource` | `std::pmr::memory_resource` adapter for polymorphic / composable PMR use (`pool_pmr.hpp`) |
| `PoolHandle` | Optional heap domain pin; `create()` auto-retires the pool when handles and live allocs are both zero |

Arrays destroy elements in **reverse** construction order — the same rule as
stack arrays and `delete[]`, so dependents tear down safely.

**Two teardown stories:** orderly (pointers first — primary, full RAII) vs
arena (pool first — bulk reclaim; documented destructor-skip for
`unique_pool_ptr`). Details:
[IMPLEMENTATION.md — Pool lifetime](IMPLEMENTATION.md#pool-lifetime-controlblock--poolhandle).

### Finalization vs reclamation

It helps to name two distinct steps that `delete` usually bundles together:

| Term | Meaning | In this library |
|------|---------|-----------------|
| **Finalization** | Run `~T()` (release non-memory resources, break invariants) | Orderly path: `PoolDeleter` always finalizes while the pool is alive |
| **Reclamation** | Return or drop the underlying storage | Orderly: return the slot to a slab freemap. Arena: `~Pool` drops slabs in bulk |

**Orderly teardown** does both, in the usual C++ order: finalize then reclaim,
per object, under smart-pointer / container lifetime.

**Arena teardown** (`~Pool` while `unique_pool_ptr` / `make_pool_shared` owners
may still exist) **skips finalization and per-object reclaim** in `PoolDeleter`
when the control block is already `!alive`, then reclaims storage by destroying
slabs. That matches deferred-reclamation thinking: reclaim a whole domain at a
chosen moment. It is safe for byte buffers and **trivially destructible**
payloads; for types with meaningful destructors, prefer orderly teardown (or
accept that `~T()` will not run). Do not rely on one arena-reclaimed object
observing another during skipped finalization - there is no destruction order.

`PoolDeleter` is **stateful** (it holds a `PoolLifetimeToken`). Empty / EBO-friendly
deleters are preferable when size matters, but the token is required so arena
`~Pool` can be observed safely without parking the control block in the slab.
That size cost is intentional.

### Destroying delete (future, type-local)

C++ also has **destroying delete**: a typed `operator delete` that runs with
full knowledge of `T`, so one function can finalize and reclaim together.
That can be attractive for **type-specific** pools (custom `new`/`delete` on a
`final` class, Roy Ch.7 theme).

This library’s default path stays **generic**: `make_pool_unique` /
`make_pool_shared` + `PoolDeleter` work for any `T` under the pool’s alignment
cap and remain arena-observable via `PoolLifetimeToken`. Replacing that with
destroying delete is an explicit **non-goal** for the generic pool; it remains
a possible future for a narrowly typed facade, not for `Pool` itself.
Details: [IMPLEMENTATION.md — Smart pointers](IMPLEMENTATION.md#smart-pointers-design).

### Exception safety (the strong guarantee)

When constructing an array of `N` elements, if element *k* throws:

1. Destroy elements `k-1 … 0`  
2. Return the block to the pool  
3. Propagate the exception  

The caller observes either a fully built smart pointer or no allocation left
behind. That is the **strong exception guarantee**, and it is one of the main
reasons factories exist instead of ad-hoc placement new.

**Lesson:** a custom allocator is only as safe as its matching deallocation path.
Factories and deleters exist so application code rarely touches raw
`std::byte*`.

Details: [API.md — Smart pointers](API.md#smart-pointers).

---

## Observing lifetime without owning

### The async use-after-free problem

```text
Thread / event loop                    Object A
─────────────────                      ────────
register callback capturing &A
…                                      A destroyed
callback runs → uses A                 ✗ use-after-free
```

Deferred work (timers, I/O completions, thread-pool tasks) often outlives the
object that registered it.

### What `LifetimeObserver` provides

An intrusive **liveness** helper (inspired by `shared_ptr` / `weak_ptr`):

| Piece | Role |
|-------|------|
| **Owner** reference | Held by the live object (or copies that share ownership semantics) |
| **`getObserver()`** | Hands out a **weak** view that does *not* keep the object alive |
| **`isAlive()`** | Asks: “is there still an owner ref?” via atomic counts |

Compare patterns:

| | `shared_ptr` / `weak_ptr` | `enable_shared_from_this` | `LifetimeObserver` |
|--|---------------------------|---------------------------|------------------|
| Model | Shared ownership | Intrusive weak-from-`this` | Intrusive weak **liveness** |
| Upgrade to own | `weak_ptr::lock()` | Via existing `shared_ptr` world | Not a strong pin — by design |
| Typical fit | Share ownership for real | Already using `shared_ptr` | Check liveness without full sharing |

### Same-thread / single-strand vs cross-thread

**Single-strand event loops** (one thread runs callbacks *and* destruction, and
they never overlap): `isAlive()`-then-use is a **complete** pattern. You do not
need an extra mutex for correctness — the strand *is* the synchronization.

**Cross-thread** (callback on thread B while thread A may destroy):

- Atomics make the **liveness bit** race-free: concurrent `isAlive()` while
  another thread destroys does not data-race on the control-block counts.  
- Atomics do **not** make check-then-use of object **members** safe if destroy
  can run between the check and the use (classic TOCTOU).  

When use and destroy can race, hold the **same mutex/strand that covers
destroy** across check and use, or switch to `shared_ptr` / `weak_ptr` when you
need upgrade-to-own.

This is the same rule as any shared mutable object — not a gap in `isAlive()`.

### Teaching pitfalls

1. **Capturing an owner copy in the callback** — that keeps the object alive
   accidentally; use `getObserver()`.  
2. **Treating `isAlive()` as a pin** — it answers liveness; it does not extend
   lifetime.  
3. **Skipping the check** — any deferred callback can UAF.  
4. **Assuming “atomics ⇒ TOCTOU solved”** — atomics fix the counter race, not
   cross-thread member access without a protocol.  

API contracts and snippets: [mlvlib API — LifetimeObserver](../mlvlib/docs/API.md#lifetimeobserver).

---

## Putting the pieces together

A typical service-shaped use of the library:

```text
Pool                         ← your disposable / isolatable allocation domain
  ├── routes to Slab<…>      ← fixed-size slot recycling per class
  ├── routes to SlabProxy    ← oversized system allocations
  └── make_pool_* / PoolAllocator  ← typed RAII on top of the pool

LifetimeObserver (optional)   ← mlvlib; independent of Pool/Slab
SpinLock                     ← mlvlib::SpinLock; used by Pool/Slab
```

Remember: destroy or shard **pools** for lifetime and isolation; tune or
profile **slabs** for size-class recycling behavior.

**Suggested learning demos** (after `make`):

| Demo / test | Concepts |
|-------------|----------|
| `demo_spalloc_shared_ptr` | Pool as domain + shared ownership + STL allocator |
| `demo_pool_pmr` | `PoolMemoryResource` + `pmr::vector`, vs `PoolAllocator`, tracing decorator |
| mlvlib `demo_lifetime_observer` | Single-strand vs cross-thread liveness |
| `make test` | Slab growth/trim, pool routing/alignment, threads, locks |

---

## Concepts illustrated

Clear production code also surfaces these ideas — useful as a checklist while
reading headers or teaching from the repo.

### Template metaprogramming

- `constexpr` size ladders and `Slab<ElemSize, Alignment>` specializing layout
  at compile time  
- `requires` / concepts on factory overloads for clear errors  
- Zero-cost abstraction: templates that instantiate into straight-line code  

### Modern C++

- Minimum **C++20** with working `<format>`, ranges, and `atomic_flag::wait` /
  `notify_one`; configure may select C++23 or C++26 when available  
- `std::print` when `__cpp_lib_print` is set; otherwise a format-based shim  
- Ranges for reverse-order destruction  
- `std::source_location` in debug asserts  

### Memory management

- Pools as lifetime / isolation boundaries vs slabs as fixed-size recyclers  
- Slab allocation vs general heap  
- Headers / metadata beside the payload (pool protocol over slab slots)  
- Internal vs external fragmentation  
- Finalization vs reclamation (orderly vs arena)  
- RAII and custom deleters / allocators  

### Data structures

- Bitsets and two-level availability maps  
- Interval / base maps for ownership  
- Intrusive control blocks for lifetime  

### Concurrency

- Lock granularity and ordering  
- Acquire/release and happens-before  
- Single-strand protocols vs cross-thread TOCTOU  
- Why custom locks need TSan annotations  

---

## Choosing a memory strategy

Custom allocation pays off only when the default path is a measured bottleneck
or when you need a named lifetime / isolation domain. Prefer the simplest tool
that meets the workload; re-check after compiler or standard-library upgrades.

### When to use what

| Need | Prefer | Why |
|------|--------|-----|
| General-purpose alloc/free; no special lifetime story | `new` / `delete`, `std::allocator`, containers as usual | Highly tuned; zero maintenance in *your* code |
| STL containers with a shared runtime resource; stack buffers; easy composition | `std::pmr` (`monotonic_buffer_resource`, `synchronized_pool_resource` / `unsynchronized_pool_resource`, custom `memory_resource`) | Value-based allocators; virtual dispatch is usually cheap vs allocation itself |
| Concurrent service churn across many sizes; shardable domains; RAII / STL on a pool; optional arena bulk reclaim | **`spallocator::Pool`** (this library) | Size-class recycling slabs + large fallback + smart pointers / `PoolAllocator` |
| Bounded phase (request / scene / level); allocate forward; almost no individual free; reclaim everything at end | **Arena `~Pool`** (shipped bulk reclaim); dedicated bump facility **deferred** | Bump placement (cursor, no reuse) is not slab recycling - they only share buffer-growth / domain-destroy. Use orderly teardown when `~T()` must run; see backlog if a true bump peer is revived |

If profiling cannot show a win over the standard library (or over `std::pmr`
pool resources) for your sizes and thread model, stay with the standard tools.

### How this library maps to common teaching patterns

Patrice Roy's *C++ Memory Management* (Packt, 2025) walks several patterns that
line up with - or deliberately differ from - this codebase:

| Pattern (Roy) | Role | Here |
|---------------|------|------|
| **Chunked pool** (`ChunkSizedAllocator`-style size categories, smallest fit, overflow path) | Academic multi-size pool | Production analogue: size-class `Slab`s + `SlabProxy`, with **reuse**, growth, headers, and `trim` |
| **Size-based bump arena** (sequential allocate, empty individual free, destroy domain to reclaim) | Scene / level / bounded N | Closest shipped: arena `~Pool` bulk reclaim. Dedicated bump **deferred** (peer domain later, not a Pool façade / dual-mode `Slab`) - this `Pool` still classifies and recycles on the hot path |
| **Deferred reclamation** | Finalize and/or reclaim a group at a chosen moment | Arena path: skip per-object finalization in `PoolDeleter` when `!alive`, reclaim storage with the pool. Prefer orderly teardown when `~T()` must run |
| **Traditional (type-based) allocator** | Compile-time allocator in the container type | `PoolAllocator<T>` (POCCA/POCMA/POCS false; equality by control block) |
| **PMR (`memory_resource`)** | Runtime resource pointer; easy to compose | `PoolMemoryResource` in `pool_pmr.hpp` (additive include). Prefer `PoolAllocator<T>` when the type can bake the allocator in (no virtual dispatch) |

**Alignment note:** Roy's academic chunked example often rounds categories to
powers of two so contiguous slots stay aligned. This library instead uses
explicit `Slab<ElemSize, Alignment>` alignment classes and a pool header of
`max(8, alignment)`, so high-align traffic does not share a layout that cannot
recover alignment class and slot base on free.

**Synchronization note:** allocation APIs must be thread-safe. Roy's samples
often use `std::mutex` and warn that lock cost on free can erase gains. This
project uses short-section `mlvlib::SpinLock` at pool and per-slab granularity;
pluggable lock choice is backlog if contention profiles prefer a mutex.

More design detail:
[IMPLEMENTATION.md - Future work](IMPLEMENTATION.md#future-work) and
[Smart pointers](IMPLEMENTATION.md#smart-pointers-design).

---

## Terminology

Locked stack: **Pool** → **Slab** (size-class engine) → **Block** (mapped chunk)
→ **Slot**. **Item** = external/backend allocation name (`allocateItem`); for a
`Slab` that Item is a **slot base**. Lifetime: `PoolLifetimeToken`;
`PoolControlBlock` kept. Full glossary:
[terminology-glossary.md](terminology-glossary.md).

---

## Further reading

- [terminology-glossary.md](terminology-glossary.md) — locked Pool / Slab / Block / Slot vocabulary  
- Patrice Roy - *C++ Memory Management* (Packt, 2025), especially Ch. 10
  (arenas / chunked pools), Ch. 11 (deferred reclamation), and Ch. 14
  (traditional allocators and PMR). Sample code:
  [PacktPublishing/C-Plus-Plus-Memory-Management](https://github.com/PacktPublishing/C-Plus-Plus-Memory-Management)  
- [Arena-based memory management (excerpt)](https://deepengineering.net/p/arena-based-memory-management) - public Ch. 10 material  
- [Bonwick (1994) — The Slab Allocator](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps)  
- [Linux SLAB/SLUB overview](https://www.kernel.org/doc/gorman/html/understand/understand011.html)  
- [memcached](https://memcached.org/) — popular user-space slab usage  
- [API.md](API.md) — call the types  
- [IMPLEMENTATION.md](IMPLEMENTATION.md) — layout, trim, complexity, readiness  
- [SpinLock Analysis](../mlvlib/docs/deepdive/SpinLock_Analysis.md) — lock correctness  

