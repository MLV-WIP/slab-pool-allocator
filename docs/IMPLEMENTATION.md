# Implementation Guide

Architecture, invariants, design trade-offs, and production readiness for
engineers integrating or extending the allocator.

| Doc | Role |
|-----|------|
| [API.md](API.md) | Contracts, parameters, snippets |
| [CONCEPTS.md](CONCEPTS.md) | Learning / conceptual narrative (pool ≠ slab) |
| [terminology-glossary.md](terminology-glossary.md) | Locked Pool → Slab → Block → Slot vocabulary |
| This file | How the pieces are built and why |
| [README](../README.md) | Build, test, demos |
| [SpinLock Analysis](../mlvlib/docs/deepdive/SpinLock_Analysis.md) | Lock correctness deep dive (mlvlib) |

**Audience:** production correctness first. Explanations stay precise so the same
code remains readable for teaching — without soft-pedaling limits.

**Pool ≠ Slab:** the **Pool** is the allocation domain (lifetime, routing,
isolation). A **Slab** is the size-class engine (fixed-size **Slot** storage in
zero or more **Blocks**) the Pool routes into. Keep those responsibilities
distinct while reading below. See [terminology-glossary.md](terminology-glossary.md).

---

## Table of contents

- [How to read](#how-to-read)
- [Component map](#component-map)
- [Memory layout](#memory-layout)
- [Alignment design](#alignment-design)
- [Request lifecycle (hot & cold paths)](#request-lifecycle-hot--cold-paths)
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
2. **Memory layout** + **Alignment design** — header protocol and alignment classes  
3. **Request lifecycle** — one numeric alloc/free walkthrough  
4. **Pool routing** / **Slab strategy** / **bitsets** / **address lookup** — internals  
5. **Trim**, **smart pointers**, **LifetimeObserver**, **locks** as needed  
6. **Performance**, **debugging**, **future work** when optimizing or hardening  

Skip to [API.md](API.md) for call signatures; use [CONCEPTS.md](CONCEPTS.md) when
you need the “why pools / why slabs” story without code structure.

---

## Component map

```
                    ┌─────────────────────────────────────────┐
                    │  Pool — lifetime / routing domain       │
                    │  (header protocol, lock-free routing)   │
                    └───────────────────┬─────────────────────┘
                                        │
               ┌────────────────────────┼────────────────────────┐
               ▼                        ▼                        ▼
      ISlab (align 16/32/64)     ISlab (size-class)         SlabProxy
      Slab<ElemSize,Align,Lock>  (16 rungs / ladder)   (>class max → ::new[])
               │
               ├── per-Slab Lock (default mlvlib::SpinLock; or std::mutex / NullLock)
               ├── DynBitset block_available_map_  (word/CTZ scan across Blocks)
               ├── std::vector<Block<…>> blocks_   (durable Block indices; trimmed shells)
               │     └── Block                     (aligned buffer, generation_, live_count_, SlotFreemap)
               ├── std::vector<uint32_t> trimmed_blocks_  (O(1) refill list for trimmed shells)
               └── std::vector<BlockAddrEntry> address_index_  (lazy-sorted flat unhinted / align-8 lookup)

  Smart pointers / PoolAllocator / PoolMemoryResource ──► clients via PoolLifetimeToken / PoolHandle
  LifetimeObserver                                    (stand-alone in mlvlib; not part of Pool)
```

| Layer | Responsibility |
|-------|----------------|
| **`Pool`** | Allocation *domain*: construct/destroy as a unit, isolate shards, lock-free routing by footprint + alignment, write/validate the allocation header (including routing hints), `trim` / accounting |
| **`Slab`** | Size-class *engine*: owns `vector<Block>`, `DynBitset` availability map, `trimmed_blocks_` refill list, lazy-sorted flat `address_index_` fallback, next-fit cursor, and per-slab `Lock`; allocates/frees backing buffers **outside** `slab_lock` |
| **`Block`** | Single backing chunk (`spallocator/impl/block.hpp`): RAII aligned buffer (`nullptr` when trimmed), 16-bit `generation_`, `live_count_`, and inline `SlotFreemap<slots_per_block>` |
| **`SlabProxy`** | Same `ISlab` interface; lock-free system `new[]` / `delete[]` for oversized requests (no Block/Slot freemap or retention) |
| **Smart pointers** | Clients via `PoolLifetimeToken` / `PoolHandle` (typed RAII) |
| **`LifetimeObserver`** | Independent of pool/slab (`mlvlib`) |

Destroying a `Pool` destroys its slab backends (each `Slab`'s `vector<Block>`
frees all live backing buffers via `~Block`). Speeding up 64-byte churn is
primarily a slab/block concern; naming “memory for this shard” is a pool
concern. Background: [CONCEPTS.md](CONCEPTS.md#what-is-a-pool-and-why-use-one).

`ISlab` is the narrow virtual interface (`allocateItem` /
`deallocateItem` / reclaim / accounting) that lets `Pool` treat size-class
`Slab`s and `SlabProxy` uniformly after routing, while keeping `Pool`
non-templated even when `Slab` is parameterized by `Lock`.

---

## Memory layout

Every `Pool::allocate(N, alignment)` reserves a **header** in front of the user
payload. The pointer returned to the caller is past that header; free reads
metadata behind it. The **slab** (or `SlabProxy`) only ever sees the **slot
base**; the pool is responsible for the header protocol.

```
Case 1 — header_size = 8 (request align 4 or 8; no routing-hint pad):

  slot base                                                    user pointer
  │                                                            │
  ▼                                                            ▼
┌──────────┬─────────┬──────────────┬─────────────────┬──────────────────┐
│ pad (2B) │ magic   │ header_size  │ alloc_size      │ payload (N B)    │
│          │ (1 B)   │ (1 B)        │ (uint32 / 4 B)  │                  │
└──────────┴─────────┴──────────────┴─────────────────┴──────────────────┘
  −8…−7      −6        −5             −4…−1

Case 2 — header_size ≥ 16 (default align 16, or 32/64; naturally-aligned routing hints):

  slot base                                                                          user pointer
  │                                                                                  │
  ▼                                                                                  ▼
┌───────┬─────────────┬────────────┬──────────┬───────────┬───────┬─────────┬────────────┬──────────────┐
│ pad*  │ block_index │ slot_index │ reserved │ block_gen │ magic │ hdr_sz  │ alloc_size │ payload (NB) │
│       │ (uint32/4B) │ (uint16/2B)│ (u16/2B) │ (u16/2B)  │ (1 B) │ (1 B)   │ (u32/4 B)  │              │
└───────┴─────────────┴────────────┴──────────┴───────────┴───────┴─────────┴────────────┴──────────────┘
          −16…−13       −12…−11      −10…−9     −8…−7       −6      −5        −4…−1
  (*pad is 0 B for align 16, 16 B for align 32, 48 B for align 64)
```

| Offset from user ptr | Type | Present when | Meaning |
|----------------------|------|--------------|---------|
| `−4 … −1` | `uint32_t` | All | Footprint `alloc_size = N + header_size` (selects backend on free) |
| `−5` | `uint8_t` | All | `header_size` itself; must be 8, 16, 32, or 64 |
| `−6` | `uint8_t` | All | `kAllocMagic`; validated then cleared on free |
| `−8 … −7` | `uint16_t` | `header_size ≥ 16` | `block_generation` (non-zero for live `Slab` Blocks; `0` for `SlabProxy`) |
| `−10 … −9` | `uint16_t` | `header_size ≥ 16` | Reserved (`0`; keeps `slot_index` and `block_index` naturally aligned) |
| `−12 … −11` | `uint16_t` | `header_size ≥ 16` | `slot_index` within the owning `Block` (`0 … slots_per_block − 1`) |
| `−16 … −13` | `uint32_t` | `header_size ≥ 16` | Durable `block_index` into `Slab::blocks_` (`kInvalidBlockIndex = UINT32_MAX` for `SlabProxy`) |
| `−header_size … −17` | padding | `header_size ≥ 32` | Leading alignment padding so the user pointer meets `alignment` |

Every multi-byte field is naturally aligned relative to the aligned user
pointer (`uint32_t` at −4 and −16; `uint16_t` at −8, −10, −12). On `align ≤ 8`
(`header_size == 8`), the library deliberately keeps the 8-byte header compact
and falls back to address lookup on free instead of widening the header to
16 bytes.

### Division of labor

| Step | Actor |
|------|--------|
| Choose class / allocate raw slot of `alloc_size` bytes; return `{slot_base, block_index, slot_index, block_generation}` | `Slab` (or `SlabProxy` with sentinel hints) |
| Write magic, `header_size`, `alloc_size`, and (when `header_size ≥ 16`) routing hints; return `slot_base + header_size` | `Pool` |
| On free: validate header, read hints, recover `slot_base`, clear magic & hints (restore on slab throw) | `Pool` |
| Validate hints (or fall back to address lookup), verify exact slot + occupied bit, clear bit / `delete[]` | `Slab` or `SlabProxy` |

The slab never interprets the header. That keeps storage backends simple and
puts all “pointer looks like a pool allocation” policy in one place.

### Free validation (pool)

Before trusting the header (fail closed; read only the fixed bytes immediately
behind the user pointer — do not scan neighboring memory to invent ownership):

1. `header_size ∈ {8,16,32,64}`  
2. Magic equals `kAllocMagic`  
3. Footprint in a plausible range (`header_size ≤ alloc_size ≤ kMaxAllocationBytes + max align`)  
4. Recovered slot base (`user_ptr − header_size`) is aligned to the alignment class implied by `header_size`  
5. When `header_size ≥ 16`, `Pool` reads `(block_index, slot_index, block_generation)`
   and passes them to `ISlab::deallocateItem(...)` for an **O(1) indexed**
   freemap path (`blocks_[block_index]`, generation check, and O(1) geometry
   check `item == blk.slotAddress(slot_index)`). Stale, partial, or forged hints
   fail closed (`std::invalid_argument`) without falling back.

Then clear magic (and hints), ASan-poison the header, re-resolve the backend
(lock-free), and call `slab->deallocateItem(...)`. If the slab throws (for
example, a forged header pointing into an occupied slot of another Block),
`Pool::try_deallocate` restores the header before failing so a live allocation
is not silently corrupted. Align-8 (`header_size == 8`) passes sentinel hints
(`kInvalidBlockIndex, 0, 0`) and takes the unhinted address-search path.

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
| 128 | 16 (default) | 16 | 144 | 192B | 12.5% |
| 100 | 16 (default) | 16 | 116 | 128B | 16% |
| 16 | 16 (default) | 16 | 32 | 32B | 100% |
| 128 | 8 | 8 | 136 | 192B | ~6% |
| 64 | 64 | 64 | 128 | 128B (alignment class 64) | 100% |

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
`alignof(T)` from factories) and a **routing key** into an alignment-class size ladder.

| Rule | Detail |
|------|--------|
| Default | `alignment = kDefaultAlignment` (`__STDCPP_DEFAULT_NEW_ALIGNMENT__`, typically **16**) |
| Allowed | Power of two in `[4, 64]`; else `invalid_argument` in **all** builds (including release) |
| Header width | `header_size = max(8, alignment)` |
| Native alignment class | `nativeAlignmentClass`: 4/8/16 → **16**; 32 → 32; 64 → 64 |
| Storage | `Slab<ElemSize, Alignment>` per alignment ladder |
| High-align ladders | 32/64 engines installed eagerly; Blocks may map on first use |
| Over alignment-class max | `SlabProxy` with `align_val_t{64}` |
| Factories | `static_assert(alignof(T) ≤ 64)` |

Non-power-of-two values in range (for example 12) are rejected even in release:
header/alignment-class math assumes power-of-two alignment.

### Why compound keys?

High-align requests must not share a native-16 slot layout with a narrower
header story. If a 64-align object lived in an alignment-class-16 128B slot with only an
8-byte header, free could not recover a consistent alignment class from `header_size`, and
slot bases would not guarantee the user’s alignment. Separate
`Slab<ElemSize, Alignment>` specializations keep:

- slot stride = `ElemSize` (multiple of `Alignment`)  
- freemap bit ↔ exact slot base  
- `header_size` recoverable as 8/16/32/64 and mapped back to an alignment class  

### Alignment ladders

```
Alignment class 16 (eager): 16 … 4096
Alignment class 32:  32 … 8192
Alignment class 64:  64 … 16384
```

Request align 4/8 **over-aligns** into alignment class 16 (slots are at least 16-aligned).
That is intentional: one hot ladder for the common case; pay for 32/64 only on
first use.

**Examples:**

| Call | Header | Footprint | Path |
|------|--------|-----------|------|
| `allocate(64)` | 16 | 80 | 96B class, alignment class 16 |
| `allocate(64, 8)` | 8 | 72 | 96B class, alignment class 16 |
| `allocate(64, 64)` | 64 | 128 | 128B class, alignment class 64 |

### Ladder and prealloc sizing (empirical context)

Empirical allocator studies (classic Zorn/Grunwald through Costa/Matias and
Zhou et al. ASPLOS 2024 on TCMalloc fleets) repeat a few stable patterns:

1. **Allocation count** is dominated by small objects (often medians well under
   100 B; warehouse fleets: objects <1 KiB can be ~98% of *count*).
2. **Memory volume** is dominated by fewer larger objects (multi-KiB and above).
3. Most objects **die young**; real programs mix ramps, peaks, and plateaus
   rather than Poisson arrivals.
4. A small set of distinct sizes often accounts for most of the call rate
   (classic result: top ~10 sizes ~90% of allocs in some C workloads).

General-purpose mallocs (jemalloc, TCMalloc, ...) respond with many fine size
classes in the small regime and a separate large/page path. That density is
tuned for a *process-global* heap with fleet-wide histograms. This library is a
**named domain** allocator: a jemalloc-shaped geometric ladder on the hot alignment class,
then `SlabProxy` for overflow, is intentional - not a goal to match TCMalloc's
~60-90 classes.

**How the shipped ladders fit those patterns**

| Choice | Rationale |
|--------|-----------|
| Alignment-class-16 rungs through **4096** | Covers small/medium count-dominated traffic for default / native-class traffic |
| Proxy beyond alignment-class max | Large *volume* should not force every multi-KiB size through slot freemaps |
| Default prealloc: alignment class <=16 and `ElemSize` <= **256** | Warm cold RSS where call rate concentrates; leave 384-1024 first-touch unless raised |
| High-align alignment classes not pre-warmed by default | High-align traffic is rare vs the mass of the size distribution |

Always reason in **footprint** (user + header), not bare user size. Default
`allocate(16)` lands in the **32** class (footprint 32 with a 16-byte header),
so the **16** rung is often cold for typical payloads — that is header math,
not a missing research size. Explicit `allocate(N, 8)` can still hit the 16B
rung for small `N`.

**Fine-tuning guidance (measure first)**

- Do **not** densify toward TCMalloc's class count without a histogram of *this*
  pool's clients: more rungs cut internal waste but raise empty-class RSS and
  engine count.
- If adjusting spacing, bias the **bottom** of alignment class 16 (systematic round-up
  cliffs in the <=128 B user / <=~136 B footprint band), not the rare large end.
- Raising `kPreallocateMaxElemSize` toward 512 or 1024 (still alignment class 16) trades
  construct RSS for fewer first-mmap stalls on cold <1 KiB classes; lowering
  toward 128 only if those mid rungs stay unused.
- Raising `kPreallocateMaxAlignment` to 32/64 is usually unjustified by size-
  distribution research alone.
- Block packing (`kSlabTargetBytes` / multiplier — sizes **Blocks**) is a
  separate RSS/growth knob from ladder spacing; tune it from multi-Block
  peak/live-set behavior, not from malloc size-class papers.

Without a client size histogram (or future per-class `PoolStats`), treat ladder
edits as speculative. Configure knobs above are the low-risk experiments.

---

## Request lifecycle (hot & cold paths)

This section traces every layer (`Pool` → `ISlab` → `Slab` → `Block` /
`SlotFreemap`, plus `SlabProxy`) through **allocate** and **deallocate**,
calling out lock boundaries and asymptotic cost at each step.

### At-a-glance path matrix

| Operation & Path | Trigger | Lock held? | Lookup / scan cost | OS heap call? |
|---|---|---|---|---|
| **Alloc — Hot** (cursor hit) | Warm `Slab`, cursor `Block` & `Slot` free | `slab_lock` (brief) | **O(1)** bit test + set | No |
| **Alloc — Warm** (same Block scan) | Cursor `Slot` occupied, same `Block` has free slot | `slab_lock` | **O(slots / 64)** (`SlotFreemap` CTZ) | No |
| **Alloc — Warm** (other Block scan) | Cursor `Block` full, another `Block` available | `slab_lock` | **O(blocks / 64)** (`DynBitset` CTZ) + **O(slots / 64)** | No |
| **Alloc — Cold A** (refill / grow) | All live `Block`s full (or first touch on cold Slab) | **Released** during `new[]`; re-acquired to publish | **O(1)** amortized shell pop / vector append + **O(1)** `address_index_` append | Yes (`allocateRawBuffer` **outside** lock) |
| **Alloc — Cold B** (oversized) | `alloc_size` > alignment-class max | **None** (`SlabProxy`) | **O(1)** routing (`selectClassIndex`) | Yes (`operator new[]`) |
| **Dealloc — Hot** (`align ≥ 16` Pool) | `header_size ≥ 16` with valid `(block_index, slot_index, gen)` | `slab_lock` (brief) | **O(1)** direct array index + geometry verify; **0 address-index lookups, 0 division** | No |
| **Dealloc — Cold A1** (unhinted, 1 Block) | `align ≤ 8` Pool or direct unhinted `Slab` call; `m == 1` | `slab_lock` | **O(1)** single-Block range check + `offset / ElemSize` | No |
| **Dealloc — Cold A2** (unhinted, `m > 1`) | `align ≤ 8` Pool or direct unhinted `Slab` call; `m > 1` | `slab_lock` | **O(log m)** contiguous binary search over `address_index_` (lazy `std::sort` once if grown out-of-order) + `offset / ElemSize` | No |
| **Dealloc — Cold B** (oversized) | Footprint > alignment-class max | **None** (`SlabProxy`) | **O(1)** routing | Yes (`operator delete[]`) |

*(Here `m` = number of live `Block`s in that single `Slab` size class, `blocks` =
durable `blocks_.size()` capacity including trimmed shells, `slots` =
`slots_per_block`, and `64` = bits per `uint64_t` freemap word.)*

---

### Allocate — Hot path (`Pool::allocate(100)`, default align 16)

1. **`Pool` — validate & classify (unlocked, O(1)):**
   - Reject `item_size > kMaxAllocationBytes` (`std::out_of_range`) and
     non-power-of-two or out-of-range alignment (`std::invalid_argument`) in all
     builds.
   - Compute `header_size = max(8, alignment)` (`16`),
     `alloc_size = item_size + header_size` (`116`), and
     `alignment_class = nativeAlignmentClass(alignment)` (`16`).
2. **`Pool::resolveSlab` — lock-free routing (unlocked, O(1)):**
   - `selectClassIndex(116, 16)` returns the `128B` rung index via a short
     branching ladder of `constexpr` comparisons.
   - Reads `slabs16[index].get()`. Every `std::unique_ptr<ISlab>` entry is
     installed once in `Pool()` and never replaced or nulled, so **no Pool lock
     exists or is acquired**.
   - Calls virtual `ISlab::allocateItem(alloc_size)` (one vtable dispatch).
3. **`Slab::allocateItem` — size check & lock acquire (O(1)):**
   - Checks `size <= ElemSize` unlocked (throws `std::invalid_argument` on
     misuse in all builds).
   - Acquires per-slab `slab_lock` (`std::scoped_lock<Lock>` — compiles away
     completely when `Lock` is `mlvlib::NullLock`) and calls
     `tryAllocateFromExistingLocked()`.
4. **`Slab::tryAllocateFromExistingLocked` — Block selection (under `slab_lock`):**
   - Reads `nblocks = block_available_map_.size()` (plain `std::size_t` load,
     avoiding a division by non-power-of-two `sizeof(Block)`). If `0`, returns
     `false` → Cold Path A.
   - Resumes at next-fit cursor `(alloc_cursor_block_, alloc_cursor_slot_)`.
   - **Hot check (O(1)):** tests `block_available_map_.test(block_index)`. If set
     (the cursor Block has at least one free Slot), skips straight to `Block`.
   - **Warm fallback (`findAvailableBlockLocked`, O(blocks / 64)):** if the
     cursor Block is full (or trimmed), scans `block_available_map_` forward from
     `block_index + 1` and wraps `[0, block_index)` using word-level
     `DynBitset::find_next_set()` (`std::countr_zero` skips 64 full/trimmed
     Blocks per word). Resets `slot_start = 0`. If no bit is set, returns `false`
     → Cold Path A.
5. **`Block::allocateSlot(slot_start)` — Slot acquisition (under `slab_lock`):**
   - **Hot fast path (O(1), inlined into `Slab`):** checks
     `slot_start < slots_per_block && isLive() && !isFull() && !freemap_.test(slot_start)`.
     If clear, sets the bit (`freemap_.set(slot_start)`), increments
     `live_count_`, and returns `slot_start`.
   - **Warm fallback (`Block::allocateSlotSlow`, O(slots / 64)):** if `slot_start`
     is already occupied, calls `SlotFreemap::find_next_clear(slot_start)`
     (64-bit word scan + `std::countr_zero`) and wraps once within `[0, slot_start)`
     before setting the bit and incrementing `live_count_`.
6. **`Slab` — cursor & availability update, release lock (under `slab_lock`, O(1)):**
   - Only on the non-full → full edge (`blk.isFull()`, O(1) via
     `live_count_ == slots_per_block`), clears `block_available_map_.reset(block_index)`.
   - Advances `(alloc_cursor_block_, alloc_cursor_slot_)` to the next Slot
     (wrapping at `slots_per_block` and `nblocks`).
   - Populates `AllocateItemResult{blk.slotAddress(item_index), block_index, item_index, blk.generation()}`.
   - Releases `slab_lock`.
7. **`Pool::allocate` — publish header & accounting (unlocked, O(1)):**
   - Under ASan, unpoisons `[slot_base, slot_base + header_size)`.
   - Writes `alloc_size` (`−4`), `header_size` (`−5`), `kAllocMagic` (`−6`), and
     because `header_size >= 16`, writes `block_generation` (`−8`), `reserved = 0`
     (`−10`), `slot_index` (`−12`), and `block_index` (`−16`).
   - Updates `PoolControlBlock::note_alloc()` (atomic `live_allocs_` increment)
     and high-water mark stats (`live_hwm_`, `retained_hwm_`, optional counters).
   - Returns `slot_base + header_size` (**user pointer**). Exclusive caller
     ownership begins here.

---

### Allocate — Cold paths

#### Cold Path A: Slab growth / trimmed-Block refill (`Slab::allocateSlow`)

Entered when `tryAllocateFromExistingLocked` returns an empty result (`ptr == nullptr`, either first touch
on a cold Slab or all live Blocks are full):

1. **Pre-check cap (under `slab_lock`):** before releasing the lock,
   `allocateItem` checks `live_block_count_ < max_blocks` (throwing
   `std::out_of_range` immediately without touching the OS heap if the class cap
   is already reached).
2. **Release `slab_lock`, allocate raw buffer outside lock:**
   - `allocateSlow()` declares `RawBufferGuard staged_buf` *before* the lock
     scope so any unused buffer is automatically freed *after* `slab_lock`
     releases (both on normal return and on exception).
   - Calls `Block::allocateRawBuffer()` (`::operator new[](block_bytes, align_val_t{Alignment})`)
     **with no lock held**. Other threads freeing into this Slab or racing to
     grow it are not stalled behind `malloc` / `mmap` / page faults.
3. **Re-acquire `slab_lock` and double-check:**
   - Calls `tryAllocateFromExistingLocked()` again. If another thread
     freed a Slot or published a Block while we were allocating outside the lock,
     we consume that Slot immediately and return; `staged_buf` is freed by
     `~RawBufferGuard()` **after** `slab_lock` unlocks.
   - Re-checks `live_block_count_ < max_blocks` (throwing cleanly if another
     thread took the last allowed Block while we were unlocked).
4. **Publish buffer (`publishBlockBuffer`, under `slab_lock`):**
   - First reserves `address_index_` geometrically (`capacity * 2`, amortized **O(1)**)
     before mutating any Slab container so appending `{raw_buf, idx}` cannot throw.
   - **Sub-path A1 — Refill a trimmed `Block` shell (`!trimmed_blocks_.empty()`, O(1) amortized):**
     - Pops `idx = trimmed_blocks_.back()`.
     - Appends `{raw_buf, idx}` to `address_index_` (**O(1)**; clears
       `address_index_sorted_` only if `raw_buf < address_index_.back().base`,
       avoiding O(m²) shifts when OS addresses arrive in descending/LIFO order).
     - Calls `blocks_[idx].attachBuffer(raw_buf.release(), gen)` (using and
       advancing the 16-bit non-zero `next_block_generation_` counter to defeat
       ABA against any stale pointer from the previous buffer at index `idx`),
       sets `block_available_map_.set(idx)`, and increments `live_block_count_`.
   - **Sub-path A2 — Append a new `Block` (`trimmed_blocks_.empty()`, O(1) amortized):**
     - If `blocks_.size() == blocks_.capacity()`, grows `blocks_` geometrically
       (`capacity * 2`, amortized **O(1)**) and reserves `block_available_map_`
       *before* mutating any container size so the final commit is non-throwing.
     - Appends `{raw_buf, idx}` to `address_index_` (**O(1)**), calls
       `blocks_.emplace_back(raw_buf.release(), gen)`,
       `block_available_map_.push_set()` (amortized **O(1)**), advances
       `next_block_generation_`, `++live_block_count_`.
5. **Take Slot 0 (under `slab_lock`, O(1)):** sets cursor to
   `(published_index, 0)` and calls `tryAllocateFromExistingLocked()` to
   claim Slot 0, then releases `slab_lock` and returns to `Pool::allocate` step 7.

#### Cold Path B: Oversized allocation (`SlabProxy`)

When `alloc_size` exceeds the alignment class's maximum rung (e.g., `> 4096` for
alignment class 16):

- `resolveSlab` returns `&large_slab` (lock-free, **O(1)**).
- `SlabProxy::allocateItem(alloc_size)` allocates directly via aligned
  `operator new[]` (`align_val_t{kMaxAlignment}`) with **no lock**, **no
  Block**, and **no freemap**, returning `{ptr, kInvalidBlockIndex, 0, 0}`.
- `Pool::allocate` writes the same front header (with sentinel hints
  `kInvalidBlockIndex / 0 / 0`) and returns `ptr + header_size`.

---

### Deallocate — Hot path (`align ≥ 16`, O(1) indexed free)

1. **`Pool::try_deallocate` — peek & validate header (unlocked, O(1)):**
   - Null pointer returns `true` immediately (`Pool::deallocate(nullptr)` is a
     no-op).
   - Under ASan, briefly unpoisons the fixed peek window if already poisoned so
     double-free/corrupt-header checks fail cleanly instead of ASan-aborting first.
   - Validates `header_size ∈ {8, 16, 32, 64}`, `magic == kAllocMagic`,
     `alloc_size ∈ [header_size, kMaxAllocationBytes + kMaxAlignment]`, and
     `original_ptr = item - header_size` aligned to `alignment_class`.
     Any mismatch fails closed (`false` from `try_deallocate`,
     `std::invalid_argument` from `deallocate`, `std::abort` from `PoolDeleter`).
   - Because `header_size >= 16`, reads `hint_block_index` (`−16`),
     `hint_slot_index` (`−12`), and `hint_gen` (`−8`).
   - Invalidates the header *before* slab work (`magic = 0`, hints cleared,
     ASan-poisoned) to catch concurrent double-frees early.
2. **`Pool::resolveSlab` — lock-free re-routing (unlocked, O(1)):**
   - Re-selects the owning `ISlab*` from `(alloc_size, alignment_class)` — never
     trusts the caller for size.
   - Calls virtual `slab->deallocateItem(original_ptr, hint_block_index, hint_slot_index, hint_gen)`.
3. **`Slab::deallocateItem` — O(1) indexed fast path (under `slab_lock`):**
   - Acquires `slab_lock`.
   - Sees `has_index && has_gen` (`block_index != kInvalidBlockIndex && block_generation != 0`).
   - **Zero `address_index_` lookup, zero integer division:**
     1. Bounds check `block_index < block_available_map_.size()` (plain `size_t` load).
     2. Direct index `auto& blk = blocks_[block_index]` (**O(1)**).
     3. Verify `blk.isLive() && blk.generation() == block_generation` (rejects
        trimmed shells, refilled-shell ABA, and cross-Slab/forged hints).
     4. Verify `slot_index < slots_per_block && item == blk.slotAddress(slot_index)`
        (**O(1)** exact slot geometry check — catches interior pointers and
        corrupted `slot_index` without division or tree search).
     5. Verify `blk.testSlot(slot_index)` (rejects already-free double-frees).
     6. `blk.deallocateSlot(slot_index)` — clears the bit in `SlotFreemap` and
        decrements `live_count_` (**O(1)**).
     7. Full → non-full transition check: if `blk.liveCount() == slots_per_block - 1`,
        sets `block_available_map_.set(block_index)` (**O(1)**).
   - Releases `slab_lock`. Empty Blocks stay mapped until [`Pool::trim`](#trim-and-rss).
4. **`Pool::try_deallocate` — rollback on error or finalize accounting (unlocked, O(1)):**
   - If `deallocateItem` threw (e.g., forged hint), restores `magic` and hints
     so the untouched live Slot stays valid, records fail stats, and returns
     `false` (or throws in `Pool::deallocate`).
   - Otherwise calls `control_->note_free()` (atomic decrement of `live_allocs_`
     + optional heap-owned retirement check; if `try_retire()` returns `this`,
     `delete doomed` runs only *after* `Pool` is done touching members).

---

### Deallocate — Cold paths

#### Cold Path A: Unhinted free (`align ≤ 8` Pool or direct `Slab::deallocateItem(ptr)`)

When `header_size == 8` (request align 4 or 8) or a caller invokes
`Slab::deallocateItem(ptr)` directly with default sentinel hints
(`kInvalidBlockIndex, 0, 0`):

- Note: partial hints (`has_index != has_gen`) are rejected immediately with
  `std::invalid_argument` (fail closed).
- Under `slab_lock`, tail-calls `deallocateUnhintedLocked(item)` (split out with
  `SPALLOCATOR_NOINLINE` so the hinted `deallocateItem()` fast path stays
  compact with minimal prologue spills while unhinted free still receives full
  `-O3` optimization):
  - **Sub-path A1 — Single live Block (`block_available_map_.size() == 1 && blocks_[0].isLive()`, O(1)):**
    range-checks `blocks_[0].containsAddress(item)` directly without touching
    `address_index_`.
  - **Sub-path A2 — Multi-Block (`findBlockForSlotLocked(item)`, O(log m)):**
    lazily runs `std::sort` on `address_index_` once if any newly published
    Blocks arrived out of ascending address order, then runs a contiguous binary
    search over the flat `BlockAddrEntry` array (16 B/entry, cache-line dense)
    and verifies `item ∈ [block_start, block_start + block_bytes)`.
- Indexes `auto& blk = blocks_[resolved_block]`, computes
  `byte_offset = item - blk.buffer()` and `item_index = byte_offset / ElemSize`,
  verifies exact slot alignment (`item_index < slots_per_block && item == blk.slotAddress(item_index)`)
  to reject interior pointers, verifies `blk.testSlot(item_index)`, calls
  `blk.deallocateSlot(item_index)`, and sets `block_available_map_.set(resolved_block)`
  on the full → non-full edge.

#### Cold Path B: Oversized free (`SlabProxy`)

When `alloc_size` exceeds the alignment class's max rung, `resolveSlab` returns
`&large_slab`. `SlabProxy::deallocateItem` ignores routing hints and calls
aligned `operator delete[]` immediately with **no lock** and **no retention**.

### Footprint → class (default align → typically +16 header)

| User N | Footprint | Size class | Free routing path |
|--------|-----------|------------|-------------------|
| 16 | 32 | 32 | O(1) indexed (`header_size = 16`) |
| 100 | 116 | 128 | O(1) indexed (`header_size = 16`) |
| 128 | 144 | 192 | O(1) indexed (`header_size = 16`) |
| 8 (explicit align 8) | 16 | 16 | Unhinted fallback (`header_size = 8`, O(1) if `m=1`, else O(log m)) |

Always classify by **footprint**, not bare user size. Mis-estimating waste in
tests or docs almost always means forgetting the header.

---

## Pool routing & registries

`Pool` is the directory of backends for one allocation domain.

| Registry | When built | Contents |
|----------|------------|----------|
| `slabs16[kLadder16Count]` (16 rungs) | With the pool (eager) | One size-class **Slab** per ladder rung (`16 … 4096`); **never replaced/nulled** |
| `slabs32[kLadder32Count]` (16 rungs) | With the pool (eager) | High-align 32 ladder (`32 … 8192`); immutable after `Pool()` |
| `slabs64[kLadder64Count]` (16 rungs) | With the pool (eager) | High-align 64 ladder (`64 … 16384`); immutable after `Pool()` |
| `large_slab` | With the pool | Single `SlabProxy` instance |

**Terminology (Slab vs Block vs Slot):** a **Slab** is the size-class engine for
one `(ElemSize, Alignment)`. It owns a durable index table of **Blocks**
(`std::vector<Block<ElemSize, Alignment>>`), each owning one mapped chunk of
equal **Slots** (or `nullptr` when trimmed). Full glossary:
[terminology-glossary.md](terminology-glossary.md).

**Block accounting distinction:**
- `Slab::getBlockCount()` returns `live_block_count_` — the number of **live
  backing buffers** currently mapped (matches `getAllocatedMemory() / block_bytes`).
- `Slab::getBlockCapacity()` returns `blocks_.size()` — the durable index table
  length (live Blocks + trimmed shells awaiting O(1) refill).

**Routing:** `resolveSlab(alloc_size, alignment_class)` runs `selectClassIndex`. A normal
index picks a size-class Slab; `kSlabIndexNpos` means footprint exceeds that
alignment class’s max → `large_slab`. Lookups are **lock-free**.

**Cold-start Blocks:** every Slab exists after `Pool()`, but only some map a
first Block at construction. Policy (override via `-D` / `./configure`):

| Knob | Default | Meaning |
|------|---------|---------|
| `kPreallocateMaxElemSize` | 256 | Preallocate when `ElemSize <=` this |
| `kPreallocateMaxAlignment` | 16 | …and `Alignment <=` this (raise to 32/64 to warm high-align alignment classes) |

Defaults bias warm Blocks toward the small-object call-rate band; see
[Ladder and prealloc sizing](#ladder-and-prealloc-sizing-empirical-context).

Other Slabs stay at zero live Blocks until the first `allocateItem` (via
`allocateSlow()`). `trim` only detaches empty **Block** backing buffers inside
Slabs — never registry indices.

`selectClassIndex` is a pure classification helper: usable from
tests and constexpr-friendly call sites, and shared by allocate and free so both
paths agree on the ladder.

---

## Slab strategy

Each `Slab<ElemSize, Alignment, Lock = DefaultSlabLock>` is a **fixed-size recycler** for one rung of
one alignment class.

### `Block<ElemSize, Alignment>` (`spallocator/impl/block.hpp`)

Each Block is a non-copyable, `noexcept`-movable RAII value stored inside
`Slab::blocks_`:

- `std::byte* buffer_` — aligned backing chunk of `block_bytes` bytes (`nullptr`
  when trimmed into an empty shell)
- `std::uint16_t generation_` — non-zero monotonic tag assigned on attach/refill
  (`0` when trimmed); defeats ABA when a trimmed index is reused
- `std::uint16_t live_count_` — exact count of occupied Slots (`O(1)` `isEmpty()`
  and `isFull()`, avoiding multi-word freemap scans on edge checks)
- `SlotFreemap<slots_per_block> freemap_` — fixed `uint64_t` word array
  (`1 = occupied`, `0 = free`)

`block_bytes` comes from `selectBlockSize<ElemSize>()`: exact pack of
`max(kSlabTargetMultiplier, kSlabTargetBytes / ElemSize)` Slots with zero tail
padding (defaults: **8 KiB** target, **16** minimum Slots; overridable via
`./configure --with-slab-target-*`). `slots_per_block` is `static_assert`ed to
fit in `uint16_t` so `slot_index` fits cleanly in the 16-byte Pool header.

### `Slab` state

Members are ordered **hot-first** so the allocate/deallocate hot fields
(`blocks_`, `block_available_map_`, cursor, and `slab_lock`) stay packed into
the leading cache lines while growth/trim/unhinted-lookup metadata sits behind
them:

- `std::vector<Block<ElemSize, Alignment>> blocks_` — durable index table
  (`block_index` never shifts on trim; geometric capacity growth on append)
- `DynBitset block_available_map_` — 1 bit per `blocks_` entry (`1` iff live AND
  has at least one free Slot; trimmed and full Blocks both have `0`)
- Next-fit cursor `(alloc_cursor_block_, alloc_cursor_slot_)`
- `MLVLIB_NO_UNIQUE_ADDRESS Lock slab_lock` — per-slab lock (defaults to
  `DefaultSlabLock` = `mlvlib::SpinLock`)
- `live_block_count_` (capped at `max_blocks = kMaxClassBytes / block_bytes`,
  default **4 GiB** class cap)
- `std::vector<BlockAddrEntry> address_index_` + `bool address_index_sorted_` —
  compact 16 B/entry flat array mapping live `buffer_` base → durable
  `uint32_t block_index` (O(1) append on publish, lazily sorted on first
  unhinted lookup if out of order; used only by unhinted / align-8 frees and
  `findBlockForSlot()`)
- `std::vector<std::uint32_t> trimmed_blocks_` — LIFO list of trimmed `Block`
  indices ready for **O(1) refill** on the next growth
- `next_block_generation_` (`uint16_t` wrapping `1..65535`, skipping `0`)

Cold slow paths (`Block::allocateSlotSlow`, `Slab::findAvailableBlockLocked`,
`Slab::allocateSlow`, and out-of-line `detail::throw*` helpers) are annotated
with `SPALLOCATOR_COLD_NOINLINE` so exception construction / `std::format` IR
and landing pads stay out of `allocateItem()` and `deallocateItem()` — see
[Hot/cold codegen & `SPALLOCATOR_*INLINE` macros](#hotcold-codegen--spallocator_inline-macros)
for details.

**Invariants**

- `ElemSize` is a multiple of `Alignment`; `Alignment ∈ {16, 32, 64}`  
- `block_available_map_.size() == blocks_.size()` at all quiescent points under
  `slab_lock`  
- `block_available_map_.test(i)` is `1` iff `blocks_[i].isLive() && !blocks_[i].isFull()`  
- `live_block_count_ == address_index_.size()`  
- Every live `Block`'s durable index `i` remains constant for the lifetime of the
  `Slab` (no swap-with-last); trimming detaches `buffer_` and bumps `generation_`
  on refill  
- Every set freemap bit corresponds to a live user allocation whose slot base
  the pool wrapped with a header  

**Growth:** when no live Block has a free Slot, `allocateSlow()` allocates a raw
backing buffer **outside** `slab_lock`, re-acquires and double-checks, then
publishes it either by refilling a trimmed shell from `trimmed_blocks_` or
appending a new `Block` to `blocks_`. **Monotonic by default:** a fully empty
Block is *not* returned to the OS on the last free — see [Trim and RSS](#trim-and-rss).

### `SlabProxy`

Large / overflow path behind the same `ISlab` interface:

- `allocateItem` → aligned `new[]` (unlocked), returns sentinel hints
  (`kInvalidBlockIndex, 0, 0`)  
- `deallocateItem` → aligned `delete[]` (unlocked; no freemap, no retention)  
- No address map: each allocation is standalone  

The pool still attaches the usual header on the returned user pointer so free
looks the same. Choosing proxy vs size-class is purely a routing decision from
footprint + alignment-class max.

---

## Bitset tracking

Two levels of bits, owned by the **Slab** (`DynBitset`) and each **Block**
(`SlotFreemap`):

| Structure | Owner | Word scan | Role |
|-----------|-------|-----------|------|
| `SlotFreemap<N>` | Per `Block` | `find_next_clear(start)` via `std::countr_zero` (**O(slots / 64)**) | Bit per Slot: `1` = occupied, `0` = free |
| `DynBitset` (`block_available_map_`) | Per `Slab` | `find_next_set(start)` (`std::countr_zero`) & `find_prev_set(before)` (`std::countl_zero`) (**O(blocks / 64)**) | Bit per `blocks_` entry: `1` iff live AND has free capacity (`0` if full or trimmed) |

`DynBitset` grows geometrically with `blocks_.size()` — empty size classes do
not embed a fixed ~128 KiB phantom map (an earlier fixed-bitset design paid that
tax even when idle).

### Why next-fit + word/CTZ (not intrusive freelist)?

Bitsets stay the single source of truth for occupancy and double-free detection,
while `Block::live_count_` makes `isEmpty()` / `isFull()` **O(1)** without
scanning words. Next-fit keeps the common LIFO / sequential alloc path strictly
**O(1)** (`!freemap_.test(slot_start)`), and 64-bit word/CTZ search makes cold
scans **O(slots / 64)** inside a `Block` and **O(blocks / 64)** across `Block`s.
An intrusive freelist duplicates free-set state and touches cold payload memory
on every alloc/free; keep it as a profile-driven S5+ candidate only if profiles
show bitset scans dominating.

---

## Address lookup

On free, the Slab must locate the owning **Block** and **Slot** for `slot_base`:

- **Primary path (`Pool` with `align ≥ 16`, `header_size ≥ 16`):** **O(1) direct
  indexed lookup** via `blocks_[block_index]`, verified in O(1) against
  `blk.isLive()`, `blk.generation() == block_generation`, and
  `slot_index < slots_per_block && item == blk.slotAddress(slot_index)`.
  `address_index_` is **never touched** on this path.
- **Fallback path (`Pool` with `align ≤ 8`, direct unhinted `Slab` calls, and
  `findBlockForSlot()`):** Blocks occupy half-open intervals
  `[base, base + block_bytes)`. `address_index_` (`std::vector<BlockAddrEntry>`,
  16 B per live Block) maps each live Block's base address to its durable
  `uint32_t` index without per-node heap allocations or red-black-tree pointer
  chasing:
  - **O(1)** direct `blocks_[0]` check in `deallocateUnhintedLocked` when
    `block_available_map_.size() == 1 && blocks_[0].isLive()`.
  - **O(log m)** contiguous binary search (`upper_bound` style, checking
    `slot_base ∈ [block_start, block_end)`) when `m > 1`. New Blocks are
    appended in **O(1)** during growth and lazily sorted once on the first
    locked unhinted lookup if any OS backing addresses arrived out of order.

| Lookup path | Cost | When used |
|-------------|------|-----------|
| Header hint (`block_index`, `slot_index`, `gen`) | **O(1)** | Default `Pool` (`align ≥ 16`) |
| Single-Block fast path (`m == 1`) | **O(1)** | `align ≤ 8` / direct `Slab`, 1 live Block |
| Flat `address_index_` binary search (`m > 1`) | **O(log m)** | `align ≤ 8` / direct `Slab`, multiple live Blocks |

### Interior pointers

The freemap bit corresponds to an **exact** slot base. Freeing
`user_ptr + 1`, a mid-slot address, or a pointer into padding must not clear a
neighbor’s bit:

- On the hinted path, `item == blk.slotAddress(slot_index)` rejects any offset
  mismatch in **O(1)** without integer division.
- On the unhinted path, `item_index = (item - block_start) / ElemSize` followed
  by `item == blk.slotAddress(item_index)` rejects any misaligned/interior
  address.

Combined with pool header validation, accidental interior frees fail loudly
(`std::invalid_argument` / `false` / `std::abort` per API) instead of corrupting
the freemap.

---

## Trim and RSS

Ordinary free returns a **Slot** to a `Block`'s `SlotFreemap`. It does not
shrink the process image. Peak RSS after a burst therefore tends to stick until
the caller explicitly invokes `Pool::trim()`.

| Mechanism | Layer | Behavior |
|-----------|-------|----------|
| Ordinary free | `Slab` / `Block` | Clears Slot bit; empty `Block` stays mapped |
| `Slab::reclaimEmptyBlocks(keep_min)` | `Slab` | Synchronously scans `block_available_map_` **high-to-low** via `find_prev_set`, detaches empty `Block` backing buffers, pops trailing trimmed `Block` shells (and shrinks excess metadata capacity with 4× hysteresis), and **frees the raw buffers outside `slab_lock`**; retains ≥ `keep_min` live Blocks |
| `Pool::trim(keep_min)` | `Pool` | Walks all size-class `Slab`s synchronously and calls `reclaimEmptyBlocks(keep_min)`; domain policy knob |
| `SlabProxy` free | `SlabProxy` | Memory returned immediately every time |

**Durable indices vs swap-with-last (no background thread):**
- Earlier revisions used swap-with-last during `reclaimEmptyBlocks`, which
  renumbered surviving Blocks and prevented storing a stable `block_index` in
  allocation headers.
- With durable indices, trimming an empty `Block` detaches its ≥8 KiB backing
  buffer (`blk.detachBuffer()` → `buffer_ = nullptr`, `generation_ = 0`),
  clears its bit in `block_available_map_`, pushes `idx` onto `trimmed_blocks_`,
  and batch-removes detached entries from `address_index_` in one linear
  `std::erase_if` pass. Surviving live Blocks keep their exact `block_index`
  unchanged.
- **Trailing-shell compaction in `trim()` only (never on `deallocateItem`):**
  `reclaimEmptyBlocks` scans empty Blocks **high-to-low** (`find_prev_set`) so
  lower-indexed empty Blocks are kept first, then synchronously pops all
  trailing `!isLive()` `Block` shells from `blocks_` (and `block_available_map_`),
  prunes those indices from `trimmed_blocks_`, and applies best-effort
  `shrink_to_fit()` with 4× hysteresis once `blocks_.capacity() > 64`. If the
  Slab is completely idle with its sole empty Block stranded above trimmed
  shells (e.g., after a high-index straggler was later freed), `trim()`
  relocates that empty buffer to index `0` with a freshly bumped `generation_`
  (zero OS heap calls) and pops all remaining shells. Interior trimmed shells
  below a still-live higher-index Block stay at ~24–80 B for O(1) refill until
  the tail becomes empty and `trim()` runs again. Because
  `next_block_generation_` is Slab-wide and monotonic, re-appending or
  relocating an index still rejects stale routing hints.
- **No cleanup thread is spawned.** Both backing-buffer reclamation (outside
  `slab_lock`) and trailing-shell compaction run synchronously inside
  `Pool::trim()` — never during ordinary `deallocateItem()`.

**Why monotonic default?** Alloc/free churn should not mmap/munmap thrash; the
next burst stays warm. **Why expose trim on the pool?** “This shard is quiet —
give memory back” is a **domain** decision. The Pool knows the registry; each
Slab knows which Blocks are empty.

`keep_min` is per size class and clamped to at least 1 so a class does not
oscillate between “zero live Blocks” and “allocate a Block on every hit.”

### Accounting

`getAllocatedMemory()` sums **live size-class `Block` buffer bytes**
(`live_block_count_ * block_bytes`) across the pool’s Slabs (trimmed shells
contribute `0` bytes). It is not:

- a live “bytes currently checked out to clients” gauge  
- inclusive of in-flight `SlabProxy` blocks (those are not retained in the
  registry the same way)  

Use it to understand high-water retention and the effect of `trim`, not as a
full heap profiler.

---

## Smart pointers (design)

Factories and deleters are **clients of the pool domain** via
`PoolLifetimeToken` (and convenience overloads on `Pool&` / `PoolHandle`). They do
not change slab internals; they enforce RAII and exception safety on top.

### Roles

| Piece | Design point |
|-------|----------------|
| `PoolLifetimeToken` | `block_refs` pin on `PoolControlBlock`; observes `alive`; not a handle pin |
| `PoolDeleter` | Placement-new counterpart: **finalize** (`~T()`) then **reclaim** (`try_deallocate`); arrays destroy **reverse** order; if `!alive`, **no-op** both steps (arena); abort on bad free while alive. Stateful (`PoolLifetimeToken`) by necessity - see below |
| `make_pool_unique` | Placement `new`; on ctor throw, return slot (**strong guarantee**); overloads for `Pool&` / `PoolHandle` / token |
| `make_pool_shared` | Placement `new` + `shared_ptr(p, PoolDeleter)`; object in pool, CB on heap |
| `PoolAllocator` | Stateful token; no POCCA/POCMA/POCS; equal iff **same control block**; allocate throws `bad_alloc` if `!alive`; deallocate no-ops if dead |
| `PoolMemoryResource` | Additive `pool_pmr.hpp`; same token/arena model; virtual `memory_resource` for `std::pmr` composition |

### `PoolAllocator` design decisions

1. Holds a **`PoolLifetimeToken`**, not a raw `Pool&` — survives arena `~Pool` so
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

`PoolDeleter` / `PoolAllocator` hold a **`PoolLifetimeToken`** (`block_refs` only —
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

**Destroying delete (non-goal for the generic pool).** A typed destroying
`operator delete` can combine `~T()` and reclaim with full type context (useful
for type-local arenas). The shipped path stays `PoolDeleter` +
`PoolLifetimeToken`: generic over `T`, arena-safe, and independent of per-type
`operator new`/`delete`. Do not replace `make_pool_unique` /
`make_pool_shared` with destroying delete as the library default; document any
future typed facade separately.

**Why a stateful deleter?** Empty / EBO-friendly `unique_ptr` deleters keep
`sizeof(unique_ptr)` at a raw pointer. `PoolDeleter` stores a `PoolLifetimeToken`
so arena `~Pool` remains observable after storage is gone. That enlarges
`unique_pool_ptr` relative to a stateless deleter; the cost buys defined
arena behavior instead of UAF.

Factories accept `Pool&`, `PoolHandle`, or `PoolLifetimeToken`. Prefer
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

Concurrency follows the pool-vs-slab split: **`Pool` routing is lock-free**
because all alignment-class Slab registries (`slabs16`, `slabs32`, `slabs64`)
are installed eagerly in `Pool()` and remain immutable for the pool’s lifetime.
Each **`Slab`** owns an independent per-slab lock protecting its `blocks_`,
`block_available_map_`, `trimmed_blocks_`, `address_index_`, and cursor.
Different size classes therefore run in parallel with zero shared lock
contention, and separate `Pool` instances share no locks at all.

| Lock | Protects | Hold time intent |
|------|----------|------------------|
| Per-`Slab` `Lock` (`DefaultSlabLock` or `Pool(std::in_place_type<Lock>)`) | `blocks_`, `SlotFreemap`s, `block_available_map_`, `trimmed_blocks_`, `address_index_`, cursor | Brief — bit ops, O(1) array index / O(log m) flat binary search; **never** holds OS `new[]` / `delete[]` |

**No OS heap calls under `slab_lock`:**
- **Growth (`Slab::allocateSlow`):** `slab_lock` is released before calling
  `Block::allocateRawBuffer()` (`::operator new[]`). The buffer is staged in a
  `RawBufferGuard` declared *before* the lock guard; after re-acquiring
  `slab_lock`, `Slab` double-checks `tryAllocateFromExistingLocked` before
  publishing. Any redundant or exception-aborted buffer is freed by
  `~RawBufferGuard()` **after** `slab_lock` releases.
- **Trim (`Slab::reclaimEmptyBlocks`):** empty backing buffers are detached and
  collected into a local `to_free` vector under `slab_lock`; the actual
  `Block::freeRawBuffer()` (`::operator delete[]`) calls run **after**
  `slab_lock` is released.

### Compile-time lock policy selection

`Slab<ElemSize, Alignment, Lock = DefaultSlabLock>` is parameterized over any
`mlvlib::BasicLockable` (`mlvlib/lock_traits.hpp`). Because `Pool` stores slabs
behind `std::unique_ptr<ISlab>`, `Pool` itself stays non-templated while still
supporting both build-wide and per-pool lock selection:

| Mechanism | Scope | Options |
|-----------|-------|---------|
| `SPALLOCATOR_LOCK_*` (`slab_mutex.hpp`) / `./configure --with-lock-policy=` / `make LOCK_POLICY=` | Build-wide `DefaultSlabLock` | `spinlock` (`mlvlib::SpinLock`, default), `mutex` (`std::mutex`), `null` (`mlvlib::NullLock`) |
| `Pool(std::in_place_type<Lock>)` | Single `Pool` instance | Any `mlvlib::BasicLockable` (`SpinLock`, `std::mutex`, `NullLock`, custom) |

Key design points:

- **Zero overhead for `NullLock`:** `slab_lock` is declared last with
  `MLVLIB_NO_UNIQUE_ADDRESS`, so `mlvlib::NullLock` adds zero bytes to `Slab` in
  `NDEBUG` builds and `std::scoped_lock<NullLock>` compiles away completely.
- **Debug precondition checks:** internal helpers that require the slab lock
  (`publishBlockBuffer`, `tryAllocateFromExistingLocked`,
  `findAvailableBlockLocked`) call `mlvlib::assert_lock_held(slab_lock)`,
  which verifies current-thread ownership in `!NDEBUG` builds for introspectable
  locks (`SpinLock`, `NullLock`) and compiles to a no-op for `std::mutex` and in
  `NDEBUG`.
- **Metadata without leaking the template:** `ISlab` and `Pool` expose
  `isThreadSafe()` and `lockPolicyName()` (driven by `mlvlib::lock_traits<Lock>`),
  allowing tests, benchmarks, and callers to guard concurrent paths explicitly.
- **Single-threaded safety:** `NullLock` provides no mutual exclusion and emits
  no TSan annotations. Concurrent access to a `NullLock` pool is a caller bug
  (caught by `NullLock` debug owner assertions in `!NDEBUG` and by TSan if
  accidentally raced).

### Ordering intent

- **Common allocate / free (any alignment class):** lock-free `Pool::resolveSlab`
  → single `Slab::slab_lock` (never nested across classes).
- **Cold growth (`allocateSlow`):** `slab_lock` → unlock → `new[]` → re-lock
  `slab_lock` → publish & take Slot → unlock → optional redundant `delete[]`.
- **Trim (`Pool::trim`):** acquires one `Slab::slab_lock` at a time, never two
  concurrently, and calls `delete[]` after releasing each `Slab`’s lock.
- **`SlabProxy` (oversized):** acquires **no** lock at all.

Exact sequences live in `pool.hpp` / `pool_lifetime.hpp` / `impl/block.hpp` /
`impl/slab.hpp` — treat those as source of truth if this summary drifts.

### Client-facing notes

| Client | Thread notes |
|--------|----------------|
| Concurrent `Pool::allocate` / free | Supported on one pool when `pool.isThreadSafe()` is `true` (`SpinLock` / `std::mutex`); single-thread / externally serialized for `NullLock` |
| `unique_pool_ptr` | Unique ownership — cross-thread handoff needs external sync on the pointer |
| `shared_ptr` from `make_pool_shared` | Standard atomic control-block refcounts |
| Multiple `Pool` instances | Separate domains; no shared locks between pools |
| `LifetimeObserver` | Allocator-independent; see its own thread model |

Prefer `std::scoped_lock` or `std::unique_lock` around slab locks so exceptions
cannot leave a lock held (`unique_lock` when early unlock / deferred lock is
needed).

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
burning CPU and blocks via `atomic_flag::wait`. Embeds that prefer OS blocking
or zero-overhead single-threaded domains can select `std::mutex` or
`mlvlib::NullLock` at compile time (`--with-lock-policy=` / `LOCK_POLICY=` /
`Pool(std::in_place_type<Lock>)`).

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

| Operation | Hot / Amortized | Cold / Worst-case | Notes |
|-----------|-----------------|-------------------|-------|
| `Pool::resolveSlab` (`selectClassIndex`) | **O(1)** | **O(1)** | Lock-free ladder of compares + immutable array index |
| Alloc (size class, warm) | **O(1)** | **O(blocks/64 + slots/64)** | Next-fit cursor fast path; CTZ word scans in `DynBitset` & `SlotFreemap` |
| Alloc (size class, grow / refill) | **O(1)** amortized + OS `new[]` | **O(1)** amortized + OS `new[]` | `new[]` runs **outside** `slab_lock`; `trimmed_blocks_` refill or geometric `blocks_` append + O(1) `address_index_` append |
| Free (`Pool`, `align ≥ 16`, hinted) | **O(1)** | **O(1)** | Direct `blocks_[block_index]` + generation + exact slot address check; **zero address-index lookup, zero division** |
| Free (`Pool` `align ≤ 8` / unhinted `Slab`) | **O(1)** (`m = 1`) | **O(log m)** (`m > 1`) | Single-Block range check when `m = 1`; lazy-sorted flat `address_index_` binary search + `offset / ElemSize` otherwise |
| `SpinLock` uncontended / `NullLock` | **O(1)** / **0** | Spin → backoff → `wait` | `NullLock` compiles away completely in `NDEBUG` |
| `SlabProxy` alloc/free | OS dependent | OS dependent | Lock-free; no slot freemap or retention |
| `trim` (`reclaimEmptyBlocks`) | **O(classes × blocks/64)** | **O(classes × (blocks/64 + m))** | Word-level `find_prev_set` scans high-to-low, batch `std::erase_if` on `address_index_`, pops trailing shells (with 4× hysteresis shrink), and frees `k` detached buffers **outside** `slab_lock` |

“Hot O(1)” alloc assumes `block_available_map_` keeps you on a non-full `Block`
and that the next-fit cursor lands on a free Slot (typical sequential / LIFO
churn). A pathological pattern (one free bit far from the cursor every time)
still walks words via `std::countr_zero` — O(slots / 64) inside a Block and
O(blocks / 64) across Blocks.

### Fragmentation

Always reason about **footprint vs class size**, not bare user size vs class.
Waste has three buckets:

1. Header bytes (`max(8, alignment)`)  
2. Internal slack inside the chosen class Slot  
3. Retained empty `Block` backing buffers until `trim` (RSS), plus ~24–80 B per
   trimmed `Block` shell in `blocks_` (reused O(1) on next growth)  

### Hot/cold codegen & `SPALLOCATOR_*INLINE` macros

`spallocator/impl/helper.hpp` defines three portable `#ifndef`-guarded
compiler-hint macros used across `Block` and `Slab` to control code layout,
inlining budgets, and register pressure on the allocation and deallocation fast
paths:

| Macro | GCC / Clang | MSVC | Effect on **caller** | Effect on **callee** | Current call sites |
|-------|-------------|------|----------------------|----------------------|--------------------|
| **`SPALLOCATOR_COLD_NOINLINE`** | `[[gnu::cold, gnu::noinline]]` | `__declspec(noinline)` | Never inlined; branches leading to the call are treated as unlikely (`[[unlikely]]` inferred), keeping the hot path straight-line with minimal stack frame and zero EH landing-pad pollution | Optimized for size (`-Os`) and placed in `.text.unlikely` away from hot I-cache lines | `detail::throwSizeExceedsElemSize`, `detail::throwMaxBlocksExceeded`, `detail::throwInvalidArgument`, `Slab::allocateSlow`, `Slab::findAvailableBlockLocked`, `Block::allocateSlotSlow` |
| **`SPALLOCATOR_NOINLINE`** | `[[gnu::noinline]]` | `__declspec(noinline)` | Never inlined; keeps the primary hot path compact, avoids callee-saved register spills, and allows tail-call dispatch | Compiled at **full `-O3`** in normal `.text` and free to inline its own callees | `Slab::deallocateUnhintedLocked` |
| **`SPALLOCATOR_ALWAYS_INLINE`** | `[[gnu::always_inline]] inline` | `__forceinline` | Forced inline into caller even when compiler cost heuristics hesitate around lock guards or template wrappers; keeps register-returned structs (`AllocateItemResult`) in registers | Inline linkage preserved for header-defined templates | `Slab::tryAllocateFromExistingLocked` |

**Why throws are outlined into non-template `detail::throw*` helpers:**
In C++, writing `throw std::invalid_argument(std::format(...))` directly inside
`Slab::allocateItem()` or `Slab::deallocateItem()` forces the compiler to keep
`std::format` buffers, `std::string` destructors, and exception-handling unwind
landing pads inside the hot function's frame — even though those branches are
never taken in normal execution. That has three compounding costs:
1. **Stack frame & spill tax:** extra callee-saved registers are pushed/popped in
   the prologue/epilogue on *every* hot call, even when the fast path returns
   without throwing.
2. **Inlining-budget collapse:** the bloated IR pushes `allocateItem()` /
   `deallocateItem()` over the compiler's inline threshold so tiny helpers like
   `tryAllocateFromExistingLocked()` stop inlining.
3. **Template code bloat:** `Slab<ElemSize, Alignment, Lock>` is instantiated
   dozens of times (3 alignment ladders × ~16 size classes × lock policies).
   Calling non-template `[[noreturn]] SPALLOCATOR_COLD_NOINLINE` helpers shares
   one cold throw implementation across every `Slab` instantiation and reduces
   each error check in the hot function to a single conditional branch to a cold
   tail call.

**Why `SPALLOCATOR_NOINLINE` is distinct from `SPALLOCATOR_COLD_NOINLINE`:**
Unhinted frees (`Pool` with `align ≤ 8` or direct `Slab::deallocateItem(ptr)`)
are *not* error paths — for an `align=8` workload they execute on every single
free. Marking `deallocateUnhintedLocked()` as `[[gnu::cold]]` would compile its
contiguous binary search (`findBlockForSlotLocked()`) at `-Os` in
`.text.unlikely` and discourage inlining inside it. Using plain
`SPALLOCATOR_NOINLINE` gets the best of both worlds:
- The hinted `align ≥ 16` path in `Slab::deallocateItem()` stays small (~190 B
  on x86-64 Clang `-O3`) with zero callee-saved spills and tail-calls
  `deallocateUnhintedLocked()` when routing hints are absent.
- `deallocateUnhintedLocked()` itself still compiles at full `-O3` in `.text`
  and inlines `findBlockForSlotLocked()`.

**Data-cache counterpart (hot-first `Slab` layout):**
Just as the macros separate hot and cold *instructions* into different cache
lines, `Slab`'s member layout places the hot per-op fields (`blocks_`,
`block_available_map_`, next-fit cursor, and `slab_lock`) in the leading two
cache lines and moves growth/trim/unhinted-lookup metadata (`live_block_count_`,
`address_index_`, `trimmed_blocks_`, `next_block_generation_`,
`address_index_sorted_`) behind them (reducing `sizeof(Slab)` by 24 B across all
lock policies).

### Scalability levers

| Lever | Effect |
|-------|--------|
| Lock-free `Pool::resolveSlab` + per-`Slab` locks | Different size classes proceed in parallel with zero pool-wide lock |
| Out-of-lock `allocateSlow` / `reclaimEmptyBlocks` | OS heap / `mmap` / page-fault latency never holds `slab_lock` |
| O(1) header-hinted free (`align ≥ 16`) | Shorter `slab_lock` hold time on multi-Block frees (no address-index search) |
| Lazy-sorted flat `address_index_` (`align ≤ 8` / unhinted) | Contiguous 16 B/entry binary search replaces per-Block `std::map` nodes; faster unhinted bulk frees and cleaner glibc coalescing on `~Pool()` |
| Hot/cold codegen split (`SPALLOCATOR_*INLINE`) & hot-first `Slab` layout | Keeps `allocateItem` / `deallocateItem` compact in I-cache with minimal register spills and hot metadata in the first two D-cache lines |
| Multiple pools | Isolate shards / tenants; reduce cross-talk and lock sharing |
| Compile-time `Lock` policy (`NullLock` / `std::mutex` / `SpinLock`) | Eliminates atomic TAS cost completely for single-thread domains |
| Eager Slabs + selective first-Block | Lock-free resolve on all alignment classes; cold RSS only for configured warm classes |
| `trim` | Synchronously reclaims empty `Block` buffers after spikes without a background thread |
| `SlabProxy` | Avoids forcing huge objects through size-class bookkeeping |

Measure before moving to an intrusive freelist — `SlotFreemap` + next-fit CTZ
remains deliberate simplicity until profiles disagree.

### P4 re-baseline (measure-gated hot path)

Re-measured on `PR-CMM-alignment` tip after SpinLock N1a, `SlotFreemap` (N2b),
and P2 (`22f7439` era). Prefer `make bench BUILD_TYPE=release` and
`make profile` with the on-demand harness; absolute ns are host-specific.

**Release ratios (illustrative single run, default align, iters 300k):**

| Scenario | `::new` | `Pool` | `Slab<16,16>` |
|----------|---------|--------|---------------|
| LIFO churn (16B pair) | 1.0× (24.5 ns) | ~3.0× | ~1.8× |
| Multi-size mix | 1.0× | ~2.4× | n/a |
| Bulk alloc / free | 1.0× / 1.0× | ~0.84× / ~4.8× | ~0.45× / ~4.0× |

**`perf` (LIFO `pool`):** top symbols are `Slab::allocateItem` /
`deallocateItem` and `Pool::allocate` / `try_deallocate`. No standalone
freemap/CTZ, `std::map`, virtual thunk, or `SpinLock::lock_contended` hotspot.
Annotate shows **uncontended** `SpinLock` TAS (`xchg`) as a large share of
samples *inside* the slab methods — lock cost without contention.

**Go / no-go (classic P4 candidates):**

| Candidate | Decision |
|-----------|----------|
| `ISlab` de-virtualization | **Deferred** — no measurable vtable/thunk hotspot |
| Intrusive freelist | **Deferred** — scan/freemap not dominating post-`SlotFreemap` |
| Denser free address map | **Deferred** — not a clear top-level hotspot; single-buffer free fast path remains |
| Pluggable `SpinLock` vs `std::mutex` | **Deferred** — cost is uncontended TAS, not contention; mutex unlikely to help |

No hot-path code shipped in P4. Optional future probe (separate from this list):
reduce per-op lock acquisitions if the TAS tax remains the primary gap vs
`::new` after further workload-shaped profiles.

### Comparative baselines (`make bench`)

On-demand harness: `bench/bench_allocators.cpp` via `make bench` (not part of
`make test` / default `all`). Prefer `BUILD_TYPE=release`. The `bench` and
`profile` make goals force `ENABLE_COVERAGE=no` so gcov arcs do not inflate
ns/op.

```bash
make bench BUILD_TYPE=release
make bench BUILD_TYPE=release BENCH_ARGS='--threads=4'
make bench BUILD_TYPE=release BENCH_ARGS='--backend=new,locks'  # SpinLock vs std::mutex vs NullLock
make bench BUILD_TYPE=release BENCH_ARGS='--align=32'   # Pool vs ::new alignment-class-32 spot
make bench BUILD_TYPE=release BENCH_ARGS='--align=64'   # Pool vs ::new alignment-class-64 spot
make bench BUILD_TYPE=release BENCH_ARGS='--scenario=lifo --backend=pool'
make bench BUILD_TYPE=release \
  BENCH_ARGS='--scenario=chase --backend=new,pool --align=64'   # WS pointer-chase cliffs
```

Compares `::operator new`, `spallocator::Pool`, `spallocator::PoolMemoryResource`
(`--backend=pool_pmr`; virtual dispatch over Pool),
`std::pmr::unsynchronized_pool_resource`,
`std::pmr::synchronized_pool_resource` (multi-thread), and bare `Slab<16,16>`
(16B churn). Prints **ns/op** and **ratio vs `::new`** for warmed LIFO churn,
randomized multi-size mix (uniform user sizes in `[16, 1024]`), alloc-then-free-all,
and optional multi-thread multi-size churn (`--threads=N`). Size picks use
per-thread `std::minstd_rand` seeded from a fixed key plus worker index
(repeatable). Pass `--pool-stats` to dump `PoolStats` / slab retained bytes
after scenarios (off by default). Absolute ns are host/toolchain-specific;
re-run after compiler or stdlib upgrades and compare ratios on the same machine.

**Pointer-chase locality sweep (`--scenario=chase`)** — bench-only, not part of
`--scenario=all`. Dependent loads (`p = p->next`) over rings while sweeping
working-set sizes. Default WS list comes from sysfs cache sizes when readable,
else fixed presets; override with `--ws-bytes=`. Modes: **calibrate** (one
contiguous `new[]` buffer), **pool** spatial/random, **`::new`** spatial/random.
Useful flags: `--stride=64`, `--link=spatial|random|all`, `--chase-loads=N`,
`--align=16|64`. Read ns/load plateaus as L1/L2/L3/DRAM cliffs; knees mark
capacity.

Filters (for focused timing or profiling):

| Flag | Values | Notes |
|------|--------|-------|
| `--scenario=` | `lifo`, `multi`, `bulk`, `threaded`, `chase`, `all` (comma-ok) | Default `all` (**excludes** `chase`). `chase` = WS pointer-chase locality sweep |
| `--ws-bytes=` | e.g. `4K,32K,256K,2M,32M` | Chase WS list (default: sysfs / presets) |
| `--stride=` | bytes (default **64**) | Chase node stride; multiple of pointer size |
| `--link=` | `spatial`, `random`, `all` | Chase ring link mode (default `all`) |
| `--chase-loads=` | N | Dependent loads per cell (default scales with `--iters`) |
| `--backend=` | `new`, `pool`, `pmr_unsync`, `pmr_sync`, `slab16`, `pool_pmr`, `all`, `pool_spin`, `pool_mutex`, `pool_null`, `slab16_spin`, `slab16_mutex`, `slab16_null`, `locks` | Inapplicable combos are skipped with a note (including `pool`/`pool_pmr` when built with `LOCK_POLICY=null` and `pool_null` in `threaded`); without `new`, ratios show `n/a`. `locks` expands to all six per-lock backends for side-by-side comparison. `slab16*` participates in `lifo` and `bulk`. `pool_pmr` shows PMR virtual tax vs raw `pool` |
| `--pool-stats` | (flag) | Opt-in `[PoolStats]` / slab retained dumps after scenarios |
| `--batch=` | Per-thread live batch for `threaded` (default **1024**) | Peak live ≈ `threads×batch`; sized for multi-buffer growth under `--size-mix=class` |
| `--size-mix=` | `uniform` or `class` (default **class**) | `class` = equal weight per reachable alignment-class-16 size class (exact-fit user size); better multi-slab coverage than uniform bytes |

Default alignment is `kDefaultAlignment` (typically 16; full comparative suite).
`--align=8` remains available for an 8-byte-header comparison; `--align=32`
and `--align=64` are Pool vs `::new` LIFO spot-checks for high-align
alignment-class/header tax (not a full alignment matrix).

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
make profile BENCH_ARGS='--scenario=lifo --backend=pool --align=32 --iters=2000000'
# Full interactive report / GUI:
#   perf report -i obj/profile/perf.data
#   hotspot obj/profile/perf.data
```

Pin one scenario and one backend so samples concentrate on the path under study.
Compare Slab-only vs Pool to separate engine cost from Pool routing/header work.
Do not treat sanitizer or coverage builds as profile baselines.

### Header-path perf habits

Practices for measuring and evolving the current front-header alloc/free path:

| Habit | Practice |
|-------|----------|
| Fair ratios | `make bench` / `make profile` only — both force `ENABLE_COVERAGE=no`. Prefer `BUILD_TYPE=release` for ns/op. |
| Split alloc vs free | Profile `lifo` (paired) and `bulk` (alloc-then-free-all) separately; compare `--backend=slab16` vs `pool` to isolate header write/validate from freemap. Use `make profile` with one scenario + one backend. |
| Locality | `--scenario=chase` (opt-in) for payload working-set cliffs; not a substitute for LIFO latency. |
| Lock scope | Free holds **no** pool-wide lock across slab work — only the per-`Slab` `SpinLock` inside `deallocateItem`. Keep it that way unless a profile forces a different design. |
| Fail closed | Validate header geometry (size / magic / footprint / base alignment) **before** freemap flips; peek only the fixed bytes behind the user pointer. |
| Integrity tax | Headers today use magic + sizes (no CRC). Add optional CRC / `-march=native` only if a build gate and profiles show the cost is worth it. |

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
| Alloc bit scan | Next-fit + CTZ (`SlotFreemap` + `DynBitset::find_next_set`, production); freelist deferred |
| Free routing / address index | **O(1)** indexed routing via header hints (`align ≥ 16`); **O(1)** single-Block / **O(log m)** lazy-sorted flat `address_index_` binary search for `align ≤ 8` and direct unhinted `Slab` frees |
| Trimmed Block shells | `trim()` frees ≥8 KiB backing buffers outside `slab_lock` and synchronously pops trailing trimmed `Block` shells (with 4× hysteresis `shrink_to_fit`, never during `deallocateItem`); interior trimmed shells below a live higher-index Block stay at ~24–80 B for O(1) refill until the tail empties |
| Fixed size-class ladders | Deliberate; exact-pack buffers within classes; non-ladder shaping is backlog R2 |
| Lock type | **Done** — compile-time selectable (`mlvlib::SpinLock` default, `std::mutex`, or zero-overhead `mlvlib::NullLock` for single-thread domains) via `--with-lock-policy=` / `LOCK_POLICY=` or `Pool(std::in_place_type<Lock>)` |
| `ISlab` virtuals | Deferred de-virt (P4: no measurable thunk hotspot) |
| MSan in CI | Deferred pending host instrumented runtime |
| Metrics API | `PoolStats` / `Pool::stats()` (HWMs always; alloc/free tallies opt-in via `SPALLOCATOR_POOL_COUNTERS`); richer per-class breakdown later |
| C API | C++ only today; C99 wrapper is backlog R4 |

These are engineering follow-ons, not soft-pedaled “educational” gaps. Pool
lifetime vs clients is covered under [Pool lifetime](#pool-lifetime-controlblock--poolhandle)
and [API.md](API.md); remaining rows are scale/API surface, not silent UB.

---

## Future work

### CMM alignment status

Roy / C++ Memory Management alignment pass on `PR-CMM-alignment`:

| Phase | Status |
|-------|--------|
| P0 docs (when-to-use / Roy mapping) | **Done** |
| P1 baselines (`make bench` / `make profile`) | **Done** |
| P2 `PoolMemoryResource` | **Done** |
| P3 size-based / bump arena peer | **Deferred** (see backlog) |
| P4 measure-gated hot path | **Done** (measure-only; candidates deferred) |
| P5 modern safety hygiene | **Done** (docs); language contracts toolchain-deferred |

### Near-term / opportunistic

- Per-class breakdown on top of `PoolStats` (counters/HWMs already shipped)  
- Intrusive freelist — **deferred** (P4 re-baseline; revive if scan dominates)  
- Denser free address lookup — **deferred** (P4 re-baseline; revive if map hot at large `m`)  
- `ISlab` de-virt — **deferred** (P4 re-baseline)  
- Thread-local caches; NUMA-aware buffers  
- Lock-free fast paths / fewer per-op lock acquisitions if TAS tax remains primary  
- Configurable SpinLock backoff parameters  
- **C++26 language contracts** (`pre` / `post` / `contract_assert` on allocate/deallocate) — **deferred**: `-std=c++26` may already be selected while contract syntax is still unsupported on current clang; keep prose + runtime checks until a clean probe compiles under default `./configure`  
- Destroying delete — **documented non-goal** for the generic pool; possible future type-local facade only  

### Research / design backlog

Also tracked on the
[research-design-backlog canvas](/home/michaelv/.cursor/projects/home-michaelv-dev-projects-slab-pool-allocator/canvases/research-design-backlog.canvas.tsx)
(open beside chat):

| Item | Notes |
|------|--------|
| Non–fixed-class buffer shaping | Drop ladders entirely; buffers already exact-pack within classes (`kSlabTargetBytes` / multiplier) |
| Pool lifetime vs smart pointers | **Done** — ControlBlock / Handle / BlockToken; orderly + arena + heap retire |
| Bump / size-based arena facility | **Deferred** — true bump (cursor, empty individual free) does not map onto slab recycling; do not ship as a Pool façade or dual-mode `Slab`. Closest shipped path: arena `~Pool` bulk reclaim. If revived: peer domain to `Pool`, private chunk list or extracted buffer-ownership substrate (not freemap/headers). Unlock when allocate-forward / free-never needs exceed what arena `~Pool` covers. See [CONCEPTS - Choosing a memory strategy](CONCEPTS.md#choosing-a-memory-strategy). |
| PMR `memory_resource` adapter | **Done** — `PoolMemoryResource` in `pool_pmr.hpp` (additive; virtual cost); keep `PoolAllocator` as the primary / non-virtual STL path |
| Baseline ratio benches | **Shipped** — `make bench` (`bench/bench_allocators.cpp`); re-check after toolchain upgrades |
| C99 wrapper | Opaque pool handle API for C callers |
| Pluggable lock primitive | **Done** — `Slab<ElemSize, Alignment, Lock = DefaultSlabLock>` + `mlvlib::lock_traits` / `NullLock` + `Pool(std::in_place_type<Lock>)` + `--with-lock-policy=spinlock\|mutex\|null` |
| Fail-closed allocate policy | Optional: refuse OS / `new` spill when over caps or domain exhausted (timing-critical / bounded domains); loud `bad_alloc` instead of silent fallback |
| C++26 language contracts | **Deferred** — wait for toolchain contract syntax under default `-std=c++26` |
| Destroying delete (typed) | **Non-goal** for generic `Pool` / `PoolDeleter`; future type-local path only |

---

## Demos & tests

| Artifact | Proves |
|----------|--------|
| `test/tester.cpp` | Slab growth/trim/`DynBitset` scaling, pool routing/alignment/header validation, multi-thread pool use, smart-pointer exception safety, PoolHandle/BlockToken lifetime, SpinLock contention, LifetimeObserver (including concurrent `isAlive`) |
| `demos/demo_spalloc_shared_ptr.cpp` | Pool as domain + `make_pool_shared` + `PoolAllocator` with STL containers |
| `demos/demo_pool_pmr.cpp` | `PoolMemoryResource` + `pmr::vector`, vs `PoolAllocator`, in-demo tracing decorator |
| mlvlib `demos/demo_lifetime_observer.cpp` | Single-strand vs mutex-guarded cross-thread observer patterns |

Prefer `make test` under `addrsan` for day-to-day; add `threadsan` when touching
locks or observer atomics. Commands:
[README — Quick Start](../README.md#quick-start).
