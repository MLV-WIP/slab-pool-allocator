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
  - [`allocate`](#allocate)
  - [`deallocate` / `try_deallocate`](#deallocate--try_deallocate)
  - [`trim`](#trim)
  - [`getAllocatedMemory`](#getallocatedmemory)
  - [`selectSlab`](#selectslab)
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
| `spallocator/pool.hpp` | `Pool`, alignment constants, size-class ladders |
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
  Smart pointers / PoolAllocator ──► clients of Pool&
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
- Destroying the pool destroys its slabs; do not use pointers from it afterward.

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
~Pool() = default;
```

| | |
|--|--|
| **Effects** | Constructs an empty pool: axis-16 slabs are prepared; axis 32/64 slabs are created lazily on first use. |
| **Thread safety** | Construct/destroy from one thread; do not use the pool concurrently with its destructor. |
| **Copy / move** | Deleted. |

```cpp
spallocator::Pool pool;  // ready to allocate
```

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
    Pool& pool;
    void operator()(T* p) const noexcept;   // ~T() then try_deallocate
};

template<typename T>
struct PoolDeleter<T[]> {
    Pool& pool;
    const std::size_t size;                 // element count
    void operator()(T* p) const noexcept;   // reverse-order dtors, then free
};
```

| Piece | Description |
|-------|-------------|
| `pool` | Pool that owns the block; must outlive the smart pointer. |
| `size` (array) | Number of elements to destroy; stored in the deleter when the array was created. |
| `operator()` | If `p` is non-null: run destructor(s), then `try_deallocate`. On free failure, **`std::abort()`** (must not throw through `noexcept`). |

Array destruction order matches stack arrays (last constructed → first
destroyed).

### `make_pool_unique`

```cpp
template<typename T, typename... Args>
    requires (!std::is_array_v<T>)
unique_pool_ptr<T> make_pool_unique(Pool& pool, Args&&... args);

template<typename T>  // T = U[]
    requires std::is_unbounded_array_v<T>
unique_pool_ptr<T> make_pool_unique(Pool& pool, std::size_t size);

// Bounded array forms are deleted — use std::array or U[].
```

**Single object**

| Parameter | Description |
|-----------|-------------|
| `pool` | Pool used for the underlying `allocate` / free. |
| `args...` | Forwarded to `T`’s constructor via placement new. |

| | |
|--|--|
| **Returns** | `unique_pool_ptr<T>` owning the constructed object. |
| **Alignment** | Uses `alignof(T)`; `static_assert` that it is `≤ 64`. |
| **Exceptions** | If construction throws, the raw block is returned to the pool, then the exception propagates (**strong guarantee**). Allocation failures throw as from `Pool::allocate`. |

**Unbounded array `T = U[]`**

| Parameter | Description |
|-----------|-------------|
| `pool` | Pool for the block. |
| `size` | Number of default-constructed `U` elements. |

| | |
|--|--|
| **Returns** | `unique_pool_ptr<U[]>` with deleter holding `size`. |
| **Exceptions** | If the *k*-th element constructor throws, elements `[0, k)` are destroyed in reverse order, the block is freed, then the exception propagates. |

```cpp
auto u = spallocator::make_pool_unique<Widget>(pool, arg1, arg2);
auto a = spallocator::make_pool_unique<int[]>(pool, 100);
```

### `make_pool_shared`

```cpp
template<typename T, typename... Args>
    requires (!std::is_array_v<T>)
std::shared_ptr<T> make_pool_shared(Pool& pool, Args&&... args);

template<typename T>
    requires std::is_unbounded_array_v<T>
std::shared_ptr<T> make_pool_shared(Pool& pool, std::size_t size);

template<typename T>
    requires std::is_unbounded_array_v<T>
std::shared_ptr<T> make_pool_shared(Pool& pool, std::size_t size,
                                    const std::remove_extent_t<T>& init_value);
```

Builds a `std::shared_ptr` via `std::allocate_shared` and `PoolAllocator`, so
the object (and control block) come from the pool.

| Overload | Parameters | Behavior |
|----------|------------|----------|
| Single object | `pool`, ctor `args...` | `allocate_shared<T>(PoolAllocator<T>{pool}, args...)` |
| Array | `pool`, `size` | `size` default-initialized elements |
| Array + value | `pool`, `size`, `init_value` | `size` elements copy-initialized from `init_value` |

Bounded array overloads are deleted. Array size lives in the `shared_ptr`
control block (unlike `make_pool_unique`, which stores count in `PoolDeleter`).

```cpp
auto s = spallocator::make_pool_shared<Widget>(pool, arg1);
auto arr = spallocator::make_pool_shared<int[]>(pool, 100);
auto filled = spallocator::make_pool_shared<int[]>(pool, 100, 42);
std::weak_ptr<Widget> w = s;
```

---

## PoolAllocator

**Header:** `spallocator/spallocator.hpp`

Standard-library-style allocator that obtains storage from a `Pool`. Enables
`std::vector`, `std::allocate_shared`, and other allocator-aware facilities.

```cpp
template<typename T>
class PoolAllocator {
public:
    using value_type = T;
    // POCCA / POCMA / POCS = false_type; is_always_equal = false_type

    explicit PoolAllocator(Pool& pool) noexcept;
    PoolAllocator(const PoolAllocator&) noexcept = default;
    template<typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept;  // rebind

    [[nodiscard]] T* allocate(std::size_t n);
    void deallocate(T* p, std::size_t n) noexcept;
};
```

| Member / parameter | Description |
|--------------------|-------------|
| Constructor `pool` | Reference to the pool; **not owned**. The pool must outlive this allocator and every container/object still using it. |
| Rebind ctor | Shares the same `Pool&` when containers rebind to node types. |
| `allocate(n)` | Allocates space for `n` objects of type `T` (`n * sizeof(T)` bytes at `alignof(T)`). Throws `std::bad_array_new_length` on overflow; otherwise same failures as `Pool::allocate`. |
| `deallocate(p, n)` | Returns storage for `p`. `n` is ignored (pool headers carry footprint). `noexcept`; bad pointers **abort** via `try_deallocate`. |
| Equality | `operator==` is true only when both allocators refer to the **same** `Pool` instance. |

Propagation traits are false because the pool is external and must not silently
move to another container’s allocator state.

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
| `PoolDeleter` / `PoolAllocator::deallocate` | `abort` on bad pointer | Pool operations are safe; pointer handoff is the caller’s protocol |
| `make_pool_unique` construction | throw; slot returned | Pool-safe |
| `LifetimeObserver::isAlive` | — | Race-free liveness bit |
| Object members after `isAlive` | — | Complete on one strand; cross-thread needs shared sync with destroy |
| `SpinLock::lock` / `unlock` | — | Mutual exclusion + TSan annotations when built with TSan |
| `runtime_assert` | abort (debug only) | N/A |

See also: [IMPLEMENTATION.md](IMPLEMENTATION.md) for header layout and lock
ordering; [CONCEPTS.md](CONCEPTS.md) for when to choose each tool.
