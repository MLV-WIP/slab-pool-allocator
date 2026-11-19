# Implementation Guide

Architecture, invariants, design trade-offs, and production readiness for
engineers integrating or extending the allocator.

| Doc | Role |
|-----|------|
| [API.md](API.md) | Contracts, parameters, snippets |
| [CONCEPTS.md](CONCEPTS.md) | Learning / conceptual narrative (pool ≠ slab) |
| This file | How the pieces are built and why |
| [README](../README.md) | Build, test, demos |
| [SpinLock Analysis](../mlvlib/docs/deepdive/SpinLock_Analysis.md) | Lock correctness deep dive (mlvlib) |

**Audience:** production correctness first. Explanations stay precise so the same
code remains readable for teaching — without soft-pedaling limits.

**Pool ≠ slab:** the **pool** is the allocation domain (lifetime, routing,
isolation). A **slab** is fixed-size slot storage the pool routes into. Keep
those responsibilities distinct while reading below.

---

## Table of contents

- [How to read](#how-to-read)
- [Component map](#component-map)
- [Memory layout](#memory-layout)
- [Alignment design](#alignment-design)
- [Request lifecycle](#request-lifecycle)
- [Pool routing & registries](#pool-routing--registries)
- [Slab strategy](#slab-strategy)
- [Bitset tracking](#bitset-tracking)
- [Address lookup](#address-lookup)
- [Trim and RSS](#trim-and-rss)
- [Smart pointers (design)](#smart-pointers-design)
- [Pool lifetime (ControlBlock & PoolHandle)](#pool-lifetime-controlblock--poolhandle)
- [LifetimeObserver (design)](#lifetimeobserver-design)
- [Thread safety & lock ordering](#thread-safety--lock-ordering)
- [SpinLock (design summary)](#spinlock-design-summary)
- [Performance](#performance)
- [Debugging & sanitizers](#debugging--sanitizers)
- [Known limits & readiness](#known-limits--readiness)
- [Future work](#future-work)
- [Demos & tests](#demos--tests)

---

## How to read

1. **Component map** — who owns what (pool vs slab)  
2. **Memory layout** + **Alignment design** — header protocol and axes  
3. **Request lifecycle** — one numeric alloc/free walkthrough  
4. **Pool routing** / **Slab strategy** / **bitsets** / **address lookup** — internals  
5. **Trim**, **smart pointers**, **LifetimeObserver**, **locks** as needed  
6. **Performance**, **debugging**, **future work** when optimizing or hardening  

Skip to [API.md](API.md) for call signatures; use [CONCEPTS.md](CONCEPTS.md) when
you need the “why pools / why slabs” story without code structure.

---

## Component map

```
                    ┌─────────────────────────────────────┐
                    │  Pool — lifetime / routing domain   │
                    │  (registry, header protocol, locks) │
                    └──────────────┬──────────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
     AbstractSlab           AbstractSlab          SlabProxy
     Slab<16> …          (fixed-size slots)    (>axis max → ::new[])
              │
              └── per-slab SpinLock, bitsets, base_address_map

  Smart pointers / PoolAllocator ──► clients via PoolBlockToken / PoolHandle
  LifetimeObserver                 (stand-alone; not part of Pool)
```

| Layer | Responsibility |
|-------|----------------|
| **`Pool`** | Allocation *domain*: construct/destroy as a unit, isolate shards, route by footprint + alignment, write/validate the allocation header, `trim` / accounting, pool-level locks |
| **`Slab`** | Fixed-size *storage*: buffers, freemap, address map, growth/reclaim of empty buffers, per-slab lock |
| **`SlabProxy`** | Same backend interface; system `new[]` / `delete[]` for oversized requests |
| **Smart pointers** | Clients via `PoolBlockToken` / `PoolHandle` (typed RAII) |
| **`LifetimeObserver`** | Independent of pool/slab |

Destroying a `Pool` destroys its slab backends. Speeding up 64-byte churn is
primarily a slab concern; naming “memory for this shard” is a pool concern.
Background: [CONCEPTS.md](CONCEPTS.md#what-is-a-pool-and-why-use-one).

`AbstractSlab` is the narrow virtual interface (`allocateItem` /
`deallocateItem` / reclaim / accounting) that lets `Pool` treat size-class
`Slab`s and `SlabProxy` uniformly after routing.

---

## Memory layout

Every `Pool::allocate(N, alignment)` reserves a **header** in front of the user
payload. The pointer returned to the caller is past that header; free reads
metadata behind it. The **slab** (or `SlabProxy`) only ever sees the **slot
base**; the pool is responsible for the header protocol.

```
Default layout (alignment ≤ 8 → header_size = 8):

  slab slot base                                                user pointer
  │                                                             │
  ▼                                                             ▼
┌──────────┬─────────┬────────────────┬────────────────┬──────────────────┐
│ pad      │ magic   │ header_size    │ alloc_size     │ payload (N B)    │
│          │ (1 B)   │ (1 byte)       │ (uint32 / 4 B) │                  │
└──────────┴─────────┴────────────────┴────────────────┴──────────────────┘
             ▲         ▲                ▲
             │         │                └─ user_ptr − 4
             │         └─ user_ptr − 5
             └─ user_ptr − 6  (kAllocMagic; cleared on free)
```

| Offset from user ptr | Type | Meaning |
|----------------------|------|---------|
| `−4 … −1` | `uint32_t` | Footprint `alloc_size = N + header_size` (selects backend on free) |
| `−5` | `uint8_t` | `header_size` itself; must be 8, 16, 32, or 64 |
| `−6` | `uint8_t` | `kAllocMagic`; validated then cleared on free |
| `−header_size … −7` | padding | Remaining bytes so the user pointer meets `alignment` |

### Division of labor

| Step | Actor |
|------|--------|
| Choose class / allocate raw slot of `alloc_size` bytes | Slab or `SlabProxy` |
| Write magic, `header_size`, `alloc_size`; return `base + header_size` | Pool |
| On free: validate header, recover `base`, clear magic | Pool |
| Mark slot free / `delete[]` | Slab or `SlabProxy` |

The slab never interprets the header. That keeps storage backends simple and
puts all “pointer looks like a pool allocation” policy in one place.

### Free validation (pool)

Before trusting the header:

1. `header_size ∈ {8,16,32,64}`  
2. Magic equals `kAllocMagic`  
3. Footprint in a plausible range (`header_size ≤ alloc_size ≤ kMaxAllocationBytes + max align`)  

Failures:

| API | Behavior |
|-----|----------|
| `Pool::deallocate` | Throws `std::invalid_argument` |
| `Pool::try_deallocate` | Returns `false` (including after failed slab free) |
| `PoolDeleter` / `PoolAllocator::deallocate` | `noexcept` → **`std::abort()`** on failure |

Clearing magic on a successful free turns a second free of the same user
pointer into a magic mismatch (double-free detection at the pool layer). Slab
interior-pointer checks remain a second line of defense.

Under AddressSanitizer the header region is **poisoned** after free and
**unpoisoned** when the slot is reused. Free briefly unpoisons a fixed peek
window so double-free / corrupt-header checks can run without ASan aborting
first, then re-poisons if the peek path fails.

### Overhead intuition

| User N | Align | Header | Footprint | Typical class | Header / N |
|--------|-------|--------|-----------|---------------|------------|
| 128 | 8 | 8 | 136 | 192B | ~6% |
| 100 | 8 | 8 | 108 | 128B | 8% |
| 16 | 8 | 8 | 24 | 32B | 50% |
| 64 | 64 | 64 | 128 | 128B (axis 64) | 100% |

Tiny objects pay a higher relative header tax; that is expected. Bitset and
class waste are additional — always reason in footprints when estimating.

### Design rationale

Storing size and header width with the pointer matches `delete`, avoids
size-mismatch bugs, and supports variable alignment without pointer tagging.

| Alternative | Why not (here) |
|-------------|----------------|
| Size argument on free | Easy to pass wrong size; diverges from `delete` |
| Pointer tagging | Steals bits; fights high alignment and sanitizers |
| Side size table | Extra indirection and lifetime coupling |

---

## Alignment design

Alignment is both a **user contract** (`allocate`’s second argument /
`alignof(T)` from factories) and a **routing key** into a size-class axis.

| Rule | Detail |
|------|--------|
| Default | `alignment = 8` |
| Allowed | Power of two in `[4, 64]`; else `invalid_argument` in **all** builds (including release) |
| Header width | `header_size = max(8, alignment)` |
| Native axis | `nativeAxis`: 4/8/16 → **16**; 32 → 32; 64 → 64 |
| Storage | `Slab<ElemSize, Alignment>` per axis ladder |
| Lazy axes | 32/64 slab objects created on first use |
| Over axis max | `SlabProxy` with `align_val_t{64}` |
| Factories | `static_assert(alignof(T) ≤ 64)` |

Non-power-of-two values in range (for example 12) are rejected even in release:
header/axis math assumes power-of-two alignment.

### Why compound keys?

High-align requests must not share a native-16 slot layout with a narrower
header story. If a 64-align object lived in an axis-16 128B slot with only an
8-byte header, free could not recover a consistent axis from `header_size`, and
slot bases would not guarantee the user’s alignment. Separate
`Slab<ElemSize, Alignment>` specializations keep:

- slot stride = `ElemSize` (multiple of `Alignment`)  
- freemap bit ↔ exact slot base  
- `header_size` recoverable as 8/16/32/64 and mapped back to an axis  

### Axis ladders

```
Axis 16 (eager): 16 … 1024
Axis 32 (lazy):  32 … 2048
Axis 64 (lazy):  64 … 4096
```

Request align 4/8 **over-aligns** into axis 16 (slots are at least 16-aligned).
That is intentional: one hot ladder for the common case; pay for 32/64 only on
first use.

**Examples:**

| Call | Header | Footprint | Path |
|------|--------|-----------|------|
| `allocate(64)` | 8 | 72 | 96B class, axis 16 |
| `allocate(64, 16)` | 16 | 80 | 96B class, axis 16 |
| `allocate(64, 64)` | 64 | 128 | 128B class, axis 64 (lazy) |

---

## Request lifecycle

End-to-end path shows how pool and slab hand off. Locks noted are the *intent*
of the current code; see [Thread safety](#thread-safety--lock-ordering).

### Allocate — `pool.allocate(100)` (default align 8)

1. **Validate (pool, unlocked):** reject `N > kMaxAllocationBytes`; require
   power-of-two align in `[4, 64]`  
2. **Compute (pool):** `header_size = 8`, `alloc_size = 108`  
3. **Classify (pool):** `nativeAxis(8) → 16`; class **128B** (108 > 96)  
4. **Resolve backend (pool lock, brief):** `slabs16[index]` or lazy create path  
5. **Take slot (slab lock):** find free bit, mark freemap / availability; return
   **slot base**  
6. **Publish header (pool, after slab returns):** write magic / sizes at base;
   return `base + header_size` to the caller  

Exclusive ownership of the user pointer begins at step 6.

### Deallocate

1. **Peek (pool):** read header behind user pointer (ASan unpoison peek if needed)  
2. **Validate (pool):** magic / `header_size` / footprint; else fail per API  
3. **Recover base (pool):** `base = item - header_size`; clear magic  
4. **Re-route (pool):** backend from `(alloc_size, axis-from-header_size)`  
5. **Free slot (slab lock):** `deallocateItem(base)` — exact slot base only  
6. **Retention:** empty buffers stay mapped until [`trim`](#trim-and-rss)  

Free must re-select the backend from the header rather than trusting the caller
to pass a size: that is the point of the metadata.

### Footprint → class (align 8 → +8 header)

| User N | Footprint | Size class |
|--------|-----------|------------|
| 8 | 16 | 16 |
| 16 | 24 | 32 |
| 100 | 108 | 128 |
| 128 | 136 | 192 |

Always classify by **footprint**, not bare user size. Mis-estimating waste in
tests or docs almost always means forgetting the header.

---

## Pool routing & registries

`Pool` is the directory of backends for one allocation domain.

| Registry | When built | Contents |
|----------|------------|----------|
| `slabs16[12]` | With the pool (eager) | One `unique_ptr<AbstractSlab>` per ladder rung |
| `slabs32` / `slabs64` | Lazy via `getOrCreateLazy` | Same shape; empty until first use |
| `large_slab` | With the pool | Single `SlabProxy` instance |

**Routing:** `resolveSlab(alloc_size, axis)` runs `selectClassIndex`. A normal
index picks a size-class slab; `kSlabIndexNpos` means footprint exceeds that
axis’s max → `large_slab`.

**Lazy create sequence (intent):**

1. Under `Pool::lock`, if the slot is already populated, return it  
2. Otherwise take `slab_init_lock` (heavy construction serialized here)  
3. Double-check under `Pool::lock` (another thread may have won)  
4. Construct `Slab<…>` **without** holding `Pool::lock`  
5. Publish under `Pool::lock`  

That keeps ordinary allocate/free routing critical sections short while avoiding
duplicate lazy slabs under races.

`selectSlab` / `selectClassIndex` are pure classification helpers: usable from
tests and constexpr-friendly call sites, and shared by allocate and free so both
paths agree on the ladder.

---

## Slab strategy

Each `Slab<ElemSize, Alignment>` is a **fixed-size recycler** for one rung of
one axis.

**Owns**

- Zero or more equally sized **buffers** (`slab_alloc_size` from
  `selectBufferSize`: exact pack of
  `max(kSlabTargetMultiplier, kSlabTargetBytes / ElemSize)` slots; defaults
  **8 KiB** target / **16** min slots, overridable at compile time via
  `./configure --with-slab-target-*`)
  when `ElemSize < 1_KB`, otherwise a small multiple of `ElemSize`)  
- At most `kMaxClassBytes / slab_alloc_size` buffers (default **4 GiB** class
  cap; `./configure --with-max-class-mib`)  
- Per-buffer freemap of slots  
- Growable availability map (`DynBitset`) across buffers  
- `base_address_map` for free  
- Its own `SpinLock`  

**Invariants (informal)**

- `ElemSize` is a multiple of `Alignment`; `Alignment ∈ {16,32,64}`  
- Every in-use freemap bit corresponds to a live user allocation whose slot base
  the pool wrapped with a header  
- Buffer count only shrinks via reclaim/trim, not via ordinary free  

**Growth:** if no buffer has a free slot, allocate another buffer, zero/init its
freemap, register its base, set its availability bit. **Monotonic by default:**
a fully empty buffer is *not* returned to the OS on the last free — see
[Trim and RSS](#trim-and-rss).

### `SlabProxy`

Large / overflow path behind the same `AbstractSlab` interface:

- `allocateItem` → aligned `new[]`  
- `deallocateItem` → `delete[]` (no freemap, no retention)  
- No address map: each block is standalone  

The pool still attaches the usual header on the returned user pointer so free
looks the same. Choosing proxy vs size-class is purely a routing decision from
footprint + axis max.

---

## Bitset tracking

Two levels of bits, both owned by the **slab** (not the pool):

| Structure | Scope | Role |
|-----------|-------|------|
| Slot freemap | Per buffer | Bit per slot: free vs in use |
| `DynBitset` availability | Per `Slab` object | Bit per live buffer: “has free capacity?” |

`DynBitset` grows with live buffer count — empty size classes do not embed a
fixed ~128 KiB phantom map (an earlier fixed-bitset design paid that tax even
when idle).

### Alloc path (conceptual)

1. Resume at the slab’s **next-fit cursor** `(buffer_index, slot_index)`  
2. Skip buffers whose availability bit is clear (full)  
3. Find the next clear freemap bit at or after the cursor via **word scan +
   `std::countr_zero`** (portable CTZ; same hardware class as libstdc++
   `bitset::_Find_next`, without relying on that GNU extension). If the forward
   scan misses and the cursor was mid-buffer, **wrap once within that buffer**
   before moving on  
4. Set the bit; if the buffer is now full, clear its availability bit  
5. Advance the cursor past the taken slot; return
   `base + bit_index * ElemSize` as the slot base  
6. If a full wrap of live buffers finds nothing: grow (`allocateNewSlab`), point
   the cursor at the new buffer, retry  

`reclaimEmptySlabs` / `destroySlabAt` **reset** the cursor to `(0,0)` because
swap-with-last can invalidate buffer indices.

### Free path (conceptual)

1. Map slot base → buffer via address lookup  
2. Compute bit index from offset within the buffer  
3. Reject if already free or not an exact slot boundary  
4. Clear freemap bit; set availability bit (buffer has room again)  

Free does **not** move the alloc cursor (next-fit does not require it).

### Why next-fit + CTZ (not freelist)?

Bitsets stay the single source of truth for occupancy and “is full?”.
Next-fit removes repeated re-scans of already-occupied low bits under typical
LIFO/local churn; word/CTZ search makes a cold scan **O(slots / word_bits)**
instead of one `test()` per slot. An intrusive freelist can make alloc strictly
O(1) but duplicates free-set state and complicates trim’s swap-with-last — keep
it as a profile-driven follow-on if cursors are not enough.

---

## Address lookup

On free, the slab must answer: which buffer owns this **slot base**?

Buffers are intervals `[base, base + slab_alloc_size)`. The implementation keeps
a `std::map` keyed by buffer base and uses interval logic (`upper_bound` then
step back to the candidate owner). Cost **O(log m)** in the number of buffers
for that size class.

| Approach | Trade-off |
|----------|-----------|
| `std::map` (current) | Clear correctness; flexible growth/trim |
| Hash of bases | Faster exact-base lookup; awkward if you must prove interval membership |
| Sorted vector | Better locality; insert/delete under growth/trim more expensive |

Chosen for correctness at typical slab counts; denser structures are a
profile-driven follow-on if free shows up hot.

### Interior pointers

The freemap bit corresponds to an **exact** slot base. Freeing
`user_ptr + 1`, a mid-slot address, or a pointer into padding must not clear a
neighbor’s bit. `deallocateItem` rejects those with `invalid_argument` after
lookup. Combined with pool header validation, this is how accidental interior
frees fail loudly instead of corrupting the freemap.

Trim/reclaim updates the map when buffers are destroyed (swap-with-last style
bookkeeping keeps indices consistent — see code).

---

## Trim and RSS

Ordinary free returns a **slot** to a freemap. It does not shrink the process
image. Peak RSS after a burst therefore tends to stick until something reclaims
empty **buffers**.

| Mechanism | Layer | Behavior |
|-----------|-------|----------|
| Ordinary free | Slab | Slot free; empty buffer stays mapped |
| `Slab::reclaimEmptySlabs(keep_min)` | Slab | Deletes fully empty buffers (swap-with-last); retains ≥ `keep_min` |
| `Pool::trim(keep_min)` | Pool | Walks size-class slabs and asks each to reclaim; domain policy knob |
| `SlabProxy` free | Proxy | Memory returned immediately every time |

**Why monotonic default?** Alloc/free churn should not mmap/munmap thrash; the
next burst stays warm. **Why expose trim on the pool?** “This shard is quiet —
give memory back” is a **domain** decision. The pool knows the registry; each
slab knows which buffers are empty.

`keep_min` is per size class and clamped to at least 1 so a class does not
oscillate between “zero buffers” and “allocate a buffer on every hit.”

### Accounting

`getAllocatedMemory()` sums **retained size-class buffer bytes** across the
pool’s slabs. It is not:

- a live “bytes currently checked out to clients” gauge  
- inclusive of in-flight `SlabProxy` blocks (those are not retained in the
  registry the same way)  

Use it to understand high-water retention and the effect of `trim`, not as a
full heap profiler.

---

## Smart pointers (design)

Factories and deleters are **clients of the pool domain** via
`PoolBlockToken` (and convenience overloads on `Pool&` / `PoolHandle`). They do
not change slab internals; they enforce RAII and exception safety on top.

### Roles

| Piece | Design point |
|-------|----------------|
| `PoolBlockToken` | `block_refs` pin on `PoolControlBlock`; observes `alive`; not a handle pin |
| `PoolDeleter` | Placement-new counterpart: **finalize** (`~T()`) then **reclaim** (`try_deallocate`); arrays destroy **reverse** order; if `!alive`, **no-op** both steps (arena); abort on bad free while alive. Stateful (`PoolBlockToken`) by necessity - see below |
| `make_pool_unique` | Placement `new`; on ctor throw, return slot (**strong guarantee**); overloads for `Pool&` / `PoolHandle` / token |
| `make_pool_shared` | Placement `new` + `shared_ptr(p, PoolDeleter)`; object in pool, CB on heap |
| `PoolAllocator` | Stateful token; no POCCA/POCMA/POCS; equal iff **same control block**; allocate throws `bad_alloc` if `!alive`; deallocate no-ops if dead |

### `PoolAllocator` design decisions

1. Holds a **`PoolBlockToken`**, not a raw `Pool&` — survives arena `~Pool` so
   deallocate can no-op instead of UAF.  
2. Stateful — different control blocks compare unequal.  
3. Uses the pool’s alignment-aware `allocate` for proper object placement
   (`alignof(T)` ≤ `kMaxAlignment`).  
4. Does **not** propagate on container copy/move/swap (POCCA/POCMA/POCS false)
   — the token is not “owned” by the container in a transferable sense; rebound
   allocators share the same control block.  
5. Equality is by `token.control()` identity (not `Pool*` address), so two
   tokens for the same live/dead domain still compare equal.

### Exception safety

Single-object `make_pool_unique`: allocate bytes → placement `new` → on
constructor throw, `deallocate` the raw block, rethrow. The object was never
constructed; the caller never sees a smart pointer to a half-built object, and
the pool does not leak the slot.

Array `make_pool_unique<U[]>`: `Pool::allocate` returns the **user pointer**
(header already written). Default-construct elements in order; if element *k*
throws, destroy `k-1 … 0` in reverse, free the block, rethrow — same strong
guarantee as `std::make_unique`-style helpers. Bounded arrays are deleted
(use `std::array`).

`PoolDeleter` / `PoolAllocator::deallocate` are `noexcept` and must not throw
through `unique_ptr` / allocator requirements — hence `try_deallocate` +
**`std::abort()`** on programming errors (bad pointer) while the pool is alive.
If the pool is already dead, free is a deliberate no-op (arena), not abort.

### Why not `allocate_shared` for shared

`std::allocate_shared` co-locates object and control block. With
`PoolAllocator`, that puts the CB in a slab — arena `~Pool` then UAFs on any
later `shared_ptr` use. The standard type cannot combine that single-alloc
layout with a `unique_ptr`-style deleter whose state lives outside the block.

So `make_pool_shared` mirrors `make_pool_unique`: placement `new` into the
pool, then `std::shared_ptr(p, PoolDeleter{token})`. The CB (and deleter/token)
are heap-allocated; arena no-op works the same as for `unique_pool_ptr`.
Cost: two allocations instead of one.

Unbounded array overloads store the element count in `PoolDeleter` (same as
`make_pool_unique<T[]>`). Bounded array overloads are deleted (use
`std::array`).

### Lifetime coupling

`PoolDeleter` / `PoolAllocator` hold a **`PoolBlockToken`** (`block_refs` only —
not `handle_refs`). That keeps the control block alive so `alive` remains
readable after `~Pool`, without pinning an empty heap pool the way
`PoolHandle` does.

| Path | Teardown order | `~T()` / free |
|------|----------------|---------------|
| **Orderly (primary)** | Smart pointers / containers first, then pool | Normal: `~T()` then return slot |
| **Arena + `unique_pool_ptr` / `make_pool_shared`** | Pool first while owners may still exist | `PoolDeleter` (token outside the slab; CB on heap for `shared_ptr`) sees `!alive` → skip `~T` and free |
| **Arena + `PoolAllocator` / raw `allocate_shared`** | **Unsafe** if storage outlives `~Pool` | Standard `~T` / pool-resident CB can UAF — use orderly teardown |

Arena bulk reclaim is the right tool for byte buffers / **trivially
destructible** payloads (or when you accept skipped finalization via
`PoolDeleter`). Do not park durable resources only in pooled objects if you
might destroy the pool first. Conceptual vocabulary (finalization vs
reclamation) and when-to-use guidance:
[CONCEPTS.md - Choosing a memory strategy](CONCEPTS.md#choosing-a-memory-strategy).

**Why a stateful deleter?** Empty / EBO-friendly `unique_ptr` deleters keep
`sizeof(unique_ptr)` at a raw pointer. `PoolDeleter` stores a `PoolBlockToken`
so arena `~Pool` remains observable after storage is gone. That enlarges
`unique_pool_ptr` relative to a stateless deleter; the cost buys defined
arena behavior instead of UAF.

Factories accept `Pool&`, `PoolHandle`, or `PoolBlockToken`. Prefer
`PoolHandle::create()` for heap domains that should auto-retire when idle —
see [Pool lifetime](#pool-lifetime-controlblock--poolhandle).

API: [API.md — Smart pointers](API.md#smart-pointers).  
Concepts: [CONCEPTS.md — RAII](CONCEPTS.md#raii-on-a-custom-pool).

---

## Pool lifetime (ControlBlock & PoolHandle)

Types live in `pool_lifetime.hpp` (pulled in by `pool.hpp`). Methods that
`delete` a heap `Pool` (`PoolHandle::create`, `release_handle_ref`) are defined
in `pool.hpp` after `class Pool` is complete.

Every `Pool` is paired with a heap-allocated **`PoolControlBlock`**: alive flag,
`live_allocs`, `handle_refs`, `block_refs`, and a back-pointer to the `Pool`.
The control block can outlive the `Pool` (e.g. a `PoolHandle` still held after
arena `~Pool`) so observers can see `!alive` safely.

### Heap-owned vs not

The control block carries a **`heap_owned_`** flag (not an atomic). It answers
one question only: *may this runtime automatically `delete` the `Pool` object?*

| How the `Pool` was born | `heap_owned_` | Who destroys the `Pool`? |
|-------------------------|---------------|---------------------------|
| Stack / `unique_ptr<Pool>` / `new Pool` without `create` | **false** (default) | Caller: `~Pool` on scope exit, `delete`, etc. |
| `PoolHandle::create()` | **true** | Runtime: when `handle_refs == 0` **and** `live_allocs == 0` |

`Pool`’s constructor always builds a control block with `heap_owned_ == false`.
Only `PoolHandle::create()` flips the flag:

1. `new Pool()` — ordinary ctor (control block exists, not heap-owned).  
2. `control()->set_heap_owned()` — mark this domain eligible for auto-retire.  
3. Return a `PoolHandle` sharing that control block (`handle_refs` / `block_refs` bumped).

`Pool::handle()` on a stack (or otherwise non-`create`) pool also returns a
`PoolHandle` and bumps `handle_refs`, but it **does not** set `heap_owned_`.
Dropping those handles never `delete`s the stack object.

### Retirement gate (`try_retire`)

Last handle drop and last successful free both call `try_retire()`:

1. If `!heap_owned_` → return immediately (stack / caller-owned pools).  
2. If `handle_refs != 0` or `live_allocs != 0` → not idle.  
3. Else CAS `alive` true→false, re-check counts, then return the `Pool*` for
   `delete` (caller deletes; on the free path that may be `delete this` after
   `try_deallocate` finishes touching members).

So the distinction is **not** “was the `Pool` allocated with `new`?” (a raw
`new Pool` is still not heap-owned for auto-retire). It is **explicitly
opt-in** via `PoolHandle::create()`, which both heap-allocates and sets the
flag. That keeps arena-style stack `~Pool` (bulk reclaim while handles may
still observe the control block) separate from handle-tied heap domains that
self-delete when idle.

```text
Pool pool;                 // heap_owned_ = false
auto h = pool.handle();    // pins handle_refs; drop never delete's pool
~pool / leave scope        // mark_dead; caller-driven destruction

auto h = PoolHandle::create();  // new Pool + set_heap_owned()
// … allocate / free / copy handles …
// when last handle gone AND live_allocs == 0 → delete Pool
```

---

## LifetimeObserver (design)

`LifetimeObserver` is **independent of the allocator**. It solves a different
problem: deferred callbacks that must know whether an object still exists,
without forcing full `shared_ptr` ownership. Pool/slab never call into it.

### Control block

Each live observer family shares a heap-allocated `ControlBlock`:

| Field | Role |
|-------|------|
| `owner_count` | Atomic strong refs (the object / owner copies) |
| `observer_count` | Atomic weak refs (`getObserver()` views) |
| `retired` | Ensures only one releaser `delete`s the block when both counts hit zero |

`addRef` / `releaseRef` adjust the appropriate counter. When a release drives a
counter to zero *and* the other counter is already zero, the caller tries
`retired.compare_exchange` — the winner deletes the control block.

### Liveness check

```text
isAlive()  →  acquire-load owner_count  →  true iff count > 0
```

A **false** result synchronizes with the release that dropped the last owner
(`acq_rel` on that `fetch_sub`), so the observer can treat “not alive” as
happening-after teardown of owner-side control-block state. A **true** result
means “an owner ref still existed at the load” — it does **not** pin the object
or extend its lifetime.

### Owner vs observer

| Kind | How you get it | Effect on lifetime |
|------|----------------|--------------------|
| Owner | Derived object’s default ctor; certain copies | Keeps `owner_count` > 0 while held |
| Observer | `getObserver()`, or explicit observer copy | Does **not** keep the object alive |

Callbacks should capture `getObserver()`, not an accidental owner copy (which
would prevent destruction).

### Move semantics

Move transfers the control-block pointer and ownership kind. The moved-from
object receives a **fresh** owner control block so its destructor remains
well-defined (it must still release something). That avoids double-free of the
shared block and keeps “every `LifetimeObserver` instance has a block” as an
invariant.

### Thread-safety model

| Situation | Safe? |
|-----------|--------|
| Concurrent `isAlive()` / `getCount` / ref adjust | **Yes** — atomics; no data race on counts |
| Concurrent `isAlive()` while another thread destroys the owner | **Yes** for the liveness bit |
| Same-thread / single-strand: `isAlive()` then use when destroy cannot overlap | **Yes** — complete pattern; no extra mutex required |
| Cross-thread: `isAlive()` then use object members while destroy can race | **No** — classic TOCTOU on the object; hold the same mutex/strand that covers destroy, or use `shared_ptr` / `weak_ptr` for upgrade-to-own |

Atomics close the **counter** race. They do not replace a protocol for
**member access** when use and destroy can race across threads. That is the same
rule as any shared mutable object — not a deficiency in `isAlive()`.

### Comparison (implementation stance)

| | `weak_ptr::lock()` | `LifetimeObserver::isAlive()` |
|--|--------------------|------------------------------|
| Result | Optional strong ownership | Boolean liveness |
| Pins object | Yes, if lock succeeds | No |
| Best when | You need to use the object without an external mutex | Strand/mutex already orders use vs destroy, or you only need a skip/go check |

API contracts: [mlvlib API — LifetimeObserver](../mlvlib/docs/API.md#lifetimeobserver).  
Teaching narrative: [CONCEPTS.md](CONCEPTS.md#observing-lifetime-without-owning).

---

## Thread safety & lock ordering

Concurrency is layered the same way as ownership: **pool** locks protect the
directory of backends; **slab** locks protect slot metadata inside one size
class. Different classes can run in parallel after routing. Separate `Pool`
instances share no locks — isolation is free at the domain boundary.

| Lock | Protects | Hold time intent |
|------|----------|------------------|
| `Pool::lock` | Registry / routing snapshots | Brief — look up or publish a pointer |
| `Pool::slab_init_lock` | Lazy slab construction | May cover heavy `Slab` ctor; not on every alloc |
| Per-`Slab` `SpinLock` | Freemap, availability, address map | Brief — bit ops and map update |

### Ordering intent

Never take locks in opposing orders across threads:

- Common allocate: `Pool::lock` (resolve) → drop → `Slab::lock` (slot)  
- Lazy publish: `slab_init_lock` → brief `Pool::lock` to install  
- Avoid: holding `Slab::lock` then blocking on `Pool::lock` while another thread
  holds pool and waits for that slab  

Exact sequences live in `pool.hpp` / `pool_lifetime.hpp` / `slab.hpp` — treat those as source of
truth if this summary drifts.

### Client-facing notes

| Client | Thread notes |
|--------|----------------|
| Concurrent `Pool::allocate` / free | Supported on one pool |
| `unique_pool_ptr` | Unique ownership — cross-thread handoff needs external sync on the pointer |
| `shared_ptr` from `make_pool_shared` | Standard atomic control-block refcounts |
| Multiple `Pool` instances | Separate domains; no shared locks between pools |
| `LifetimeObserver` | Allocator-independent; see its own thread model |

Prefer `std::scoped_lock` or `std::unique_lock` around `SpinLock` so exceptions
cannot leave a lock held (`unique_lock` when early unlock / deferred lock is
needed). Compile-time swap of the lock type is backlog item R5 on the research
canvas.

---

## SpinLock (design summary)

`SpinLock` is provided by **mlvlib** (`mlvlib::SpinLock` in
`mlvlib/spinlock.hpp`) and used for **short** critical sections in `Pool` and
`Slab`. It is intentionally not nested under `spallocator::` so it can be
reused outside the allocator. It models `BasicLockable` (`lock` / `unlock` /
`try_lock`) so `std::scoped_lock` / `std::unique_lock` work. Non-copyable,
non-movable. Full line-by-line analysis:
[SpinLock_Analysis.md](../mlvlib/docs/deepdive/SpinLock_Analysis.md).

### Why not always `std::mutex`?

Pool/slab critical sections are tiny (route a pointer; flip freemap bits). A
spinlock that busy-waits briefly often beats sleeping a thread on uncontended or
lightly contended paths. Under prolonged contention the implementation **stops**
burning CPU and blocks via `atomic_flag::wait`. Whether a given embed prefers
`std::mutex` instead is exactly the pluggable-lock backlog item.

### State

One `std::atomic_flag`: clear = unlocked, set = locked. Acquisition uses
`test_and_set(acquire)`; release uses `clear(release)` plus `notify_one`.

### `lock()` algorithm

```text
1. Thread-local RNG → initial wait_time (1–100 ns); backoff_count = 0
2. Loop:
   a. Spin up to 100×: relaxed test(); yield if held; break if appears free
   b. test_and_set(acquire); if won → TSan happens-after; return
   c. If backoff_count < 10: sleep(wait_time); wait_time *= 2; ++backoff_count
   d. Else: wait(true) until notified; loop back to (a)  // must still TAS
```

Important correctness detail: after `wait()` returns, the thread **does not**
assume it owns the lock — it returns to the spin/TAS path. `backoff_count`
lives across iterations so the wait fallback remains reachable (resetting it
each loop would livelock in the sleep phase).

| Phase | Purpose |
|-------|---------|
| Relaxed TTAS spin | Cheap poll; avoid cache-line writes until a real acquire attempt |
| Escalating sleep | Desynchronize thundering herds without jumping straight to park |
| `wait` / `notify` | CPU-efficient block under sustained contention |

`try_lock()` does a relaxed test then a single `test_and_set(acquire)`; it never
waits.

### Memory ordering (intuition)

```text
Thread A                          Thread B
lock() [acquire]
  // writes protected data
unlock() [release]  ──happens-before──►  lock() [acquire]
                                           // sees A's writes
```

Relaxed loads in the spin/wait paths only decide *whether to try*; they are not
the synchronization edge. The acquire TAS and release clear are.

### ThreadSanitizer annotations

Custom locks are invisible to TSan unless happens-before is published:

| When | Annotation |
|------|------------|
| After successful acquire | `TSAN_ANNOTATE_HAPPENS_AFTER` → `__tsan_acquire` |
| Before `clear` in unlock | `TSAN_ANNOTATE_HAPPENS_BEFORE` → `__tsan_release` |

Enabled when GCC defines `__SANITIZE_THREAD__` or Clang
`__has_feature(thread_sanitizer)`. Without them, TSan may false-positive on data
guarded only by `SpinLock`.

### Debug owner tracking

In non-`NDEBUG` builds, `SpinLock` stores the owning `std::thread::id` after a
successful acquire and asserts on `unlock` that the caller is that owner (then
clears the id before the release `clear`). That catches double-unlock and
unlock-by-non-owner during development. Release builds omit the member and the
checks — do not mix debug and release TUs against the same `SpinLock` layout.

### Usage guidance

- Hold only for brief registry / bitmap work; do not call into user code under
  the lock  
- Prefer RAII (`std::scoped_lock` / `std::unique_lock`) over bare `lock`/`unlock`  
- API surface: [mlvlib API — SpinLock](../mlvlib/docs/API.md#spinlock)  

---

## Performance

### Complexity

| Operation | Amortized | Notes |
|-----------|-----------|-------|
| Alloc (size class) | O(1) | Next-fit cursor; cold scan O(slots / 64) via CTZ |
| Free (size class) | O(log m) | Address map; m = buffers in that slab |
| `selectSlab` | O(1) | Ladder of compares |
| `SpinLock` uncontended | O(1) | Single atomic |
| `SpinLock` contended | depends | Spin → sleep backoff → `wait` |
| `SlabProxy` alloc/free | OS dependent | No slot freemap |
| `trim` | O(classes × buffers) | Quiet-path; resets next-fit cursors |

“Amortized O(1)” alloc assumes the availability map keeps you on a non-full
buffer and that the next-fit cursor stays near free slots (typical local
churn). A pathological pattern (one free bit far from the cursor every time)
still walks words across the buffer — freelist remains the follow-on if that
shows up hot.

### Fragmentation

Always reason about **footprint vs class size**, not bare user size vs class.
Waste has three buckets:

1. Header bytes (`max(8, alignment)`)  
2. Internal slack inside the chosen class slot  
3. Retained empty buffers until `trim` (RSS, not per-object waste)  

### Scalability levers

| Lever | Effect |
|-------|--------|
| Per-slab locks | Different size classes proceed in parallel after routing |
| Multiple pools | Isolate shards / tenants; reduce cross-talk and lock sharing |
| Lazy 32/64 axes | No metadata cost until high-align traffic appears |
| `trim` | Reclaim RSS after spikes without changing the hot path |
| `SlabProxy` | Avoids forcing huge objects through size-class bookkeeping |

Measure before replacing the address map or moving to a freelist — both are
deliberate simplicity until profiles disagree.

### Comparative baselines (`make bench`)

On-demand harness: `bench/bench_allocators.cpp` via `make bench` (not part of
`make test` / default `all`). Prefer `BUILD_TYPE=release`. The `bench` and
`profile` make goals force `ENABLE_COVERAGE=no` so gcov arcs do not inflate
ns/op.

```bash
make bench BUILD_TYPE=release
make bench BUILD_TYPE=release BENCH_ARGS='--threads=4'
make bench BUILD_TYPE=release BENCH_ARGS='--align=64'   # optional Pool vs ::new
make bench BUILD_TYPE=release BENCH_ARGS='--scenario=lifo --backend=pool'
```

Compares `::operator new`, `spallocator::Pool`, `std::pmr::unsynchronized_pool_resource`,
`std::pmr::synchronized_pool_resource` (multi-thread), and bare `Slab<16,16>`
(16B churn). Prints **ns/op** and **ratio vs `::new`** for warmed LIFO churn,
randomized multi-size mix (uniform user sizes in `[16, 1024]`), alloc-then-free-all,
and optional multi-thread multi-size churn (`--threads=N`). Size picks use
per-thread `std::minstd_rand` seeded from a fixed key plus worker index
(repeatable). After Pool / Slab scenarios, prints `PoolStats` or slab retained
bytes. Absolute ns are host/toolchain-specific; re-run after compiler or
stdlib upgrades and compare ratios on the same machine.

Filters (for focused timing or profiling):

| Flag | Values | Notes |
|------|--------|-------|
| `--scenario=` | `lifo`, `multi`, `bulk`, `threaded`, `all` (comma-ok) | Default `all` |
| `--backend=` | `new`, `pool`, `pmr_unsync`, `pmr_sync`, `slab16`, `all` | Inapplicable combos are skipped with a note; without `new`, ratios show `n/a` |

Default alignment is 8. `--align=64` is a Pool axis/header-tax spot-check only
(not a full alignment matrix).

`SlabTest.AllocFreeChurnMicrobench` remains a light GTest smoke/`RecordProperty`
check for the slab engine; use `make bench` for comparative baselines.

### Profiling (`make profile`)

`BUILD_TYPE=profile` is release-equivalent (`-O3 -DNDEBUG`) plus `-g` and
`-fno-omit-frame-pointer` so Linux `perf` / Hotspot can unwind stacks. The
`profile` goal builds that binary, runs `perf record` into
`obj/profile/perf.data`, and prints a short `perf report` summary.

```bash
make profile BENCH_ARGS='--scenario=lifo --backend=slab16 --iters=2000000'
make profile BENCH_ARGS='--scenario=lifo --backend=pool --iters=2000000'
# Full interactive report / GUI:
#   perf report -i obj/profile/perf.data
#   hotspot obj/profile/perf.data
```

Pin one scenario and one backend so samples concentrate on the path under study.
Compare Slab-only vs Pool to separate engine cost from Pool routing/header work.
Do not treat sanitizer or coverage builds as profile baselines.

If `perf record` fails with “Access to performance monitoring…”, the profile
binary was still built under `obj/profile/`. Relax
`kernel.perf_event_paranoid` for your session (often `1` or `2`), or grant
`CAP_PERFMON`, then re-run `make profile`. You can also time the binary
directly: `./obj/profile/bench_allocators --scenario=lifo --backend=slab16`.

---

## Debugging & sanitizers

| Build | Role |
|-------|------|
| `BUILD_TYPE=addrsan` (default) | ASan + LSan + UBSan; header poison on free |
| `BUILD_TYPE=threadsan` | TSan (+ UBSan); exercises locks / annotations |
| `BUILD_TYPE=release` | `-O3 -DNDEBUG`; coverage on by default in CI |
| `BUILD_TYPE=memsan` | Valid locally when host provides instrumented C++ runtime/gtest; **omitted from CI** |
| `-DVDEBUG` | Verbose `debug_println` macros (args evaluated only when verbose; slab create/alloc/free, …) |

Sanitizer instrumentors are mutually exclusive at configure time (ASan/LSan vs
MSan vs TSan); UBSan may combine. `BUILD_TYPE` always applies the matching
`-fsanitize=` flags so a bare `make BUILD_TYPE=threadsan` cannot silently omit
TSan.

`runtime_assert` is debug-only (`if constexpr`); release compiles it out. Use
real throws / error returns for caller mistakes that can happen in production
(bad alignment, huge size, bad free via throwing `deallocate`).

When chasing freemap corruption, prefer addrsan builds: poisoned headers catch
use-after-free into metadata; interior frees should throw before flipping bits.

---

## Known limits & readiness

Solid for concurrent service use of the pool, slabs, smart-pointer helpers, and
`PoolAllocator`, with the following explicit limits:

| Topic | Status |
|-------|--------|
| Pool destroy while clients hold pointers | **Addressed** — ControlBlock / Handle / BlockToken; orderly + arena + heap auto-retire |
| Alloc bit scan | Next-fit + CTZ word search (production); freelist if still hot |
| Address map | O(log m); denser structures if free is hot |
| Fixed size-class ladders | Deliberate; exact-pack buffers within classes; non-ladder shaping is backlog R2 |
| Lock type | Hard-wired `SpinLock`; pluggable primitive is backlog R5 |
| MSan in CI | Deferred pending host instrumented runtime |
| Metrics API | `PoolStats` / `Pool::stats()` (alloc/free counters + HWMs); richer per-class breakdown later |
| C API | C++ only today; C99 wrapper is backlog R4 |

These are engineering follow-ons, not soft-pedaled “educational” gaps. Pool
lifetime vs clients is covered under [Pool lifetime](#pool-lifetime-controlblock--poolhandle)
and [API.md](API.md); remaining rows are scale/API surface, not silent UB.

---

## Future work

Near-term / opportunistic:

- Per-class breakdown on top of `PoolStats` (counters/HWMs already shipped)  
- Intrusive freelist if next-fit + CTZ still dominate profiles  
- Denser free address lookup if `std::map` shows hot at large `m`  
- Thread-local caches; NUMA-aware buffers  
- Lock-free fast paths for common size classes  
- Configurable SpinLock backoff parameters  

### Research / design backlog

Also tracked on the
[research-design-backlog canvas](/home/michaelv/.cursor/projects/home-michaelv-dev-projects-slab-pool-allocator/canvases/research-design-backlog.canvas.tsx)
(open beside chat):

| Item | Notes |
|------|--------|
| Non–fixed-class buffer shaping | Drop ladders entirely; buffers already exact-pack within classes (`kSlabTargetBytes` / multiplier) |
| Pool lifetime vs smart pointers | **Done** — ControlBlock / Handle / BlockToken; orderly + arena + heap retire |
| Bump / size-based arena facility | Sequential allocate, no individual reuse, bulk reclaim - distinct from recycling `Pool` (see [CONCEPTS - Choosing a memory strategy](CONCEPTS.md#choosing-a-memory-strategy)) |
| PMR `memory_resource` adapter | Optional `std::pmr` bridge; keep `PoolAllocator` as the primary / non-virtual STL path |
| Baseline ratio benches | **Shipped** — `make bench` (`bench/bench_allocators.cpp`); re-check after toolchain upgrades |
| C99 wrapper | Opaque pool handle API for C callers |
| Pluggable lock primitive | Compile-time choice of `SpinLock` vs `std::mutex` (or similar) for Pool/Slab |
| Fail-closed allocate policy | Optional: refuse OS fallback when over caps (timing-critical / bounded domains) |

---

## Demos & tests

| Artifact | Proves |
|----------|--------|
| `test/tester.cpp` | Slab growth/trim/`DynBitset` scaling, pool routing/alignment/header validation, multi-thread pool use, smart-pointer exception safety, PoolHandle/BlockToken lifetime, SpinLock contention, LifetimeObserver (including concurrent `isAlive`) |
| `demos/demo_spalloc_shared_ptr.cpp` | Pool as domain + `make_pool_shared` + `PoolAllocator` with STL containers |
| mlvlib `demos/demo_lifetime_observer.cpp` | Single-strand vs mutex-guarded cross-thread observer patterns |

Prefer `make test` under `addrsan` for day-to-day; add `threadsan` when touching
locks or observer atomics. Commands:
[README — Quick Start](../README.md#quick-start).
