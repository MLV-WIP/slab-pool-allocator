# API Reference

Public surface of the library: what each type and function does, parameters,
return values, failure modes, thread-safety, and usage examples.

Architecture and trade-offs live in [IMPLEMENTATION.md](IMPLEMENTATION.md).
Conceptual / learning material lives in [CONCEPTS.md](CONCEPTS.md). Build
instructions stay in the [root README](../README.md).

Unless noted, types are in namespace `spallocator`. `SpinLock` and
`LifetimeObserver` are declared as global classes in their headers today.

---

## Table of contents

- [Headers & namespace](#headers--namespace)
- [Pool](#pool)
  - [Constants](#constants)
  - [Construction](#construction)
  - [`PoolHandle`](#poolhandle)
  - [`PoolBlockToken`](#poolblocktoken)
  - [`allocate`](#allocate)
  - [`deallocate` / `try_deallocate`](#deallocate--try_deallocate)
  - [`trim`](#trim)
  - [`getAllocatedMemory`](#getallocatedmemory)
  - [`selectSlab`](#selectslab)
  - [Lifetime models](#lifetime-models)
- [Slab & SlabProxy](#slab--slabproxy)
  - [`AbstractSlab`](#abstractslab)
  - [`Slab`](#slab)
  - [`SlabProxy`](#slabproxy)
  - [`DynBitset`](#dynbitset)
- [Smart pointers](#smart-pointers)
  - [`unique_pool_ptr` / `PoolDeleter`](#unique_pool_ptr--pooldeleter)
  - [`make_pool_unique`](#make_pool_unique)
  - [`make_pool_shared`](#make_pool_shared)
- [PoolAllocator](#poolallocator)
- [LifetimeObserver](#lifetimeobserver)
- [SpinLock](#spinlock)
- [Helpers](#helpers)
  - [Size literals](#size-literals)
  - [Debug / verbose print](#debug--verbose-print)
  - [`runtime_assert`](#runtime_assert)
- [Contracts cheat-sheet](#contracts-cheat-sheet)

---

## Headers & namespace

| Header | Primary contents |
|--------|------------------|
| `spallocator/pool.hpp` | `Pool`, ladders (includes `pool_lifetime.hpp`) |
| `spallocator/pool_lifetime.hpp` | `PoolHandle`, `PoolBlockToken`, `PoolControlBlock` |
| `spallocator/slab.hpp` | `AbstractSlab`, `Slab`, `SlabProxy`, `DynBitset` |
| `spallocator/spallocator.hpp` | `make_pool_unique` / `make_pool_shared`, `PoolAllocator`, deleters |
| `spallocator/lifetimeobserver.hpp` | `LifetimeObserver` |
| `spallocator/spinlock.hpp` | `SpinLock` |
| `spallocator/helper.hpp` | Size literals, `runtime_assert`, debug print |

```
                    ┌─────────────────────────────────────┐
                    │  Pool  (lifetime / routing domain)  │
                    └──────────────┬──────────────────────┘
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
     Slab<…> (fixed-size slots)          SlabProxy (large new[])
  Smart pointers / PoolAllocator ──► PoolBlockToken / PoolHandle
  LifetimeObserver                 (stand-alone utility)
```

`Pool` and `Slab` are separate concepts: the pool is the disposable / isolatable
allocation domain you construct and pass around; each slab is a fixed-size slot
recycler the pool routes into. See [CONCEPTS.md](CONCEPTS.md) for the distinction.

Typical includes for pool allocation plus factories:

```cpp
#include "spallocator/pool.hpp"
#include "spallocator/spallocator.hpp"
```

Add `lifetimeobserver.hpp`, `spinlock.hpp`, or `helper.hpp` as needed.

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
writes a small hidden header, and returns a pointer to the user payload.

**Ownership rules**

- Each successful `allocate` returns a pointer you exclusively own until a
  matching free (`deallocate` / `try_deallocate`, or a smart-pointer deleter).
- Do **not** pass pointers from another pool (or the system heap) to free, free
  the same pointer twice, or write into the hidden bytes *before* the user
  pointer.
- The pool is non-copyable and non-movable (it owns slab registries and locks).
- Prefer **orderly** teardown: destroy smart pointers / containers first, then
  the pool (normal `~T()` + return slots). **Arena** teardown (destroy pool
  first) is supported for bulk reclaim — `unique_pool_ptr` deleters no-op when
  the pool is already dead; typed `shared_ptr` still runs `~T` (prefer orderly
  for non-trivial `T`). See [Lifetime models](#lifetime-models).

Routing uses **footprint** `size + header_size` where
`header_size = max(8, alignment)`, not the bare user size. Layout details:
[IMPLEMENTATION.md — Memory layout](IMPLEMENTATION.md#memory-layout).
Why pools exist as a concept: [CONCEPTS.md — Pool](CONCEPTS.md#what-is-a-pool-and-why-use-one).

### Constants

| Name | Meaning |
|------|---------|
| `kMinAlignment` | Smallest allowed alignment (`4`) |
| `kMaxAlignment` | Largest allowed alignment (`64`) |
| `kSlabNativeAlignment` | Native axis for request align 4/8/16 (`16`) |
| `kLadder16` / `kLadder32` / `kLadder64` | Slot sizes available on each alignment axis |
| `kLadder*Count` | Number of entries in each ladder |
| `kAllocMagic` | Byte written into the allocation header; checked (then cleared) on free |
| `kSlabIndexNpos` | Sentinel from class selection meaning “use large path” |

```
Axis 16 (eager): 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024
Axis 32 (lazy):  32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
Axis 64 (lazy):  64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
```

If the footprint exceeds the maximum class on that axis, the pool uses
`SlabProxy` (system `new[]` / `delete[]`, always 64-byte aligned).

Helpers you may see in headers (usually not called directly from apps):

| Function | Role |
|----------|------|
| `nativeAxis(alignment)` | Maps request align 4/8 → 16; otherwise returns 32 or 64 |
| `axisMaxElem(axis)` | Max size-class slot on that axis |
| `selectClassIndex(alloc_size, axis)` | Ladder index or `kSlabIndexNpos` for large |

### Construction

```cpp
Pool();
~Pool();
PoolHandle handle();
PoolBlockToken block_token();
std::uint64_t live_allocation_count() const noexcept;
PoolControlBlock* control() const noexcept;  // diagnostics
```

| | |
|--|--|
| **Effects** | Constructs an empty pool with a heap `PoolControlBlock` (`heap_owned_ == false`); axis-16 slabs prepared; axis 32/64 lazy. |
| **`~Pool`** | Marks the control block dead, then destroys slabs. Safe with outstanding `PoolBlockToken`s (arena observers). |
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
    PoolBlockToken block_token() const noexcept;
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

### `PoolBlockToken`

**Header:** `spallocator/pool_lifetime.hpp` (also via `pool.hpp`)

```cpp
class PoolBlockToken {
public:
    Pool* get() const noexcept;
    bool alive() const noexcept;
    explicit operator bool() const noexcept;
    PoolControlBlock* control() const noexcept;
};
```

Deleter/allocator pin: bumps **`block_refs` only** (not `handle_refs`). Obtained
via `pool.block_token()` or `handle.block_token()`. Keeps the control block
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

        leak_into_outer = std::move(a);  // still holds a PoolBlockToken
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
std::byte* allocate(std::size_t size, std::size_t alignment = 8);
```

Reserves `size` bytes of user payload with the requested alignment and returns
a pointer to that payload (past the hidden header).

| Parameter | Description |
|-----------|-------------|
| `size` | Number of bytes the caller will use. Must be `≤ 1_GB`. |
| `alignment` | Required alignment of the returned pointer. Default `8`. Must be a power of two in `[kMinAlignment, kMaxAlignment]` (`4`…`64`). |

| | |
|--|--|
| **Returns** | Pointer to `size` usable bytes, aligned to `alignment`. Exclusive ownership until free. |
| **Throws** | `std::out_of_range` if `size > 1_GB`; `std::invalid_argument` if `alignment` is out of range or not a power of two; may also throw from the underlying system allocator (`std::bad_alloc` / `new`). |
| **Thread safety** | Safe for concurrent callers on the same `Pool`. |

```cpp
std::byte* p = pool.allocate(100);        // align 8 → typically 8-byte header
std::byte* q = pool.allocate(64, 64);     // axis-64 lazy size class
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

Freeing a slot does **not** necessarily return slab buffers to the OS; see
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

Reclaims **fully empty** size-class slab buffers so peak RSS can drop after a
burst. Default growth is monotonic: ordinary free reuses slots but keeps empty
buffers mapped.

| Parameter | Description |
|-----------|-------------|
| `keep_min` | Minimum number of slab buffers to retain **per size class** after reclaim. Clamped to at least `1` so the next allocation in that class stays warm. |

| | |
|--|--|
| **Returns** | Number of slab buffers deleted across size classes. |
| **Throws** | Does not throw under normal operation. |
| **Thread safety** | Safe. |
| **Note** | Does not affect in-flight allocations. `SlabProxy` large blocks are already returned on each free. |

```cpp
// After a spike of short-lived allocations:
pool.trim();      // keep ≥1 buffer per class
pool.trim(2);     // keep a slightly larger warm set
```

### `getAllocatedMemory`

```cpp
std::size_t getAllocatedMemory();
```

| | |
|--|--|
| **Returns** | Sum of bytes in **retained size-class slab buffers** currently held by the pool (not a live “bytes in use by clients” counter, and not counting transient `SlabProxy` blocks). |
| **Thread safety** | Safe. |
| **Use** | Approximate RSS contribution of pooled slabs; pair with `trim` when investigating retention. |

### `selectSlab`

```cpp
constexpr std::size_t selectSlab(std::size_t size) const;
constexpr std::size_t selectSlab(std::size_t size, std::size_t alignment) const;
```

Maps a **footprint** (already including header, when used like the pool’s
internal path) onto a size-class index for the given axis. The one-argument
overload uses the native axis 16.

| Parameter | Description |
|-----------|-------------|
| `size` | Footprint to classify (for diagnostics / tooling; apps normally call `allocate` instead). |
| `alignment` | Request alignment used to pick axis 16 / 32 / 64. |

| | |
|--|--|
| **Returns** | Ladder index, or a sentinel meaning large-allocation path (`kSlabIndexNpos` in the underlying selector). |
| **Thread safety** | Pure / constexpr-friendly; no shared mutable state. |

Most applications never call this; it is useful for tests and tooling that
want to assert which class a footprint would hit.

---

## Slab & SlabProxy

**Header:** `spallocator/slab.hpp`

A **slab** is a fixed-size slot recycler: buffers carved into equal cells, with
bitmaps (and related maps) tracking free vs in-use. That is a storage strategy,
not a lifetime domain — the **pool** owns which slabs exist and when they go
away. `SlabProxy` implements the same backend interface for oversized requests
via system `new[]` / `delete[]` (no slot recycling).

Application code usually stays on `Pool` and the smart-pointer helpers. Use slab
types directly only when embedding a single size class or extending the
allocator. Conceptual background:
[CONCEPTS.md — Slab](CONCEPTS.md#what-is-a-slab-and-why-use-one).

### `AbstractSlab`

Polymorphic interface shared by size-class slabs and the large-allocation proxy.

```cpp
class AbstractSlab {
public:
    virtual std::byte* allocateItem(std::size_t size) = 0;
    virtual void deallocateItem(std::byte* item) = 0;
    virtual std::size_t reclaimEmptySlabs(std::size_t keep_min = 1);
    virtual std::size_t getAllocatedMemory() const;
    virtual ~AbstractSlab() = default;
};
```

| Method | Description |
|--------|-------------|
| `allocateItem(size)` | Allocate a block of at least `size` bytes from this slab backend. For `Slab`, `size` must fit the template slot size. Returns a **slot base** pointer (Pool then advances past its header). |
| `deallocateItem(item)` | Free a pointer previously returned by `allocateItem`. Rejects unknown, interior, or already-free addresses (`std::invalid_argument`). |
| `reclaimEmptySlabs(keep_min)` | Drop fully empty buffers; keep at least `keep_min` (default 1). Default base implementation is a no-op; `Slab` overrides. Returns how many buffers were deleted. |
| `getAllocatedMemory()` | Bytes retained by this slab object’s buffers (default `0`). |

Copy/move of `AbstractSlab` are deleted.

### `Slab`

```cpp
template<std::size_t ElemSize, std::size_t Alignment = 16>
class Slab : public AbstractSlab;
```

Fixed-size slot allocator: each slot is `ElemSize` bytes and `Alignment`-aligned
(`Alignment` must be 16, 32, or 64; `ElemSize` must be a multiple of
`Alignment`). Maintains freemap bitsets, a growable availability map, and a
base-address map for frees.

| Method | Description |
|--------|-------------|
| `allocateItem` / `deallocateItem` | See `AbstractSlab`; guarded by the slab’s `SpinLock`. |
| `getElemSize()` / `getAlignment()` / `getAllocSize()` | Compile-time slot size, alignment, and buffer size chosen for this specialization. |
| `getAllocatedMemory()` | `slab_count × buffer_size` currently retained. |
| `getAvailabilityMapBytes()` | Bytes used by the growable availability bitset (diagnostics). |
| `getSlabCount()` | Number of buffers currently held. |
| `reclaimEmptySlabs(keep_min)` | Deletes empty buffers (swap-with-last), retaining ≥ `keep_min`. |
| `findSlabForItem(item)` | Optional index of the buffer owning `item`, if it is an exact slot base in this slab. |

Empty buffers remain mapped after free until reclaim/`Pool::trim` — intentional
monotonic growth for alloc/free latency.

### `SlabProxy`

```cpp
class SlabProxy : public AbstractSlab;
```

Large / overflow path: each `allocateItem` is a distinct aligned `new[]`;
`deallocateItem` always `delete[]`s that pointer. No freemap, no retention
across frees. Pool still attaches its usual allocation header on the returned
user pointer.

### `DynBitset`

Growable bitset used internally for “does this buffer still have free slots?”
metadata. Not part of the stable application-facing API; listed so readers of
`slab.hpp` know what it is. Prefer `Pool` / `Slab` accessors for diagnostics.

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
    PoolBlockToken token;
    void operator()(T* p) const noexcept;   // if alive: ~T() then try_deallocate
};

template<typename T>
struct PoolDeleter<T[]> {
    PoolBlockToken token;
    const std::size_t size;                 // element count
    void operator()(T* p) const noexcept;   // reverse-order dtors, then free
};
```

| Piece | Description |
|-------|-------------|
| `token` | `PoolBlockToken` for the domain (from `Pool&` / `PoolHandle` / token ctors). |
| `size` (array) | Number of elements to destroy; stored in the deleter when the array was created. |
| `operator()` | If `p` is null or `!token.alive()`: no-op (arena). Else: destructor(s), then `try_deallocate`. On free failure while alive, **`std::abort()`**. |

Array destruction order matches stack arrays (last constructed → first
destroyed).

### `make_pool_unique`

```cpp
template<typename T, typename... Args>
    requires (!std::is_array_v<T>)
unique_pool_ptr<T> make_pool_unique(PoolBlockToken token, Args&&... args);
// also: Pool&, PoolHandle const&

template<typename T>  // T = U[]
    requires std::is_unbounded_array_v<T>
unique_pool_ptr<T> make_pool_unique(PoolBlockToken token, std::size_t size);
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
std::shared_ptr<T> make_pool_shared(PoolBlockToken token, Args&&... args);
// also: Pool&, PoolHandle const&

template<typename T>
    requires std::is_unbounded_array_v<T>
std::shared_ptr<T> make_pool_shared(PoolBlockToken token, std::size_t size);
// also: Pool&, PoolHandle const&

template<typename T>
    requires std::is_unbounded_array_v<T>
std::shared_ptr<T> make_pool_shared(PoolBlockToken token, std::size_t size,
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

    explicit PoolAllocator(PoolBlockToken token) noexcept;
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
| Constructor | Takes `PoolBlockToken`, `Pool&`, or `PoolHandle` (token = `block_refs` pin). |
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

## LifetimeObserver

**Header:** `spallocator/lifetimeobserver.hpp`

Intrusive helper for “is this object still alive?” checks from callbacks or
event handlers. Inspired by `shared_ptr` / `weak_ptr`: **owner** references keep
the control block’s owner count; **`getObserver()`** returns a weak view that
does not keep the object alive.

Typical pattern: a type derives from `LifetimeObserver` (protected default ctor
creates an owner). Callbacks capture `getObserver()`, then call `isAlive()`
before touching the object.

```cpp
class LifetimeObserver {
public:
    enum class e_refType { owner, observer };

    bool isAlive() const;
    operator bool() const;

    LifetimeObserver getObserver() const;
    int64_t getCount(e_refType ref_type) const;

    LifetimeObserver& reset(const LifetimeObserver& other, e_refType ref_type);

    explicit LifetimeObserver(const LifetimeObserver& other);
    ~LifetimeObserver();

protected:
    LifetimeObserver();
    LifetimeObserver(const LifetimeObserver& other, e_refType ref_type);
    LifetimeObserver& operator=(const LifetimeObserver& other);
    LifetimeObserver(LifetimeObserver&& other) noexcept;
    LifetimeObserver& operator=(LifetimeObserver&& other) noexcept;
};
```

### Liveness vs object use

| Concern | Guarantee |
|---------|-----------|
| Control-block counts | `std::atomic`; concurrent `isAlive()` / ref adjust do not data-race |
| `isAlive() == false` | Synchronizes with owner teardown of control-block state (acquire vs last release) |
| Same-thread / single-strand | If callback and destroy cannot overlap, `isAlive()`-then-use is complete — no extra mutex required |
| Cross-thread use + destroy | Serialize with the same mutex/strand that covers destroy, or use `shared_ptr` / `weak_ptr` when you need upgrade-to-own |

### Methods

| Method | Description |
|--------|-------------|
| `isAlive()` | `true` if at least one **owner** reference remains. Does **not** pin the object or grant cross-thread member access by itself. |
| `operator bool()` | Same as `isAlive()`. |
| `getObserver()` | Returns a distinct `LifetimeObserver` with **observer** (weak) semantics sharing this control block. Prefer this for callbacks — do not capture an accidental owner copy. |
| `getCount(ref_type)` | Current owner or observer count (diagnostics / tests). |
| `reset(other, ref_type)` | Drop this instance’s current ref and attach to `other`’s control block as `ref_type`. |
| Default ctor (protected) | Creates a new control block with an **owner** ref — used by derived objects. |
| Copy as observer | Public `explicit` copy constructs an **observer** view of `other` (see header notes for owner-copy rules on derived types). |
| Copy/move assign & protected copy with `e_refType` | Transfer or share ownership per `ref_type`; move leaves the source with a fresh owner control block so destruction stays well-defined. |
| Destructor | Releases this instance’s ref; deletes the control block when both counts hit zero (one winner via internal `retired` flag). |

### Usage

**Single-strand** (callback and delete on the same event thread):

```cpp
struct Service : LifetimeObserver {
    void on_event();
};

event_loop.post([obs = svc->getObserver(), svc] {
    if (obs.isAlive())
        svc->on_event();
});
```

**Cross-thread** (mutex covers check/use and destroy):

```cpp
std::mutex mu;  // outlives the service
event_loop.post([&mu, obs = svc->getObserver(), svc] {
    std::lock_guard lock(mu);
    if (obs.isAlive())
        svc->on_event();
});
{
    std::lock_guard lock(mu);
    delete svc;
}
```

More narrative: [CONCEPTS.md — Observing lifetime](CONCEPTS.md#observing-lifetime-without-owning).

---

## SpinLock

**Header:** `spallocator/spinlock.hpp`

Lightweight mutex optimized for **short** critical sections. Models
`BasicLockable` so it works with `std::scoped_lock` / `std::unique_lock`.
Non-copyable, non-movable.

Internally: test-and-test-and-set spin, then escalating sleep backoff, then
`std::atomic_flag::wait`. Under ThreadSanitizer, acquire/release annotations
publish happens-before edges (GCC `__SANITIZE_THREAD__` or Clang
`__has_feature(thread_sanitizer)`).

```cpp
class SpinLock {
public:
    SpinLock() = default;
    ~SpinLock() = default;

    void lock();
    bool try_lock();
    void unlock();
};
```

| Method | Description |
|--------|-------------|
| `lock()` | Blocks until the lock is acquired (spin → backoff → wait). |
| `try_lock()` | Attempts acquisition without blocking. Returns `true` if acquired, `false` if another holder exists. |
| `unlock()` | Releases the lock and wakes one waiter. Caller must hold the lock. Unlocking a lock you do not own (or double-unlock) is undefined in release builds; debug builds abort via an owner-thread assert. |

| | |
|--|--|
| **Thread safety** | Synchronizes threads that use the same `SpinLock` instance around the critical section. Non-recursive: the owning thread must not call `lock()` again until `unlock()`. |
| **Exceptions** | These methods do not throw. |
| **Recommendation** | Prefer RAII — `std::scoped_lock` for scope-bound holds, `std::unique_lock` when you need deferred lock, adopt, or early `unlock()` — so exceptions and mismatched pairs cannot leave the lock held. |

```cpp
SpinLock mu;
{
    std::scoped_lock guard(mu);
    // critical section
}
{
    std::unique_lock guard(mu); // early unlock / re-lock when needed
    // ...
    guard.unlock();
}
```

In non-`NDEBUG` builds, successful `lock` / `try_lock` record `std::this_thread::get_id()`; `unlock` asserts that id matches before clearing the flag. Release builds keep a flag-only object (no owner member).

Algorithm and correctness deep dive:
[SpinLock Analysis](../spallocator/docs/SpinLock_Analysis.md).

---

## Helpers

**Header:** `spallocator/helper.hpp`

Supporting utilities used by the library and demos. They are public, but they
are not the core allocator surface — called out here so call sites and build
flags are clear.

### Size literals

Binary (power-of-1024) size suffixes in namespace `spallocator`:

```cpp
constexpr std::size_t operator""_KB(unsigned long long value);  // × 1024
constexpr std::size_t operator""_MB(unsigned long long value);  // × 1024²
constexpr std::size_t operator""_GB(unsigned long long value);  // × 1024³
```

| Literal | Example | Value |
|---------|---------|-------|
| `_KB` | `4_KB` | `4096` |
| `_MB` | `1_MB` | `1048576` |
| `_GB` | `1_GB` | `1073741824` |

```cpp
using spallocator::operator""_KB;
auto n = 4_KB;
```

### Debug / verbose print

| Flag / constant | Effect |
|-----------------|--------|
| `NDEBUG` defined | `DEBUG_BUILD == false` |
| `NDEBUG` undefined | `DEBUG_BUILD == true` |
| `-DVDEBUG` and debug build | `VERBOSE_DEBUG == true` → `debug_print` / `debug_println` emit |
| Otherwise | `debug_print*` compile to no-ops via `if constexpr` |

```cpp
void print(std::format_string<Args...> fmt, Args&&... args);
void println(std::format_string<Args...> fmt, Args&&... args);
void debug_print(std::format_string<Args...> fmt, Args&&... args);
void debug_println(std::format_string<Args...> fmt, Args&&... args);
```

`print` / `println` always format to stdout (using `std::print` when available).
`debug_*` are for allocator tracing in verbose debug builds only.

### `runtime_assert`

```cpp
void runtime_assert(bool condition, const char* message,
                    const std::source_location& loc = std::source_location::current());
void runtime_assert(bool condition, const std::string& message,
                    const std::source_location& loc = std::source_location::current());
```

| Parameter | Description |
|-----------|-------------|
| `condition` | Must be true if the program invariant holds. |
| `message` | Text included in the failure report. |
| `loc` | Source location (default: call site). |

| | |
|--|--|
| **Debug builds** | On failure: print file/line/function and `std::abort()`. |
| **Release (`NDEBUG`)** | Body discarded with `if constexpr` — **no check**. |
| **Intended use** | Internal programming invariants. |
| **Not for** | Caller-facing validation (use exceptions / error codes); anything with required side effects (they vanish in release). |

---

## Contracts cheat-sheet

| Surface | On failure | Threads |
|---------|------------|---------|
| `Pool::allocate` | throws | Concurrent allocate/free on one pool is supported |
| `Pool::deallocate` | throws | Same |
| `Pool::try_deallocate` | `false` | Same |
| `Pool::trim` / `getAllocatedMemory` | — | Safe |
| `PoolDeleter` / `PoolAllocator::deallocate` | `abort` on bad pointer if alive; no-op if dead | Pool-safe; pointer handoff is the caller’s protocol |
| `PoolHandle::create` / handle drop | may `delete` heap pool when idle | Destroy/create from one thread |
| `make_pool_unique` construction | throw; slot returned | Pool-safe |
| `LifetimeObserver::isAlive` | — | Race-free liveness bit |
| Object members after `isAlive` | — | Complete on one strand; cross-thread needs shared sync with destroy |
| `SpinLock::lock` / `unlock` | — | Mutual exclusion + TSan annotations when built with TSan |
| `runtime_assert` | abort (debug only) | N/A |

See also: [IMPLEMENTATION.md](IMPLEMENTATION.md) for header layout and lock
ordering; [CONCEPTS.md](CONCEPTS.md) for when to choose each tool.
