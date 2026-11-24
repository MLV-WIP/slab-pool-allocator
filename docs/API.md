# API Reference

Public surface of the library: what each type and function does, parameters,
return values, failure modes, thread-safety, and usage examples.

Architecture and trade-offs live in [IMPLEMENTATION.md](IMPLEMENTATION.md).
Conceptual / learning material lives in [CONCEPTS.md](CONCEPTS.md). Locked
vocabulary (Pool → Slab → **Block** → Slot) lives in
[terminology-glossary.md](terminology-glossary.md). Build instructions stay in
the [root README](../README.md).

Unless noted, types are in namespace `spallocator`. General locking and
liveness utilities live in the [mlvlib](https://github.com/MLV-WIP/mlvlib)
submodule (`mlvlib::SpinLock`, `mlvlib::LifetimeObserver`, helpers). This
package’s `impl/helper.hpp` re-exports those helpers into `spallocator` for
convenience and defines allocator-only size caps.

---

## Table of contents

- [Headers & namespace](#headers--namespace)
- [Pool](#pool)
  - [Constants](#constants)
  - [Construction](#construction)
  - [`PoolHandle`](#poolhandle)
  - [`PoolLifetimeToken`](#poollifetimetoken)
  - [`allocate`](#allocate)
  - [`deallocate` / `try_deallocate`](#deallocate--try_deallocate)
  - [`trim`](#trim)
  - [`getAllocatedMemory`](#getallocatedmemory)
  - [`stats` / `PoolStats`](#stats--poolstats)
  - [`selectClassIndex`](#selectclassindex)
  - [Lifetime models](#lifetime-models)
- [Slab & SlabProxy](#slab--slabproxy)
  - [`ISlab`](#islab)
  - [`Slab`](#slab)
  - [`SlabProxy`](#slabproxy)
  - [`DynBitset`](#dynbitset)
- [Smart pointers](#smart-pointers)
  - [`unique_pool_ptr` / `PoolDeleter`](#unique_pool_ptr--pooldeleter)
  - [`make_pool_unique`](#make_pool_unique)
  - [`make_pool_shared`](#make_pool_shared)
- [PoolAllocator](#poolallocator)
- [PoolMemoryResource](#poolmemoryresource)
- [Helpers & mlvlib](#helpers--mlvlib)
- [Contracts cheat-sheet](#contracts-cheat-sheet)

---

## Headers & namespace

| Header | Primary contents |
|--------|------------------|
| `spallocator/pool.hpp` | `Pool`, ladders (includes `pool_lifetime.hpp`) |
| `spallocator/pool_lifetime.hpp` | `PoolHandle`, `PoolLifetimeToken`, `PoolControlBlock` |
| `spallocator/impl/islab.hpp` | `ISlab` |
| `spallocator/impl/slab.hpp` | `Slab`, `SlotFreemap`, `DynBitset` |
| `spallocator/impl/slab_proxy.hpp` | `SlabProxy` |
| `spallocator/spallocator.hpp` | `make_pool_unique` / `make_pool_shared`, `PoolAllocator`, deleters |
| `spallocator/pool_pmr.hpp` | `PoolMemoryResource` (`std::pmr::memory_resource`; additive — not pulled by `spallocator.hpp`) |
| `spallocator/impl/helper.hpp` | Pool size caps; re-exports mlvlib helpers into `spallocator` |
| `mlvlib/spinlock.hpp` | `mlvlib::SpinLock` (submodule; used by Pool/Slab) |
| `mlvlib/lifetimeobserver.hpp` | `mlvlib::LifetimeObserver` (submodule; optional) |

```
                    ┌─────────────────────────────────────┐
                    │  Pool  (lifetime / routing domain)  │
                    └──────────────┬──────────────────────┘
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
     Slab<…> (size-class engine)          SlabProxy (large new[])
              │
              └── Block (mapped chunk) → Slot (cell)
  Smart pointers / PoolAllocator / PoolMemoryResource ──► PoolLifetimeToken / PoolHandle
  mlvlib::SpinLock / helpers       (submodule; SpinLock used by Pool/Slab)
```

**Stack:** Pool → Slab → Block → Slot. **Item** (`allocateItem` / `deallocateItem`) is the backend allocation handle at the slab layer (a **slot base** for `Slab`). For size-class the Pool’s **user pointer** is the slot base; for `SlabProxy` it is `base + header_size`.

`Pool` and `Slab` are separate concepts: the Pool is the disposable / isolatable
allocation domain you construct and pass around; each Slab is a size-class
engine (Slots in zero or more **Blocks**) the Pool routes into. See
[CONCEPTS.md](CONCEPTS.md) and [terminology-glossary.md](terminology-glossary.md).

Typical includes for pool allocation plus factories:

```cpp
#include "spallocator/pool.hpp"
#include "spallocator/spallocator.hpp"
```

Add `spallocator/impl/helper.hpp` as needed. For SpinLock / LifetimeObserver
directly, include the mlvlib headers (see [mlvlib API](../mlvlib/docs/API.md)).

---

## Pool

**Header:** `spallocator/pool.hpp`

`Pool` is the main entry point for service code: a **named allocation domain**
with a clear lifetime. Construct a pool for a component, shard, or phase of
work; allocate from it; destroy the pool to dispose of that domain’s slab
backends as a unit. Multiple pools isolate resources and can reduce contention
by partitioning traffic — that is the pool’s job, independent of how each size
class stores bytes.

Operationally it is a thread-safe facade: you request a byte count (and optional
alignment); the pool chooses a backend (`Slab` size class or `SlabProxy`),
writes size-class end-tag or proxy front-header metadata, and returns a pointer
to the user payload.

**Ownership rules**

- Each successful `allocate` returns a pointer you exclusively own until a
  matching free (`deallocate` / `try_deallocate`, or a smart-pointer deleter).
- Do **not** pass pointers from another pool (or the system heap) to free, free
  the same pointer twice, or write into the size-class end tag / proxy header.
- The pool is non-copyable and non-movable (it owns slab registries and locks).
- Prefer **orderly** teardown: destroy smart pointers / containers first, then
  the pool (normal `~T()` + return slots). **Arena** teardown (destroy pool
  first) is supported for bulk reclaim — `unique_pool_ptr` deleters no-op when
  the pool is already dead; typed `shared_ptr` still runs `~T` (prefer orderly
  for non-trivial `T`). See [Lifetime models](#lifetime-models).

Routing uses **footprint** `N + 8` for size-class (fixed trailer) or
`N + max(16, alignment)` for proxy, not the bare user size. Layout details:
[IMPLEMENTATION.md — Memory layout](IMPLEMENTATION.md#memory-layout).
Why pools exist as a concept: [CONCEPTS.md — Pool](CONCEPTS.md#what-is-a-pool-and-why-use-one).

### Constants

| Name | Meaning |
|------|---------|
| `kMinAlignment` | Smallest allowed alignment (`4`) |
| `kMaxAlignment` | Largest allowed alignment (`64`) |
| `kSlabNativeAlignment` | Native alignment class for request align 4/8/16 (`16`) |
| `kLadder16` / `kLadder32` / `kLadder64` | Slot sizes available on each alignment class |
| `kLadder*Count` | Number of entries in each ladder |
| `kAllocMagic` | Proxy front-header LIVE magic (user − 6); size-class uses `kTagMagicLive` |
| `kSlabIndexNpos` | Sentinel from class selection meaning “use large path” |

```
Alignment class 16 (eager): 16 … 4096 (16 rungs)
Alignment class 32:  32 … 8192 (16 rungs)
Alignment class 64:  64 … 16384 (16 rungs)
```

If the footprint exceeds the maximum class on that alignment class, the pool uses
`SlabProxy` (system `new[]` / `delete[]`, always 64-byte aligned).

Helpers you may see in headers (usually not called directly from apps):

| Function | Role |
|----------|------|
| `nativeAlignmentClass(alignment)` | Maps request align 4/8 → 16; otherwise returns 32 or 64 |
| `alignmentClassMaxElem(alignment_class)` | Max size-class slot on that alignment class |
| `selectClassIndex(alloc_size, alignment_class)` | Ladder index or `kSlabIndexNpos` for large |

### Construction

```cpp
Pool();
~Pool();
PoolHandle handle();
PoolLifetimeToken lifetime_token();
std::uint64_t live_allocation_count() const noexcept;
PoolStats stats();
PoolControlBlock* control() const noexcept;  // diagnostics
```

| | |
|--|--|
| **Effects** | Constructs an empty pool with a heap `PoolControlBlock` (`heap_owned_ == false`); all alignment-class Slabs installed; first **Blocks** follow preallocate knobs (default: alignment class ≤16 and ElemSize ≤256). |
| **`~Pool`** | Marks the control block dead, then destroys slabs. Safe with outstanding `PoolLifetimeToken`s (arena observers). |
| **Thread safety** | Construct/destroy from one thread; do not use the pool concurrently with its destructor. |
| **Copy / move** | Deleted. |

```cpp
spallocator::Pool pool;  // ready to allocate
auto h = pool.handle();  // optional handle pin (does not make stack pool heap-owned)
```

### `PoolHandle`

**Header:** `spallocator/pool_lifetime.hpp` (also via `pool.hpp`)

```cpp
class PoolHandle {
public:
    PoolHandle() noexcept;
    PoolHandle(const PoolHandle&) noexcept;
    PoolHandle(PoolHandle&&) noexcept;
    static PoolHandle create();
    Pool* get() const noexcept;
    explicit operator bool() const noexcept;
    PoolControlBlock* control() const noexcept;
    PoolLifetimeToken lifetime_token() const noexcept;
};
```

| | |
|--|--|
| **`create()`** | `new Pool` + mark **heap-owned**; returns a handle (`handle_refs ≥ 1`). When `handle_refs == 0` and `live_allocs == 0`, the runtime may `delete` that pool. |
| **`pool.handle()`** | Shares the existing control block; does **not** set heap-owned (stack / caller-owned pools stay caller-destroyed). |
| **`get()`** | Live `Pool*`, or `nullptr` if empty / pool already dead. |

```cpp
auto h = spallocator::PoolHandle::create();
std::byte* p = h.get()->allocate(64);
h.get()->deallocate(p);
// drop last handle with no live allocs → heap Pool retired
```

### `PoolLifetimeToken`

**Header:** `spallocator/pool_lifetime.hpp` (also via `pool.hpp`)

```cpp
class PoolLifetimeToken {
public:
    Pool* get() const noexcept;
    bool alive() const noexcept;
    explicit operator bool() const noexcept;
    PoolControlBlock* control() const noexcept;
};
```

Deleter/allocator pin: bumps **`block_refs` only** (not `handle_refs`). Obtained
via `pool.lifetime_token()` or `handle.lifetime_token()`. Keeps the control block
readable after arena `~Pool` so free paths can no-op when `!alive`.

### Lifetime models

There are **two primary ways** to use a `Pool`. Pick one deliberately; mixing
them without care (especially arena + `PoolAllocator` containers that still
run element dtors) is where trouble starts. A third, optional pattern —
`PoolHandle::create()` — is for heap domains that should disappear when idle.

| Model | Teardown order | When to use |
|-------|----------------|-------------|
| **Orderly (primary)** | Free / destroy smart pointers **first**, then destroy the pool | Default for service objects, STL containers, anything with a non-trivial destructor or owned resources |
| **Arena bulk** | Destroy the **pool first**; leftover `unique_pool_ptr` / `make_pool_shared` no-op | Request/job scratch: many allocations, one fast reclaim; trivial or “don’t care about `~T()`” payloads |
| **Heap handle** (optional) | `PoolHandle::create()`; pool auto-`delete`s when handles and live allocs are both zero | Long-lived shared domain without a stack `Pool`; pass handles into workers |

Design depth: [IMPLEMENTATION.md — Pool lifetime](IMPLEMENTATION.md#pool-lifetime-controlblock--poolhandle).

#### Orderly mode (recommended default)

The pool **outlives** everything allocated from it. Destructors run, slots
return to freemaps, then `~Pool` tears down slabs. This is ordinary RAII —
same mental model as “`unique_ptr` before the allocator goes away.”

```cpp
void handle_request() {
    spallocator::Pool pool;

    auto widget = spallocator::make_pool_unique<Widget>(pool, /* args */);
    auto buf = spallocator::make_pool_shared<std::byte[]>(pool, 4096);

    spallocator::PoolAllocator<int> alloc(pool);
    std::vector<int, spallocator::PoolAllocator<int>> nums(alloc);
    nums.push_back(42);

    use(widget.get(), buf.get(), nums);
    // widget / buf / nums destroy here → ~T() + return slots
}   // ~pool last → slabs freed; nothing left outstanding
```

Use orderly mode whenever `Widget` (or container elements) must run real
destructors — file handles, locks, nested owning pointers, etc.

#### Arena mode (bulk reclaim)

Destroy the pool while some `unique_pool_ptr` / `make_pool_shared` owners may
still exist. `~Pool` marks the control block dead and frees slab storage in one
shot. When those smart pointers later run `PoolDeleter`, they see `!alive` and
**no-op**: no `~T()`, no `try_deallocate`. That is intentional for APR-style
“throw away the region” speed — and it means **destructors do not run** for
those objects. (`shared_ptr` control blocks stay on the ordinary heap.)

```cpp
void scratch_phase() {
    std::optional<spallocator::unique_pool_ptr<ScratchNode>> leak_into_outer;
    {
        spallocator::Pool arena;

        auto a = spallocator::make_pool_unique<ScratchNode>(arena);
        auto b = spallocator::make_pool_unique<ScratchNode>(arena);
        // … build a large graph in `arena` …

        leak_into_outer = std::move(a);  // still holds a PoolLifetimeToken
        // b destroyed here while arena is alive → normal ~ScratchNode + free
    }   // ~arena: mark dead, free all slabs (including storage for *leak_into_outer)

    leak_into_outer.reset();  // deleter: !alive → skip ~ScratchNode and free
}
```

**Rules of thumb for arena**

- Prefer `unique_pool_ptr` / `make_pool_shared` / raw `allocate`+`deallocate`.
  Both smart-pointer factories use `PoolDeleter` (token outside the slab).
- Do **not** leave `PoolAllocator`-backed containers (or raw
  `std::allocate_shared` with `PoolAllocator`) outstanding across arena
  `~Pool`: element/`allocate_shared` storage is in the pool, and standard
  teardown still runs `~T` / touches a pool-resident control block.
- Do not park durable resources (fds, mutexes, owning pointers to
  non-arena memory) only inside arena-allocated objects you might abandon.
- Warm empty pools that you want to keep: use a stack/`unique_ptr<Pool>` and
  orderly frees — arena is “dispose the whole domain,” not “trim RSS.”

#### Heap handle domain (optional)

When there is no natural stack owner, `PoolHandle::create()` heap-allocates a
pool, marks it **heap-owned**, and retires it automatically when the last handle
is dropped **and** `live_allocation_count() == 0`.

```cpp
spallocator::PoolHandle make_shard() {
    return spallocator::PoolHandle::create();
}

void worker(spallocator::PoolHandle h) {
    auto p = spallocator::make_pool_unique<JobState>(h);
    run(*p);
    p.reset();  // live_allocs → 0; pool stays while `h` (or copies) exist
}

void run_shard() {
    auto h = make_shard();
    worker(h);           // copy bumps handle_refs
    // drop h → handle_refs 0, live_allocs 0 → delete heap Pool
}
```

`pool.handle()` on a stack pool also returns a `PoolHandle`, but does **not**
set heap-owned: dropping those handles never `delete`s the stack object.

### `allocate`

```cpp
std::byte* allocate(std::size_t size,
                    std::size_t alignment = kDefaultAlignment);
```

Reserves `size` bytes of user payload with the requested alignment and returns
a pointer to that payload (past the hidden header).

| Parameter | Description |
|-----------|-------------|
| `size` | Number of bytes the caller will use. Must be `≤ kMaxAllocationBytes` (default 1 GiB). |
| `alignment` | Required alignment of the returned pointer. Default `kDefaultAlignment` (`__STDCPP_DEFAULT_NEW_ALIGNMENT__`, typically **16** — same guarantee as `::operator new`). Must be a power of two in `[kMinAlignment, kMaxAlignment]` (`4`…`64`). |

| | |
|--|--|
| **Returns** | Pointer to `size` usable bytes, aligned to `alignment`. Exclusive ownership until free. |
| **Throws** | `std::out_of_range` if `size > kMaxAllocationBytes`; `std::invalid_argument` if `alignment` is out of range or not a power of two; may also throw from the underlying system allocator (`std::bad_alloc` / `new`). |
| **Thread safety** | Safe for concurrent callers on the same `Pool`. |

```cpp
std::byte* p = pool.allocate(100);        // default align → typically 16-byte header
std::byte* q = pool.allocate(64, 64);     // alignment-class-64 size class
```

### `deallocate` / `try_deallocate`

```cpp
void deallocate(std::byte* item);
bool try_deallocate(std::byte* item) noexcept;
```

Return a pointer previously obtained from this pool (or from a factory that
allocated through it). Both validate the hidden header (magic, header size,
footprint) before releasing the slot.

| Parameter | Description |
|-----------|-------------|
| `item` | User pointer from `allocate` / factory, or `nullptr`. |

| | `deallocate` | `try_deallocate` |
|--|--------------|------------------|
| **`nullptr`** | Treated as success (no-op) via `try_deallocate` path | Returns `true` |
| **Success** | Slot returned to the slab (or `delete[]` on large path) | Returns `true` |
| **Failure** | Throws `std::invalid_argument` (invalid / corrupt header, double-free, non-pool pointer, or slab-layer rejection) | Returns `false` (does not throw) |
| **Thread safety** | Safe | Safe |

Prefer `try_deallocate` from `noexcept` contexts (`PoolDeleter`,
`PoolAllocator::deallocate`). Prefer throwing `deallocate` when the caller can
handle errors as exceptions.

Freeing a Slot does **not** necessarily return Slab **Blocks** to the OS; see
[`trim`](#trim).

```cpp
pool.deallocate(p);
if (!pool.try_deallocate(q)) {
    // programming error or already freed — handle without unwinding
}
```

### `trim`

```cpp
std::size_t trim(std::size_t keep_min = 1);
```

Reclaims **fully empty** size-class **Blocks** so peak RSS can drop after a
burst. Default growth is monotonic: ordinary free reuses Slots but keeps empty
Blocks mapped. (Under the hood each Slab’s `reclaimEmptyBlocks` counts/deletes Blocks.)

| Parameter | Description |
|-----------|-------------|
| `keep_min` | Minimum number of **Blocks** to retain **per size class** after reclaim. Clamped to at least `1` so the next allocation in that class stays warm. |

| | |
|--|--|
| **Returns** | Number of **Blocks** deleted across size classes. |
| **Throws** | Does not throw under normal operation. |
| **Thread safety** | Safe. |
| **Note** | Does not affect in-flight allocations. `SlabProxy` large allocations are already returned on each free. |

```cpp
// After a spike of short-lived allocations:
pool.trim();      // keep ≥1 Block per class
pool.trim(2);     // keep a slightly larger warm set
```

### `getAllocatedMemory`

```cpp
std::size_t getAllocatedMemory();
```

| | |
|--|--|
| **Returns** | Sum of bytes in **retained size-class Blocks** currently held by the pool (not a live “bytes in use by clients” counter, and not counting transient `SlabProxy` allocations). |
| **Thread safety** | Safe. |
| **Use** | Approximate RSS contribution of pooled Slabs; pair with `trim` when investigating retention. |

### `stats` / `PoolStats`

```cpp
struct PoolStats {
    std::uint64_t live_allocations;     // outstanding successful allocs
    std::uint64_t retained_mapped_bytes;  // size-class Block bytes held (same basis as getAllocatedMemory)
    std::uint64_t alloc_ok;
    std::uint64_t alloc_fail;           // size/align/cap/slab failures
    std::uint64_t free_ok;              // successful non-null frees
    std::uint64_t free_fail;            // try_deallocate returned false
    std::uint64_t live_hwm;             // peak live_allocations
    std::uint64_t retained_hwm;         // peak retained_mapped_bytes observed
};

PoolStats stats();
```

| | |
|--|--|
| **Returns** | Best-effort snapshot of counters and high-water marks (HWMs). Not a single transactional freeze under concurrency. |
| **`live_*` vs `retained_*`** | Live = outstanding client allocations. Retained = bytes in size-class **Blocks** still mapped (may stay high after free until `trim`). |
| **`live_hwm`** | Best-effort peak live count: cheap relaxed max on each successful allocate, and refreshed in `stats()`. |
| **`retained_hwm`** | Advanced when `stats()` or `trim()` observes a higher retained total (not on every allocate). |
| **`alloc_ok` / `free_ok` / `*_fail`** | Hot-path tallies; **off by default** (`SPALLOCATOR_POOL_COUNTERS=0`) so allocate/free avoid extra atomic RMWs. Enable with `-DSPALLOCATOR_POOL_COUNTERS=1` or `./configure --enable-pool-counters`. When disabled, these fields read as 0. `kPoolCountersEnabled` reports the build setting. |
| **Thread safety** | Safe; enabled counters use relaxed atomics. |

```cpp
auto s = pool.stats();
if (s.live_hwm > threshold) { /* alert */ }
```

### `selectClassIndex`

```cpp
constexpr std::size_t selectClassIndex(std::size_t alloc_size,
                                       std::size_t alignment_class);
```

Free function (namespace `spallocator`). Maps a **footprint** onto a size-class
**index** for the given **alignment class** (16 / 32 / 64) — not a `Slab*`.
Use `nativeAlignmentClass(alignment)` to map a request alignment onto a class
before calling. Overflow past that ladder’s max returns `kSlabIndexNpos`
(large / `SlabProxy` path).

| Parameter | Description |
|-----------|-------------|
| `alloc_size` | Footprint to classify (user size + header when matching the pool path). |
| `alignment_class` | `16`, `32`, or `64` (from `nativeAlignmentClass` or an explicit ladder). |

| | |
|--|--|
| **Returns** | Ladder index, or `kSlabIndexNpos` for the large-allocation path. |
| **Thread safety** | Pure / constexpr-friendly; no shared mutable state. |

Most applications never call this; tests and tooling use it to assert which
class a footprint would hit. (`selectSlab` was removed.)

---

## Slab & SlabProxy

**Headers:** `spallocator/impl/islab.hpp`, `spallocator/impl/slab.hpp`, `spallocator/impl/slab_proxy.hpp`

A **Slab** is a size-class engine: **Blocks** carved into equal **Slots**, with
bitmaps (and related maps) tracking free vs in-use. That is a storage strategy,
not a lifetime domain — the **Pool** owns which Slabs exist and when they go
away. `SlabProxy` implements the same backend interface for oversized requests
via system `new[]` / `delete[]` (no Block/Slot recycling).

Application code usually stays on `Pool` and the smart-pointer helpers. Use slab
types directly only when embedding a single size class or extending the
allocator. Conceptual background:
[CONCEPTS.md — Slab](CONCEPTS.md#what-is-a-slab-and-why-use-one);
[terminology-glossary.md](terminology-glossary.md).

### `ISlab`

Polymorphic interface shared by size-class Slabs and the large-allocation proxy.

```cpp
using AllocateItemResult = std::tuple<std::byte*, std::size_t, std::uint16_t>;

class ISlab {
public:
    virtual AllocateItemResult allocateItem(std::size_t size) = 0;
    virtual void deallocateItem(std::byte* item) = 0;
    virtual std::size_t reclaimEmptyBlocks(std::size_t keep_min = 1);
    virtual std::size_t getAllocatedMemory() const;
    virtual ~ISlab() = default;
};
```

| Method | Description |
|--------|-------------|
| `allocateItem(size)` | Allocate an **Item** (backend allocation). Returns `{ptr, elem_size, generation}` — for `Slab`, `ptr` is the **slot base**, `elem_size` is `ElemSize`, and `generation` is the Block’s generation (so Pool can write the end tag without a map lookup). For `SlabProxy`, `{base, 0, 0}` (Pool uses a front header; user pointer is `base + header_size`). |
| `deallocateItem(item)` | Free an **Item** previously returned by `allocateItem` (a **slot base** for `Slab`, not a user pointer). Rejects unknown, interior, or already-free addresses (`std::invalid_argument`). |
| `reclaimEmptyBlocks(keep_min)` | Drop fully empty **Blocks**; keep at least `keep_min` (default 1). Default base implementation is a no-op; `Slab` overrides. Returns how many Blocks were deleted. |
| `getAllocatedMemory()` | Bytes retained by this Slab’s **Blocks** (default `0`). |

Copy/move of `ISlab` are deleted.

### `Slab`

```cpp
template<std::size_t ElemSize, std::size_t Alignment = 16>
class Slab : public ISlab;
```

Fixed-size Slot allocator: each Slot is `ElemSize` bytes and `Alignment`-aligned
(`Alignment` must be 16, 32, or 64; `ElemSize` must be a multiple of
`Alignment`). Maintains freemap bitsets, a growable availability map, and a
base-address map for frees. Owns zero or more **Blocks**.

| Method | Description |
|--------|-------------|
| `allocateItem` / `deallocateItem` | See `ISlab`; guarded by the Slab’s `SpinLock`. Slot base in / out — Pool owns the user-pointer offset. |
| `getElemSize()` / `getAlignment()` / `getAllocSize()` | Compile-time Slot size, alignment, and **Block** bytes (`selectBlockSize`; exact multiple of `ElemSize`; see `kSlabTargetBytes` / `kSlabTargetMultiplier`). |
| `getAllocatedMemory()` | `getBlockCount() × getAllocSize()` currently retained (**Blocks** × Block bytes). |
| `getAvailabilityMapBytes()` | Bytes used by the growable availability bitset (diagnostics). |
| `getBlockCount()` | Number of **Blocks** currently held. |
| `reclaimEmptyBlocks(keep_min)` | Deletes empty **Blocks** (swap-with-last), retaining ≥ `keep_min`. |
| `findBlockForSlot(slot_base)` | Optional index of the **Block** owning an exact **slot base** in this Slab. |

Empty Blocks remain mapped after free until reclaim/`Pool::trim` — intentional
monotonic growth for alloc/free latency.

### `SlabProxy`

```cpp
class SlabProxy : public ISlab;
```

Large / overflow path: each `allocateItem` is a distinct aligned `new[]`;
`deallocateItem` always `delete[]`s that pointer. No freemap, no retention
across frees. Returns a **slot base** equivalent (allocation base); the Pool
still attaches its usual allocation header and returns the **user pointer**.

### `DynBitset`

Growable bitset used internally for “does this Block still have free Slots?”
metadata. Not part of the stable application-facing API; listed so readers of
`impl/slab.hpp` know what it is. Prefer `Pool` / `Slab` accessors for diagnostics.

---

## Smart pointers

**Header:** `spallocator/spallocator.hpp`

RAII wrappers so callers rarely manage raw `std::byte*` from `Pool`.

### `unique_pool_ptr` / `PoolDeleter`

```cpp
template<typename T>
using unique_pool_ptr = std::unique_ptr<T, PoolDeleter<T>>;

template<typename T>
struct PoolDeleter {
    PoolLifetimeToken token;
    void operator()(T* p) const noexcept;   // if alive: ~T() then try_deallocate
};

template<typename T>
struct PoolDeleter<T[]> {
    PoolLifetimeToken token;
    const std::size_t size;                 // element count
    void operator()(T* p) const noexcept;   // reverse-order dtors, then free
};
```

| Piece | Description |
|-------|-------------|
| `token` | `PoolLifetimeToken` for the domain (from `Pool&` / `PoolHandle` / token ctors). |
| `size` (array) | Number of elements to destroy; stored in the deleter when the array was created. |
| `operator()` | If `p` is null or `!token.alive()`: no-op (arena). Else: destructor(s), then `try_deallocate`. On free failure while alive, **`std::abort()`**. |

Array destruction order matches stack arrays (last constructed → first
destroyed).

### `make_pool_unique`

```cpp
template<typename T, typename... Args>
    requires (!std::is_array_v<T>)
unique_pool_ptr<T> make_pool_unique(PoolLifetimeToken token, Args&&... args);
// also: Pool&, PoolHandle const&

template<typename T>  // T = U[]
    requires std::is_unbounded_array_v<T>
unique_pool_ptr<T> make_pool_unique(PoolLifetimeToken token, std::size_t size);
// also: Pool&, PoolHandle const&

// Bounded array forms are deleted — use std::array or U[].
```

**Single object**

| Parameter | Description |
|-----------|-------------|
| `token` / `pool` / `handle` | Live domain used for `allocate` / free. |
| `args...` | Forwarded to `T`’s constructor via placement new. |

| | |
|--|--|
| **Returns** | `unique_pool_ptr<T>` owning the constructed object. |
| **Alignment** | Uses `alignof(T)`; `static_assert` that it is `≤ 64`. |
| **Exceptions** | If construction throws, the raw block is returned to the pool, then the exception propagates (**strong guarantee**). Allocation failures throw as from `Pool::allocate`. |

**Unbounded array `T = U[]`**

| Parameter | Description |
|-----------|-------------|
| `token` / `pool` / `handle` | Live domain for the block. |
| `size` | Number of default-constructed `U` elements. |

| | |
|--|--|
| **Returns** | `unique_pool_ptr<U[]>` with deleter holding `size`. |
| **Exceptions** | If the *k*-th element constructor throws, elements `[0, k)` are destroyed in reverse order, the block is freed, then the exception propagates. |

```cpp
auto u = spallocator::make_pool_unique<Widget>(pool, arg1, arg2);
auto a = spallocator::make_pool_unique<int[]>(pool, 100);
auto h = spallocator::PoolHandle::create();
auto u2 = spallocator::make_pool_unique<Widget>(h, arg1);
```

### `make_pool_shared`

```cpp
template<typename T, typename... Args>
    requires (!std::is_array_v<T>)
std::shared_ptr<T> make_pool_shared(PoolLifetimeToken token, Args&&... args);
// also: Pool&, PoolHandle const&

template<typename T>
    requires std::is_unbounded_array_v<T>
std::shared_ptr<T> make_pool_shared(PoolLifetimeToken token, std::size_t size);
// also: Pool&, PoolHandle const&

template<typename T>
    requires std::is_unbounded_array_v<T>
std::shared_ptr<T> make_pool_shared(PoolLifetimeToken token, std::size_t size,
                                    const std::remove_extent_t<T>& init_value);
// also: Pool&, PoolHandle const&
```

Builds a `std::shared_ptr` with object storage from the pool and a
**heap-allocated control block**. Teardown uses `PoolDeleter` (same arena
no-op model as `make_pool_unique`). Tradeoff vs `allocate_shared`: two
allocations (pool object + heap CB).

| Overload | Parameters | Behavior |
|----------|------------|----------|
| Single object | domain, ctor `args...` | placement `new` + `shared_ptr(p, PoolDeleter)` |
| Array | domain, `size` | `size` default-initialized elements; count in `PoolDeleter` |
| Array + value | domain, `size`, `init_value` | `size` elements copy-initialized from `init_value` |

Bounded array overloads are deleted. Array size lives in `PoolDeleter` (same
as `make_pool_unique`). Safe to leave outstanding across arena `~Pool` when
you accept skipped destructors.

```cpp
auto s = spallocator::make_pool_shared<Widget>(pool, arg1);
auto arr = spallocator::make_pool_shared<int[]>(pool, 100);
auto filled = spallocator::make_pool_shared<int[]>(pool, 100, 42);
std::weak_ptr<Widget> w = s;
```

---

## PoolAllocator

**Header:** `spallocator/spallocator.hpp`

Standard-library-style allocator that obtains storage from a pool domain.
Enables `std::vector` and other allocator-aware facilities. Prefer
`make_pool_shared` over `std::allocate_shared` with this allocator if the
pool may die first (allocate_shared places the control block in the pool).

```cpp
template<typename T>
class PoolAllocator {
public:
    using value_type = T;
    // POCCA / POCMA / POCS = false_type; is_always_equal = false_type

    explicit PoolAllocator(PoolLifetimeToken token) noexcept;
    explicit PoolAllocator(Pool& pool) noexcept;
    explicit PoolAllocator(PoolHandle const& handle) noexcept;
    PoolAllocator(const PoolAllocator&) noexcept = default;
    template<typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept;  // rebind

    [[nodiscard]] T* allocate(std::size_t n);
    void deallocate(T* p, std::size_t n) noexcept;
};
```

| Member / parameter | Description |
|--------------------|-------------|
| Constructor | Takes `PoolLifetimeToken`, `Pool&`, or `PoolHandle` (token = `block_refs` pin). |
| Rebind ctor | Shares the same control block when containers rebind to node types. |
| `allocate(n)` | Space for `n` × `T` at `alignof(T)`. Throws `bad_array_new_length` on overflow; **`bad_alloc`** if the pool is already dead; otherwise same as `Pool::allocate`. |
| `deallocate(p, n)` | Returns storage for `p`. `n` ignored. `noexcept`; bad pointers **abort** while alive; **no-op** if `!alive` (arena). |
| Equality | True iff both allocators share the same `PoolControlBlock*`. |

Propagation traits are false because the domain is external and must not
silently move to another container’s allocator state.

```cpp
spallocator::Pool pool;
spallocator::PoolAllocator<int> alloc(pool);
std::vector<int, spallocator::PoolAllocator<int>> v(alloc);
v.push_back(42);
```

---

## PoolMemoryResource

**Header:** `spallocator/pool_pmr.hpp` (include explicitly; not part of
`spallocator.hpp`, so non-PMR clients avoid `<memory_resource>`).

`std::pmr::memory_resource` backed by a pool domain. Prefer
`PoolAllocator<T>` when the container type can bake the allocator in (no
virtual dispatch). Use this for polymorphic `std::pmr` containers and runtime
composition (for example a tracing decorator over the resource).

Same arena model as `PoolAllocator`: holds a `PoolLifetimeToken`; allocate throws
`bad_alloc` if `!alive`; deallocate no-ops if dead.

```cpp
class PoolMemoryResource : public std::pmr::memory_resource {
public:
    explicit PoolMemoryResource(PoolLifetimeToken token) noexcept;
    explicit PoolMemoryResource(Pool& pool) noexcept;
    explicit PoolMemoryResource(PoolHandle const& handle) noexcept;

    PoolLifetimeToken const& token() const noexcept;
};
```

| Surface | Behavior |
|---------|----------|
| Constructors | `PoolLifetimeToken`, `Pool&`, or `PoolHandle` (token = `block_refs` pin). |
| `allocate(bytes, alignment)` | Alignment below `kMinAlignment` (4) is raised to 4. Non-power-of-two or `> kMaxAlignment` (64) throws `bad_alloc`. Dead pool throws `bad_alloc`. Pool contract failures (`invalid_argument` / `out_of_range`) are mapped to `bad_alloc` (PMR contract). |
| `deallocate(p, bytes, alignment)` | `noexcept`. Null is a no-op. Bad pointers **abort** while alive; **no-op** if `!alive` (arena). `bytes` / `alignment` are unused (pool frees by header). |
| `is_equal` | True iff `other` is a `PoolMemoryResource` on the same `PoolControlBlock*`. |

```cpp
#include <memory_resource>
#include "spallocator/pool_pmr.hpp"

spallocator::Pool pool;
spallocator::PoolMemoryResource resource(pool);
std::pmr::vector<int> v{&resource};
v.push_back(42);
```

Demo: `demos/demo_pool_pmr.cpp` (also shows an in-demo tracing decorator).
Bench token: `--backend=pool_pmr` on `make bench`.

---

## Helpers & mlvlib

**Allocator helper header:** `spallocator/impl/helper.hpp`

- Re-exports `mlvlib` helpers into `spallocator` (`print` / `println`,
  `debug_print*`, `runtime_assert`, `_KB` / `_MB` / `_GB` literals).
- Defines compile-time pool caps: `kMaxAllocationBytes`, `kMaxClassBytes`
  (override via `./configure --with-max-*-mib` / `-DSPALLOCATOR_MAX_*`).

**External utilities (submodule):** see [mlvlib](https://github.com/MLV-WIP/mlvlib)
and local docs under `mlvlib/docs/` after `git submodule update --init`.

| Utility | Header | Notes |
|---------|--------|-------|
| `mlvlib::SpinLock` | `mlvlib/spinlock.hpp` | Used by Pool/Slab; deep dive in `mlvlib/docs/deepdive/SpinLock_Analysis.md` |
| `mlvlib::LifetimeObserver` | `mlvlib/lifetimeobserver.hpp` | Optional; not required by the allocator |
| Helpers | `mlvlib/helper.hpp` | Canonical home; re-exported via `spallocator/impl/helper.hpp` |

Build requires `-Imlvlib` (Makefile sets this) so `#include "mlvlib/..."` resolves
against the submodule root.

---

## Contracts cheat-sheet

| Surface | On failure | Threads |
|---------|------------|---------|
| `Pool::allocate` | throws | Concurrent allocate/free on one pool is supported |
| `Pool::deallocate` | throws | Same |
| `Pool::try_deallocate` | `false` | Same |
| `Pool::trim` / `getAllocatedMemory` | — | Safe |
| `PoolDeleter` / `PoolAllocator::deallocate` | `abort` on bad pointer if alive; no-op if dead | Pool-safe; pointer handoff is the caller’s protocol |
| `PoolMemoryResource::allocate` | `bad_alloc` (dead / unsupported align / mapped Pool throws) | Same as Pool |
| `PoolMemoryResource::deallocate` | `abort` on bad pointer if alive; no-op if dead | Same as PoolAllocator |
| `PoolHandle::create` / handle drop | may `delete` heap pool when idle | Destroy/create from one thread |
| `make_pool_unique` construction | throw; slot returned | Pool-safe |
| `LifetimeObserver::isAlive` | — | Race-free liveness bit |
| Object members after `isAlive` | — | Complete on one strand; cross-thread needs shared sync with destroy |
| `SpinLock::lock` / `unlock` | — | Mutual exclusion + TSan annotations when built with TSan |
| `runtime_assert` | abort (debug only) | N/A |

**Prose contracts today:** alignment must be a power of two in
`[kMinAlignment, kMaxAlignment]`; size caps throw; bad frees throw or abort
on noexcept paths. These remain documentation + runtime checks.

**C++26 language contracts:** intentional follow-on for `pre` / `post` /
`contract_assert` on allocate/deallocate when the toolchain actually accepts
contract syntax. `./configure` may already select `-std=c++26` while contract
keywords are still unsupported — do not gate the library on them yet. See
[IMPLEMENTATION.md — Future work](IMPLEMENTATION.md#future-work).

See also: [IMPLEMENTATION.md](IMPLEMENTATION.md) for header layout and lock
ordering; [CONCEPTS.md](CONCEPTS.md) for when to choose each tool.
