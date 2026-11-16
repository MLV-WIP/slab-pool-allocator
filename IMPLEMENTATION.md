# Implementation Guide

This document provides an in-depth analysis of the slab pool allocator implementation, design decisions, and educational insights. For build/test/demo instructions and a project overview, see [README.md](README.md).

**Educational Focus**: This guide teaches advanced C++ techniques, memory management concepts, and concurrent programming patterns through detailed explanations and real-world examples.

---

## How to Read This Guide (5-minute path)

1. **Component map** (below) — who owns what
2. **[Memory Layout](#memory-layout)** + **[Alignment API](#alignment-api)** — what sits in front of every allocation
3. **[Request Lifecycle](#request-lifecycle)** — one numeric walkthrough of allocate/free
4. Skim **[Smart Pointer Integration](#smart-pointer-integration)** or **[SpinLock Design](#spinlock-design)** depending on your goal
5. Run demos from the README, then return for **[LifetimeObserver](#lifetimeobserver---asynchronous-object-lifetime-tracking)** or **[Thread Safety](#thread-safety-and-concurrent-access)** as needed

Build, test, and demo commands stay in the README so this file can stay focused on design.

### Component diagram

```
                    ┌─────────────────────────────────────┐
                    │         spallocator::Pool           │
                    │  (routes by footprint; pool SpinLock)│
                    └──────────────┬──────────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
     AbstractSlab           AbstractSlab          SlabProxy
     Slab<16> … Slab<1024>  (size classes)        (>1KB → ::new[])
              │
              └── per-slab SpinLock, bitsets, base_address_map

  Smart pointers (spallocator.hpp)          LifetimeObserver
  make_pool_unique / make_pool_shared  ──►  (stand-alone utility;
  PoolAllocator<T> → Pool&                  not part of Pool)
```

**Layers**: `Pool` chooses a slab and writes the allocation header; `Slab`/`SlabProxy` own raw slots; smart pointers and `PoolAllocator` are clients of `Pool&`; `LifetimeObserver` is independent of the allocator.

---

## Table of Contents

- [How to Read This Guide (5-minute path)](#how-to-read-this-guide-5-minute-path)
- [Architecture Deep Dive](#architecture-deep-dive)
- [Memory Layout](#memory-layout)
- [Alignment API](#alignment-api)
- [Request Lifecycle](#request-lifecycle)
- [Slab Allocation Strategy](#slab-allocation-strategy)
- [Bitset Tracking System](#bitset-tracking-system)
- [Address Lookup for Deallocation](#address-lookup-for-deallocation)
- [Smart Pointer Integration](#smart-pointer-integration)
- [LifetimeObserver - Asynchronous Object Lifetime Tracking](#lifetimeobserver---asynchronous-object-lifetime-tracking)
- [Thread Safety and Concurrent Access](#thread-safety-and-concurrent-access)
- [SpinLock Design](#spinlock-design)
- [Performance Analysis](#performance-analysis)
- [Advanced Topics](#advanced-topics)
- [Learning Objectives](#learning-objectives)
- [Further Reading](#further-reading)
- [Debugging and Instrumentation](#debugging-and-instrumentation)
- [Future Enhancements](#future-enhancements)
- [Demo Programs](#demo-programs)

---

## Architecture Deep Dive

### 1. Slab<ElemSize> (`spallocator/slab.hpp`)

The core template class that manages fixed-size memory allocations.

**Key Concepts Demonstrated**:
- **Template Metaprogramming**: Compile-time size calculations using `constexpr`
- **RAII Pattern**: Automatic memory cleanup in destructor
- **Virtual Interface**: Abstract base class `AbstractSlab` enables polymorphic slab management

```cpp
template<const std::size_t ElemSize>
class Slab: public AbstractSlab
```

**Design Insights**:
- Uses bitsets to track allocated/free slots - each bit represents one slot state
- Dynamically grows by allocating additional slabs when needed
- Stores base address mappings in `std::map` for O(log n) deallocation lookup
- Template parameter `ElemSize` allows compile-time optimization

**Educational Highlights**:

**Why bitsets?** Bitsets provide extremely compact memory tracking (1 bit per slot vs 1 byte for bool arrays). For a 4KB slab with 16-byte elements (256 slots), this means 32 bytes vs 256 bytes - an 87.5% reduction in tracking overhead.

**Slab growth strategy**: Initial 4KB allocation balances memory overhead with allocation frequency. This size is chosen because:
1. It's a common memory page size, potentially reducing allocation overhead
2. It provides enough slots to amortize the slab management overhead
3. It's small enough to avoid wasting memory for infrequently-used size classes

**Address mapping**: The `base_address_map` demonstrates how to locate which slab owns a given pointer. This is a classic interval lookup problem solved using a red-black tree (`std::map`).

---

### 2. Pool (`spallocator/pool.hpp`)

High-level interface that manages multiple slabs and routes allocations.

**Key Concepts Demonstrated**:
- **Strategy Pattern**: Selects appropriate slab based on total allocation footprint
- **Memory Metadata**: Prefixed header stores total alloc size and header length for transparent deallocation
- **Size Class Optimization**: Pre-defined size classes reduce fragmentation

```cpp
constexpr std::size_t Pool::selectSlab(std::size_t size) const
```

**Design Insights**:
- Size classes chosen based on common allocation patterns (powers of 2, with intermediate steps)
- `selectSlab()` sees `item_size + header_size`, not the bare user request
- Allocation header stores size for O(1) slab routing during deallocation (no size argument)
- Non-copyable and non-moveable (proper resource management semantics)

**Educational Highlights**:

**Allocation metadata trick**: A small prefix before the user pointer stores the total footprint and header length, so `deallocate()` needs no size argument. This matches the standard `delete` interface and prevents size-mismatch bugs. See [Memory Layout](#memory-layout).

**Size class selection**: The ladder of sizes (16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024) balances fragmentation vs. number of slabs. Intermediate sizes (48, 96, 192, 384, 768) significantly reduce waste. With the default 8-byte header, a 100-byte user request becomes a 108-byte footprint and lands in the 128B class (20 bytes of slot waste, ~16%). Without intermediate classes, that same 108-byte footprint would still fit 128B; the win shows up at boundaries such as 90 → 98 → 128 vs a coarser 256B ladder.

**Constexpr function**: `selectSlab()` is marked `constexpr`, so a call with a compile-time-constant footprint can fold away. In the live `Pool::allocate` path the argument is almost always a **runtime** `item_size + header_size`, so routing still happens at run time. Treat `constexpr` here as a clean, optimizable API — not as “all allocations are compile-time dispatched.”

---

### 3. SlabProxy (`spallocator/slab.hpp`)

Handles large allocations by delegating to the system allocator (`::operator new[]` / `delete[]` with 16-byte alignment).

**Key Concepts Demonstrated**:
- **Adapter Pattern**: Same `AbstractSlab` interface as pooled slabs
- **Separation of Concerns**: Large allocations skip bitsets and address maps

**Design Insights**:
- Used when footprint is **> 1 KB** (`selectSlab` returns “large”)
- Hard ceiling: **≤ 1 GB** (Pool also rejects `item_size > 1_GB` before routing)
- **No ownership map**: unlike `Slab`, there is no `base_address_map` — each large block is a standalone `new[]`
- `deallocateItem` always `delete[]`s the pointer it is given (fire-and-forget)
- Pool still routes free via the **allocation header** (`selectSlab(alloc_size)` → `large_slab`), so the user pointer must be a pool-issued pointer

**Small slab vs SlabProxy**:

| | `Slab<N>` | `SlabProxy` |
|--|-----------|-------------|
| Tracking | Bitsets + `base_address_map` | None |
| Free path | Interval lookup, clear bit | Unconditional `delete[]` |
| Growth | More 4KB (etc.) slabs | N/A — one system allocation per request |
| Best for | Hot size classes ≤ 1KB | Occasional large buffers |

**Educational Highlights**:

**When NOT to pool**: Large allocations are infrequent and don't benefit from pre-allocation overhead. The bookkeeping cost of maintaining a pool for large objects often exceeds the allocation cost itself.

**Interface uniformity**: Same `allocateItem()`/`deallocateItem()` API regardless of backend — Pool does not special-case call sites beyond choosing which `AbstractSlab*` to use.

---

### 4. SpinLock (`spallocator/spinlock.hpp`)

A lightweight spinlock for thread synchronization with advanced contention handling.

**Key Concepts Demonstrated**:
- **Atomic Operations**: Lock-free synchronization using `std::atomic_flag`
- **Test-and-Test-and-Set (TTAS)**: Optimized spinning to reduce cache coherency traffic
- **Escalating Backoff**: Progressively increasing wait times to reduce contention under high load
- **BasicLockable Concept**: Compatible with `std::scoped_lock` and `std::unique_lock`

```cpp
class SpinLock {
    void lock();        // TTAS with escalating backoff
    bool try_lock();    // Non-blocking acquisition attempt
    void unlock();      // Release with notification
};
```

**Design Insights**:
- Uses thread-local random number generator to randomize initial backoff times
- Implements a 10-iteration backoff strategy before blocking with `wait()`
- Doubles wait time on each failed acquisition (escalating backoff: `wait_time += wait_time`)
- Uses memory order acquire/release semantics for proper synchronization

**Educational Highlights**:

**TTAS optimization**: First checks lock state with relaxed ordering (cheaper) before attempting acquisition. This reduces cache line bouncing between cores:
- Simple test-and-set: Every thread continuously writes to the cache line
- TTAS: Threads only read (shared state) until lock appears free, then one writes

**Escalating backoff strategy**: Prevents "thundering herd" when lock becomes available by staggering retry attempts. Avoids excessive wait times of true exponential backoff by using additive doubling rather than multiplicative growth.

**Memory ordering**:
- `acquire` on lock ensures subsequent reads see prior writes from other threads
- `release` on unlock publishes all prior writes to other threads
- This is the foundation of the "happens-before" relationship in C++ memory model

**Thread-local RNG**: Each thread needs different backoff times to avoid synchronized retry attempts. Thread-local storage avoids synchronization overhead while `std::minstd_rand` provides lightweight randomization (single 32-bit state).

**Notification mechanism**: C++20 `atomic_flag::wait()`/`notify_one()` provides efficient blocking when contention is high, avoiding CPU waste while maintaining low latency for the common case.

**When to use SpinLocks vs Mutexes**:
- **SpinLocks**: Short critical sections (< 100 cycles), low contention, known bounded wait times
- **Mutexes**: Longer critical sections, high contention, unpredictable wait times (avoid wasting CPU cycles)

---

### 5. Helper Utilities (`spallocator/helper.hpp`)

Common utilities and modern C++ conveniences.

**Key Concepts Demonstrated**:
- **User-Defined Literals**: Custom `_KB`, `_MB`, `_GB` operators for readable size constants
- **Format Library**: C++23 `std::format` for type-safe string formatting
- **Source Location**: Automatic capture of file/line info in assertions

```cpp
constexpr std::size_t operator""_KB(unsigned long long value) {
    return value * 1024;
}
```

**Educational Highlights**:

**User-defined literals**: Enable `4_KB` instead of `4096` for better readability. This is a powerful C++ feature that allows domain-specific syntax while maintaining type safety.

**Runtime assertions**: `spallocator::runtime_assert` is a **debug-build** check. It is compiled in only when `DEBUG_BUILD` is true (`NDEBUG` is not defined). In release builds the body is discarded via `if constexpr`, so it is a no-op — closer to `<cassert>` than to a always-on production invariant. Prefer real error handling (`throw`, return codes) for conditions that can fail because of callers or the environment. When active, failures print file/line via `std::source_location` and call `std::abort()`.

**Modern formatting**: `std::format` is type-safe unlike `printf`, and more efficient than streams. It combines the best of both worlds: compile-time format string checking with runtime efficiency.

---

## Memory Layout

Every `Pool::allocate(N, alignment)` reserves a **header** in front of the user payload. The returned pointer is past the header; deallocation reads metadata from fixed offsets behind that pointer.

```
Default layout (alignment ≤ 8 → header_size = 8):

  slab slot base                                                user pointer
  │                                                             │
  ▼                                                             ▼
┌──────────┬─────────┬────────────────┬────────────────┬──────────────────┐
│ pad      │ magic   │ header_size    │ alloc_size     │ payload (N B)    │
│ (2 B)    │ (1 B)   │ (1 byte)       │ (uint32 / 4 B) │                  │
└──────────┴─────────┴────────────────┴────────────────┴──────────────────┘
             ▲         ▲                ▲
             │         │                └─ user_ptr − 4
             │         └─ user_ptr − 5
             └─ user_ptr − 6  (kAllocMagic; cleared on free)
```

**Fields** (from the user pointer backward):

| Offset from user ptr | Type | Meaning |
|----------------------|------|---------|
| `−4 … −1` | `uint32_t` | Total footprint `alloc_size = N + header_size` (used to select slab on free) |
| `−5` | `uint8_t` | `header_size` itself (recover slot base; must be 8/16/32/64) |
| `−6` | `uint8_t` | Allocation magic (`kAllocMagic`); validated then cleared on free |
| `−header_size … −7` | padding | Remaining pad so the user pointer meets `alignment` |

**Deallocate checks** (before trusting the header): `header_size ∈ {8,16,32,64}`, magic matches, `header_size ≤ alloc_size ≤ 1_GB + kMaxAlignment`. Non-pool pointers, corrupted headers, and Pool-level double-frees fail validation. Direct `Pool::deallocate` throws `std::invalid_argument`; `Pool::try_deallocate` returns `false` without throwing. **`PoolAllocator::deallocate` and `PoolDeleter`** are `noexcept` and use `try_deallocate` (abort on failure) so exceptions never cross the STL/`unique_ptr` noexcept boundary. Returned blocks are exclusively owned until free; do not mutate the hidden header. In ASan builds the header region is poisoned after free and unpoisoned on the next allocate into that slot.

**Rules implemented in `Pool::allocate`:**

- `alignment` must be a power of two in `[4, 64]` (default `8`)
- `header_size = max(8, alignment)` — at least 8 bytes so size + header_size byte always fit; larger alignments widen the prefix so the user pointer stays aligned
- Request align **4/8** map to native **axis 16** (slot over-align is valid; header stays 8)
- **Compound-key slabs** `Slab<ElemSize, Alignment>` with per-axis ladders (16 eager; 32/64 lazy on first use)
- Beyond that axis’s max footprint → `SlabProxy` (always `align_val_t{64}`)
- Slab selection uses **`(alloc_size, nativeAxis(alignment))`**

**Overhead examples** (header only, before bitset amortization):

| User request N | Alignment | header_size | alloc_size | Size class / path | Header / N |
|----------------|-----------|-------------|------------|-------------------|------------|
| 128 | 8 (default) | 8 | 136 | 192B (axis 16) | 6.25% |
| 100 | 8 | 8 | 108 | 128B (axis 16) | 8% |
| 16 | 8 | 8 | 24 | 32B (axis 16) | 50% |
| 8 | 8 | 8 | 16 | 16B (axis 16) | 100% of footprint is half header |
| 64 | 16 | 16 | 80 | 96B (axis 16) | 25% |
| 64 | 64 | 64 | 128 | 128B (axis 64, lazy) | 100% |

**Why this design?**

Storing size next to the object eliminates the need to pass size to `deallocate()`, matching `delete` and preventing size-mismatch bugs. Recording `header_size` separately supports variable alignment without encoding tricks in the pointer value.

Alternative approaches:
1. **Require size in deallocate()**: Error-prone, easy to pass wrong size
2. **Use pointer alignment tricks**: Limits allocator flexibility, harder to debug
3. **Separate size table**: Extra indirection, more memory overhead for large numbers of allocations

The header approach is simple, efficient, and robust.

### Alignment API

`Pool::allocate` accepts an optional alignment:

```cpp
std::byte* Pool::allocate(std::size_t size, std::size_t alignment = 8);
```

| Rule | Detail |
|------|--------|
| Default | `alignment = 8` |
| Allowed | Power of two in **`[4, 64]`** (`kMinAlignment` … `kMaxAlignment`) |
| Rejected | `< 4`, `> 64`, or non-power-of-two → `std::invalid_argument` (all builds) |
| Header width | `header_size = max(8, alignment)` so the returned user pointer is aligned |
| Native axis | `nativeAxis(align)`: 4/8/16 → **16**; 32 → 32; 64 → 64 |
| Size classes | `Slab<ElemSize, Alignment>` ladders per axis (16 eager; 32/64 lazy) |
| Over axis max | `SlabProxy` with always `align_val_t{64}` |
| Factories | `static_assert(alignof(T) ≤ 64)` |

**Examples:**
- `allocate(64)` → 8-byte header, footprint 72 → 96B class (axis 16)
- `allocate(64, 16)` → 16-byte header, footprint 80 → 96B class (axis 16)
- `allocate(64, 64)` → 64-byte header, footprint 128 → 128B class (axis 64, lazy slab)
- `alignof(T)` from `make_pool_unique` / `PoolAllocator` is passed through when constructing objects

---

## Request Lifecycle

End-to-end path for a typical call `pool.allocate(100)` (default alignment 8):

1. **Validate** — reject `N > 1_GB`; require power-of-two alignment in `[4, 64]` (throws in all builds)
2. **Compute header** — `header_size = max(8, 8) = 8`
3. **Compute footprint** — `alloc_size = 100 + 8 = 108`
4. **Select slab** — `nativeAxis(8) → 16`, then `selectClassIndex(108, 16)` → **128B** class (not 96B: 108 > 96); lazy axes use `slab_init_lock` on first create
5. **Allocate slot** — slab’s own lock guards bitset updates; return a slot base pointer
6. **Write header** — store `alloc_size`, `header_size`, and `kAllocMagic` at fixed offsets behind the user pointer
7. **Return** — `base + header_size` to the caller (exclusive ownership until `deallocate`)

Deallocate reverses the path:

1. Read `alloc_size` from `item − 4` and `header_size` from `item − 5`
2. Recover `original = item − header_size`
3. `selectSlab(alloc_size)` under the pool lock → same slab
4. `slab->deallocateItem(original)` clears the bit and updates availability maps (does not delete empty slab buffers; use `Pool::trim()` for that)

### User size → size class (default alignment 8)

`selectSlab` thresholds are on **footprint** `N + 8`:

| User N (bytes) | Footprint | Size class |
|----------------|-----------|------------|
| 1–8 | 9–16 | 16B |
| 9–24 | 17–32 | 32B |
| 25–40 | 33–48 | 48B |
| 41–56 | 49–64 | 64B |
| 57–88 | 65–96 | 96B |
| 89–120 | 97–128 | 128B |
| 121–184 | 129–192 | 192B |
| 185–248 | 193–256 | 256B |
| 249–376 | 257–384 | 384B |
| 377–504 | 385–512 | 512B |
| 505–760 | 513–768 | 768B |
| 761–1016 | 769–1024 | 1024B |
| ≥ 1017 | ≥ 1025 | `SlabProxy` (large path) |

With `alignment = 16`, use `N + 16` against the same thresholds.

---

## Slab Allocation Strategy

### Initial Allocation Sizes

- **Small sizes (< 1 KB)**: Initial slab size of 4 KB
- **Large sizes (≥ 1 KB)**: Initial slab size of `ElemSize * 4`

### Growth Policy

- New slabs allocated on-demand when current slabs are full
- Each slab is the same size (no exponential growth)
- Maximum of `4 GB / slab_alloc_size` slabs per size class

### Why Linear Growth?

The growth model is linear (same-sized slabs) rather than exponential because:

1. **Predictable memory usage**: Applications can reason about maximum memory consumption
2. **Simpler to reason about**: No complex growth formulas
3. **Prevents excessive over-allocation**: Exponential growth can lead to wasteful pre-allocation
4. **Better for debugging**: Uniform slab sizes make memory dumps easier to analyze

Trade-off: More frequent allocations under sustained growth, but this is acceptable since slab allocation itself is fast.

---

## Bitset Tracking System

Each slab maintains two types of bitsets for efficient allocation tracking.

### 1. Slab Map (`slab_map`)

Tracks individual slot allocation status within each slab.

```cpp
std::vector<std::bitset<slab_alloc_size / ElemSize>> slab_map;
```

- **Size**: One bit per slot (e.g., 256 bits for 16-byte elements in 4KB slab)
- **Semantics**: 1 = allocated, 0 = free
- **Usage**: Scanned to find free slots during allocation

### 2. Availability Map (`slab_available_map`)

Tracks which slabs have at least one free slot.

```cpp
// DynBitset: vector<uint64_t> words, sized to live slab count
DynBitset slab_available_map;
```

- **Size**: One bit per **live** slab (`⌈N/64⌉` words), grown geometrically on `allocateNewSlab` — not a fixed `bitset<4_GB/slab_size>` (~128 KiB per 4 KiB class)
- **Semantics**: 1 = has free slots, 0 = completely full
- **Usage**: Fast filtering to skip full slabs; when every live slab is full, allocate a new slab then retry (no phantom bits past `slab_data.size()`)
- **RSS**: A fresh `Pool` with 12 eager native-16 classes pays a few words of availability metadata total, not ~1.5 MiB of empty bitsets

### Two-Level Bitmap Optimization

This is a classic optimization pattern used throughout systems programming:

**Without availability map:**
```
For each allocation:
  For each slab:
    For each slot in slab:
      If slot is free: allocate and return
```
Complexity: O(total_slabs × slots_per_slab)

**With availability map:**
```
For each allocation:
  For each slab marked available:
    For each slot in slab:
      If slot is free: allocate and return
```
Complexity: O(available_slabs × slots_per_slab)

When most slabs are full (common in long-running applications), this dramatically reduces allocation time.

**Real-world examples of this pattern:**
- Linux buddy allocator (zone watermarks)
- Page frame allocators (free page lists)
- Disk block allocation (block group descriptors in ext4)
- Network buffer management (available buffer pools)

---

## Address Lookup for Deallocation

### The Problem

Given a pointer, which slab does it belong to? This is an interval lookup problem.

### The Solution

Maintain a map of slab base addresses:

```cpp
std::map<std::byte*, std::size_t> base_address_map;
```

### Algorithm (in `findSlabForItem()`)

1. Use `upper_bound()` to find the first slab starting AFTER the pointer
2. Back up one entry to find the slab starting BEFORE the pointer
3. Verify pointer is within `[slab_start, slab_end)` range
4. Return slab index as `std::optional<std::size_t>`

### Failure mode (unknown / invalid pointers)

`upper_bound` + predecessor only yields a **candidate** slab. Pointers in gaps between slabs, before the first slab, or otherwise outside every `[start, end)` must not abort the process:

1. **`findSlabForItem`**: if the candidate range check fails (or there is no predecessor), emit `debug_println` (when verbose debug is on) and return `std::nullopt`
2. **`deallocateItem`**: if the optional is empty, throw `std::invalid_argument` (“no corresponding slab found”)
3. **`deallocateItem`**: require an **exact slot base** — `item == slab_start + index * ElemSize` (and `index` in range). An interior pointer must not use integer division alone (that would clear the wrong freemap bit)

So invalid frees are **detectable errors**, not `runtime_assert` fatals. Callers that pass non-pool pointers, interior pointers, or already-freed addresses should expect an exception from the slab layer (Pool still assumes a well-formed header behind the user pointer).

### Why This Works

The map is keyed by slab start addresses. For any **valid** pool pointer:
- It must fall within some slab's range `[start, start+size)`
- `upper_bound(ptr)` finds the next slab after ptr
- Backing up one entry finds the slab that could contain ptr
- Final bounds check confirms it's actually in that slab; otherwise treat as unknown

### Alternative Approaches

| Approach | Time | Space | Pros | Cons |
|----------|------|-------|------|------|
| **std::map** (current) | O(log n) | O(n) | Simple, flexible | Extra indirection |
| **Hash table** | O(1) | O(n) | Faster lookup | Requires alignment tricks |
| **Linear search** | O(n) | O(n) | Simplest code | Too slow for many slabs |
| **Binary search on vector** | O(log n) | O(n) | Better cache locality | Insert/delete expensive |

The map approach balances performance with code clarity, making it ideal for an educational implementation.

---

## Smart Pointer Integration

The allocator provides RAII-based memory management through `make_pool_unique`, a pool-aware analog to `std::make_unique`.

### Design Philosophy

Smart pointers solve a fundamental problem in custom allocators: **matching allocation and deallocation**. Raw pool allocations require manual cleanup, creating opportunities for leaks and use-after-free bugs. By integrating with `std::unique_ptr`, we get:

1. **Automatic cleanup**: Memory returns to pool when pointer goes out of scope
2. **Exception safety**: Cleanup happens even during stack unwinding
3. **Move semantics**: Efficient ownership transfer without copying
4. **Type safety**: Compiler enforces correct usage patterns

### Components

#### 1. PoolDeleter - Custom Deleter

**Key Concepts Demonstrated**:
- **Custom deleters**: Extending `std::unique_ptr` behavior
- **Template specialization**: Different behavior for arrays vs. single objects
- **RAII principles**: Automatic resource management
- **C++20 ranges**: Efficient iteration with `std::views::counted` and `std::views::reverse`

```cpp
template<typename T>
struct PoolDeleter {
    spallocator::Pool& pool;

    void operator()(T* p) const {
        if (p) {
            p->~T();  // Explicit destructor call
            pool.deallocate(reinterpret_cast<std::byte*>(p));
        }
    }
};
```

**Design Insights**:
- Stores a reference to the pool (not a pointer, ensuring it exists)
- Explicitly invokes destructor before deallocation (placement new counterpart)
- Checks for null before deallocating (matches standard `delete` behavior)

#### 2. PoolDeleter<T[]> - Array Specialization

Handling arrays requires special care: destructors must run in **reverse order** (LIFO), and the deleter must know the element count. This implementation stores the count in the **deleter object** (not in a second size prefix in front of the array). `Pool::allocate` still writes its own allocation header in front of the user pointer for slab routing.

```cpp
template<typename T>
struct PoolDeleter<T[]> {
    spallocator::Pool& pool;
    const std::size_t size;  // element count captured at construction

    void operator()(T* p) const {  // Note: T*, not T*[]
        if (p) {
            // Call destructors in reverse order using ranges
            std::ranges::for_each(
                std::views::counted(p, size) | std::views::reverse,
                [](T& elem){ elem.~T(); }
            );

            // Deallocate the user pointer; Pool reads its own header behind p
            pool.deallocate(reinterpret_cast<std::byte*>(p));
        }
    }
};
```

**Educational Highlights**:

**Why reverse order?** This mirrors how stack-allocated arrays work. If objects depend on each other, reverse destruction order prevents use-after-destruction bugs:

```cpp
struct Node {
    Node* next;
    ~Node() { if (next) /* access next... */ }
};
```

If `next` is destroyed first, the destructor accesses freed memory.

**Ranges composition**: The expression `std::views::counted(p, size) | std::views::reverse` demonstrates C++20 ranges:
- `std::views::counted(p, size)`: Creates a range from pointer + size
- `| std::views::reverse`: Composes with reverse view (lazy, no copying)
- `std::ranges::for_each`: Applies lambda to each element

This is more elegant than manual indexing:
```cpp
// Old way (error-prone)
for (std::size_t i = size; i-- > 0; ) {
    p[i].~T();
}

// Ranges way (intent-revealing, harder to mess up)
std::ranges::for_each(
    std::views::counted(p, size) | std::views::reverse,
    [](T& elem){ elem.~T(); }
);
```

**Count in the deleter (not a second in-memory size header):** `std::unique_ptr` type-erases the deleter by value, so `PoolDeleter<T[]>{pool, size}` can carry the element count. That avoids a second `std::size_t` prefix in front of the array. The only bytes before the array elements are `Pool`'s normal allocation header (see [Memory Layout](#memory-layout)).

```
┌──────────────────────┬──────────────────────────────────┐
│ Pool allocation hdr  │   Array elements (count × sizeof(T)) │
│ (max(8, align) bytes)│                                      │
└──────────────────────┴──────────────────────────────────────┘
^                      ^
│                      └─ User pointer held by unique_ptr
└─ Slab slot base (Pool recovers via header_size)

Element count lives in PoolDeleter::size, not in a second prefix.
```

**Alignment considerations**: Array allocation requests alignment of at least `alignof(std::size_t)` so the payload meets the stricter of the element type and that floor (matching how `Pool` sizes its header):
```cpp
std::size_t alignment = alignof(ElementType) > alignof(std::size_t) ?
                        alignof(ElementType) : alignof(std::size_t);
std::byte* mem = pool.allocate(sizeof(ElementType) * size, alignment);
```

`pool.allocate` returns an already-aligned user pointer; the factory constructs elements at that address with placement new.

#### 3. make_pool_unique - Factory Functions

**Single Object Version**:
```cpp
template<typename T, typename... Args>
    requires (!std::is_array_v<T>)
constexpr unique_pool_ptr<T> make_pool_unique(spallocator::Pool& pool, Args&&... args)
{
    void* mem = pool.allocate(sizeof(T), alignof(T));
    try {
        T* obj = new (mem) T(std::forward<Args>(args)...);
        return unique_pool_ptr<T>(obj, PoolDeleter<T>{pool});
    } catch (...) {
        pool.deallocate(reinterpret_cast<std::byte*>(mem));
        throw;
    }
}
```

**Educational Highlights**:

**Concepts constraint**: `requires (!std::is_array_v<T>)` ensures this overload only applies to non-array types. This is cleaner than SFINAE and produces better error messages.

**Perfect forwarding**: `std::forward<Args>(args)...` preserves value categories (lvalue vs rvalue) when passing arguments to constructor. This enables:
```cpp
auto obj = make_pool_unique<MyClass>(pool, std::move(expensive_arg));
// expensive_arg is moved, not copied
```

**Placement new**: `new (mem) T(...)` constructs object at specific memory location. The pool provides raw memory; placement new invokes the constructor.

**Exception safety (strong guarantee):** If `T`’s constructor throws, the object was never constructed, so the factory returns the raw slot to the pool and rethrows — no leak.

**Alignment specification**: Passing `alignof(T)` ensures properly aligned memory for the type. Misaligned access can cause crashes on some architectures (e.g., ARM) or performance degradation on others (e.g., x86).

**Array Version**:
```cpp
template<typename T>
    requires std::is_unbounded_array_v<T>
constexpr unique_pool_ptr<T> make_pool_unique(spallocator::Pool& pool, std::size_t size)
{
    using ElementType = std::remove_extent_t<T>;

    std::size_t array_size = sizeof(ElementType) * size;
    std::size_t alignment = alignof(ElementType) > alignof(std::size_t) ?
                            alignof(ElementType) : alignof(std::size_t);

    // Pool writes its own allocation header; mem is the user pointer
    std::byte* mem = pool.allocate(array_size, alignment);
    ElementType* array_ptr = reinterpret_cast<ElementType*>(mem);

    std::size_t constructed = 0;
    try {
        for (; constructed < size; ++constructed) {
            new (&array_ptr[constructed]) ElementType();
        }
    } catch (...) {
        while (constructed > 0) {
            array_ptr[--constructed].~ElementType();
        }
        pool.deallocate(mem);
        throw;
    }

    // Element count travels with the deleter, not a second in-memory prefix
    return unique_pool_ptr<T>(array_ptr, PoolDeleter<T>{pool, size});
}
```

**Educational Highlights**:

**Type extraction**: `std::remove_extent_t<T>` extracts element type from array type:
- Input: `int[]` → Output: `int`
- Input: `MyClass[]` → Output: `MyClass`

**Default initialization**: Each array element is default-constructed with `ElementType()`. For types with non-trivial constructors, this ensures proper initialization. For primitive types (int, float), this zero-initializes.

**Deleter-held size vs pool header**: Two different “sizes” are in play — `PoolDeleter::size` is the **element count** for destructor loops; `Pool`’s header stores the **byte footprint** for slab routing. Do not confuse them.

**Exception safety (strong guarantee):** If construction of element *N* throws, already-constructed elements are destroyed in reverse order and the block is returned to the pool before rethrowing:

```cpp
std::size_t constructed = 0;
try {
    for (; constructed < size; ++constructed) {
        new (&array_ptr[constructed]) ElementType();
    }
} catch (...) {
    while (constructed > 0) {
        array_ptr[--constructed].~ElementType();
    }
    pool.deallocate(mem);
    throw;
}
```

**Bounded Array Deletion**:
```cpp
template<typename T, typename... Args>
    requires std::is_bounded_array_v<T>
void make_pool_unique(spallocator::Pool& pool, Args&&... args) = delete;
```

This prevents usage like `make_pool_unique<int[10]>(pool)`, which is ambiguous (does caller specify size or not?). Use `std::array<int, 10>` instead for fixed-size arrays.

### Usage Patterns

**Basic object allocation**:
```cpp
spallocator::Pool pool;
auto obj = spallocator::make_pool_unique<MyClass>(pool, arg1, arg2);
// obj is std::unique_ptr<MyClass, PoolDeleter<MyClass>>
// Automatically cleaned up when obj goes out of scope
```

**Array allocation**:
```cpp
auto arr = spallocator::make_pool_unique<int[]>(pool, 100);
arr[0] = 42;  // Works like normal array
// All 100 elements destroyed in reverse order at end of scope
```

**Transfer ownership**:
```cpp
spallocator::unique_pool_ptr<MyClass> create_object(spallocator::Pool& pool) {
    return spallocator::make_pool_unique<MyClass>(pool, args...);
}

auto obj = create_object(pool);  // Move semantics, no copying
```

**Container storage**:
```cpp
std::vector<spallocator::unique_pool_ptr<MyClass>> objects;
objects.push_back(spallocator::make_pool_unique<MyClass>(pool, args...));
// Move into vector, no deep copy
```

### Comparison to Standard Smart Pointers

| Feature | `std::make_unique` | `spallocator::make_pool_unique` |
|---------|-------------------|--------------------------------|
| **Allocation source** | System allocator (new) | Custom pool allocator |
| **Deallocation** | `delete` | `pool.deallocate()` |
| **Destructor handling** | Automatic | Explicit call (placement new counterpart) |
| **Array support** | Yes (`T[]`) | Yes; element count in `PoolDeleter`, reverse destruction |
| **Exception safety** | Full | Full (array ctor unwind + `pool.deallocate`) |
| **Alignment** | Standard alignment | Explicit alignment specification |
| **Performance** | General-purpose | Optimized for specific size classes |

### std::shared_ptr Support

✅ **IMPLEMENTED** - The allocator provides full `std::shared_ptr` support through `make_pool_shared` and `PoolAllocator<T>`.

**Implementation Strategy**:

Rather than using custom deleters (as with `unique_ptr`), we leverage `std::allocate_shared` with a standard-compliant allocator:

```cpp
template<typename T>
class PoolAllocator
{
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::false_type;  // equal only if same Pool&

    explicit PoolAllocator(Pool& pool) noexcept : pool_ref(pool) {}

    template<typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept;  // rebind

    T* allocate(std::size_t n);
    void deallocate(T* p, std::size_t n) noexcept;

private:
    Pool& pool_ref;  // non-owning; pool must outlive allocator + containers
};
```

#### Stateful allocator contract

| Trait / rule | Value | Why it matters |
|--------------|-------|----------------|
| **Pool lifetime** | `Pool&` only | Destroying the pool while a `vector`/`shared_ptr` still uses it is UB |
| **POCCA / POCMA / POCS** | `false` | Copy/move/swap of containers must **not** steal or replace the pool reference |
| **`is_always_equal`** | `false` | Allocators compare equal iff they refer to the **same** `Pool` instance |
| **`operator==`** | `&lhs.pool_ref == &rhs.pool_ref` | Cross-pool container operations that assume interchangeable allocators are unsafe |
| **Rebind** | `PoolAllocator<U>(PoolAllocator<T>)` | Node-based containers (`map`, `list`) allocate different `U` from the same pool |

**How `make_pool_shared` works**:
- `std::allocate_shared` uses `PoolAllocator` for **both** the object and control block
- Standard library handles reference counting, weak pointers, and thread safety
- Control block and object allocated from the pool in a single allocation (like `std::make_shared`)
- Deleter is embedded in the control block (no custom deleter needed)

**Benefits Over Custom Deleter Approach**:
1. ✅ Single allocation for object + control block (better cache locality)
2. ✅ Standard library handles all complexity (reference counting, weak pointers)
3. ✅ Thread-safe reference counting built-in
4. ✅ No deleter lifetime issues (embedded in control block)
5. ✅ Works with `std::weak_ptr` automatically
6. ✅ Same API as `std::make_shared` (familiar to users)

**Array Support**:
```cpp
auto arr = make_pool_shared<int[]>(pool, 100);        // Default init
auto arr2 = make_pool_shared<int[]>(pool, 100, 42);   // Value init
```

Unlike `make_pool_unique` (which stores the element count in `PoolDeleter`), `shared_ptr` tracks array size internally in the control block.

### Learning Value

This implementation teaches:

1. **Custom deleters**: How to extend standard library smart pointers (`unique_ptr`)
2. **Standard allocators**: C++ allocator requirements for `shared_ptr` integration
3. **Template specialization**: Providing different behavior for related types (arrays vs single objects)
4. **Concepts and constraints**: Modern C++ type restrictions with clear errors
5. **Perfect forwarding**: Preserving value categories through forwarding references
6. **Placement new/explicit destructors**: Manual object lifetime management
7. **C++20 ranges**: Composable, lazy view transformations (reverse iteration)
8. **RAII principles**: Automatic resource management through scope
9. **Type traits**: Compile-time type introspection and manipulation
10. **Memory alignment**: Hardware requirements for safe memory access
11. **Stateful allocators**: `PoolAllocator` stores reference to pool, demonstrating stateful design
12. **Allocator rebind**: Supporting container node allocations (e.g., `std::map` nodes)
13. **Exception safety**: Resource leak prevention during unwinding

---

## LifetimeObserver - Asynchronous Object Lifetime Tracking

### The Problem: Use-After-Free in Async Callbacks

A common challenge in asynchronous programming is ensuring an object is still alive before accessing it in a callback:

```cpp
class EventHandler {
public:
    void registerCallback(std::function<void()> cb) {
        // Callback will be invoked later, possibly after object destruction
        event_system.on_event([this, cb](){
            // But 'this' might be invalid by now!
            process(cb);
        });
    }
};

EventHandler handler;
handler.registerCallback([](){ /* ... */ });
// handler destroyed, but callback still pending...
```

This creates a use-after-free vulnerability: the callback invokes a method on a destroyed object.

### The Solution: LifetimeObserver

`LifetimeObserver` is a helper class that tracks whether an object is still alive. The pattern allows:

1. Objects to be observed (tracked for liveness)
2. Callbacks to safely check if the object still exists before using it
3. Weak references that don't prevent object destruction

**Key Concepts Demonstrated**:
- **Observer Pattern**: Separation of concerns between object lifetime and observers
- **Mediator Pattern**: Control block mediates between strong and weak references
- **Reference Counting**: Lightweight reference counting for lifetime management
- **Async-oriented checks**: Observers can ask “is the owner still alive?” before touching the object

### Thread model (read this first)

`ControlBlock` stores **`std::atomic<int64_t>`** owner/observer counts. Concurrent `isAlive()` / `getCount()` / ref adjust do **not** data-race on the counters. `isAlive()` loads with **acquire**; the last owner `releaseRef` uses **acq_rel**, so a false `isAlive()` synchronizes with owner teardown of the control-block state.

| Situation | Safe? |
|-----------|--------|
| Concurrent `isAlive()` while another thread destroys the owner / releases refs | **Yes** for the liveness bit (no data race on counts) |
| Single-threaded or mutex-protected use of the observed object after `isAlive()` | Yes |
| Check `isAlive()` then use the object on another thread with no lock / ownership handoff | **No** — classic TOCTOU; atomics do not fix this |
| Prefer `shared_ptr` / `weak_ptr` when you need lock-style upgrade to a strong ref | Often yes — `weak_ptr::lock()` is the closer production pattern for “use if still alive” |

Liveness is race-free; **object access** after a true `isAlive()` still needs an external protocol (mutex, strand, or upgrade to shared ownership). See [pitfalls](#common-pitfalls).

### Design Architecture

```cpp
class LifetimeObserver
{
    struct ControlBlock {
        std::atomic<int64_t> owner_count;      // Strong references
        std::atomic<int64_t> observer_count;   // Weak references
        std::atomic<bool> retired;             // One-winner delete of the block
    };

    ControlBlock* control_block;  // Shared between strong and weak references
    e_refType my_ownership;       // owner = strong, observer = weak
};
```

The `ControlBlock` is the mediator pattern implementation:
- **Owner References** (`e_refType::owner`): Owned by the original object
- **Observer References** (`e_refType::observer`): Held by callbacks/handlers
- **Control Block**: Mediates all reference operations, survives object destruction

### Usage Pattern

Hold an **external** mutex across both callback use and `delete`. A member mutex
unlocked at the end of the derived destructor is too late: `~LifetimeObserver`
(which drops the owner count) runs *after* that unlock.

```cpp
class AsyncService: public LifetimeObserver
{
public:
    void process_data(const Data& d);
};

std::mutex mu;  // outlives the service
AsyncService* svc = new AsyncService();

event_loop.on_data([&mu, svc, alive = svc->getObserver()](const Data& d)
{
    std::lock_guard lock(mu);
    if (alive)
    {
        svc->process_data(d);  // safe: delete cannot run while we hold mu
    }
});

{
    std::lock_guard lock(mu);
    delete svc;  // full dtor chain (including owner release) runs under lock
    svc = nullptr;
}
// Later callbacks take the lock, see isAlive() == false, and skip
```

### Key Components

#### 1. ControlBlock - Reference Count Mediator

```cpp
struct ControlBlock {
    int64_t addRef(e_refType ref_type);
    int64_t releaseRef(e_refType ref_type);
    int64_t getCount(e_refType ref_type) const;
};
```

**Design Insights**:
- Maintains separate counts for owners and observers
- Non-copyable/non-moveable (proper resource semantics)
- Deleted copy/move operations prevent accidental duplication
- Survives as long as any references exist

#### 2. getObserver() - Creating Observer References

```cpp
LifetimeObserver observer = obj.getObserver();
```

**Educational Highlights**:

**Why separate method?** Creates a new `LifetimeObserver` with weak reference semantics:
```cpp
LifetimeObserver(const LifetimeObserver& other, e_refType::observer)
// Creates weak reference copy, not owned by this object
```

This explicitly communicates intent - the observer doesn't prevent object destruction.

In addition, requiring an observer to request a unique observer object via method
rather than creation via assignment or copy constructor avoids making confusing
overloaded use cases for copying. Internally, assignment is reserved for the case
where the owning inheriting object is copied.

#### 3. isAlive() - Liveness Check

```cpp
if (observer.isAlive()) {
    // Object still exists
}
```

**Implementation**:
```cpp
bool isAlive() const {
    return control_block->getCount(e_refType::owner) > 0;
}
```

**What this means**:
- Acquire-loads `owner_count` from the shared control block (`std::atomic`)
- Concurrent liveness checks vs owner destruction are free of data races on the counters
- Observer can outlive the object: the control block remains while any owner or observer refs exist
- Does **not** by itself make a following member call race-free on another thread (see [Thread model](#thread-model-read-this-first))

**Usage in Callbacks**:
```cpp
auto callback = [&mu, &obj, observer = obj.getObserver()]() {
    std::lock_guard lock(mu);  // same mutex teardown holds around delete
    if (observer) {            // operator bool() = isAlive()
        obj->use();            // safe under the lock
    }
};
```

### Copy and Move Semantics

#### Copy Constructor

```cpp
LifetimeObserver(const LifetimeObserver& other)
    : control_block(other.control_block),
      my_ownership(e_refType::observer)
{
    control_block->addRef(e_refType::observer);
}
```

**Design insight**: Default copy always creates observer (weak) reference.

For owner copies, which are intended to occur only in the case where the
inheriting object is being copied, explicitly use the constructor that
expects a reference type:
```cpp
LifetimeObserver(original, e_refType::owner);
// Creates separate control block, independent lifetime
```

#### Move Constructor

```cpp
LifetimeObserver(LifetimeObserver&& other) noexcept
    : control_block(other.control_block),
      my_ownership(other.my_ownership)
{
    other.my_ownership = e_refType::owner;
    other.control_block = new ControlBlock(other.my_ownership);
}
```

**Educational highlights**:

**Why new control block?** Moved-from object must still be valid:
- Old `control_block` transfers to new object
- Moved-from object gets fresh owner reference
- Both can independently release when destroyed
- Prevents double-deletion

**RAII on move**: The moved-from object can be safely destroyed:
```cpp
LifetimeObserver obj1;
LifetimeObserver obj2 = std::move(obj1);  // obj1 gets new control block
// Both obj1 and obj2 destructible
```

### Reference Counting Mechanics

#### Adding References

```cpp
int64_t count = control_block->addRef(e_refType::owner);
```

Returns the new count after increment. Used to verify reference was created.

#### Releasing References

```cpp
control_block->releaseRef(e_refType::owner);

// Cleanup: if last reference, delete control block
if (owner_count == 0 && observer_count == 0) {
    delete control_block;
}
```

**Lifetime guarantee**:
- Control block exists as long as any reference exists
- Safe to call `isAlive()` on observer even after original destroyed
- Automatic cleanup when last reference released

### Exception Safety

**Copy assignment with exception safety** (simplified):

```cpp
LifetimeObserver& operator=(const LifetimeObserver& other) {
    if (this != &other) {
        if (control_block != other.control_block) {
            // Old cleanup (may throw, but recoverable)
            control_block->releaseRef(my_ownership);
            // ... if counts = 0, delete control_block

            // New assignment (noexcept)
            control_block = other.control_block;
            control_block->addRef(e_refType::observer);
        }
    }
    return *this;
}
```

**Safety guarantee**: If cleanup throws, object still points to valid control block. Exception propagates.

### Comparison to shared_ptr / enable_shared_from_this

| Feature | `shared_ptr` / `weak_ptr` | `enable_shared_from_this` | `LifetimeObserver` |
|---------|---------------------------|---------------------------|------------------|
| **Purpose** | Shared ownership | Intrusive weak-from-`this` | Lightweight liveness tracking |
| **Thread safety** | Atomic refcounts | Same as `shared_ptr` | Atomic refcounts; object use after `isAlive` still needs sync |
| **Weak view** | `weak_ptr(shared)` | `weak_from_this()` | `getObserver()` |
| **Requires** | External `shared_ptr` | Object already owned by `shared_ptr` | Derive from `LifetimeObserver` |
| **Use case** | Production sharing | Callbacks needing weak `this` with `shared_ptr` | Teaching async checks without full sharing |

`enable_shared_from_this` is the closer cousin: both are intrusive “object exposes a weak self.” Prefer it (or plain `weak_ptr`) when you already use `shared_ptr` and need `lock()`-style upgrade; use `LifetimeObserver` for an intrusive owner/observer split without full shared ownership.

### Learning Value

This implementation teaches:

1. **Observer Pattern**: Loose coupling between object and lifetime observers
2. **Mediator Pattern**: Control block mediates strong/weak reference operations
3. **Reference Counting**: Manual lifetime management without garbage collection
4. **Move Semantics**: Efficient ownership transfer and moved-from object validity
5. **Copy Assignment**: Exception-safe assignment with reference swapping
6. **RAII**: Automatic cleanup through scope (destructor calls release)
7. **Async Safety**: Liveness checks plus an external sync protocol (mutex/strand)
8. **Control Flow**: Checking preconditions before accessing objects; liveness ≠ ownership

### Common Pitfalls

#### 1. Holding Reference to Original, Not Observer

```cpp
// ❌ WRONG - callback holds reference to owner
LifetimeObserver obj;
event_loop.on_event([obs = obj]() {  // Wrong: makes owner copy
    // This prevents obj from being destroyed!
});

// ✅ RIGHT - callback holds weak reference via getObserver()
LifetimeObserver obj;
event_loop.on_event([obs = obj.getObserver()]() {
    // Still must synchronize before using the observed object (see #2)
    if (!obs.isAlive())
        return;
});
```

#### 2. isAlive() ≠ Safe Access (TOCTOU)

`isAlive()` answers “is there still an owner ref?” It does **not** pin the object.
Atomic counts close data races on the control block; they do **not** make
check-then-use of a raw pointer race-free across threads.

```cpp
// ❌ WRONG - object can be destroyed between check and use
if (observer.isAlive()) {
    use_object();  // TOCTOU on another thread
}

// ❌ WRONG - checked once, then used later with no sync
auto observer = obj.getObserver();
// ... later, another thread may destroy obj ...
obj_method();  // CRASH / UAF if destroyed

// ✅ RIGHT - mutex (or strand) also covers destruction
std::lock_guard lock(mutex);
if (observer.isAlive()) {
    use_object();  // Protected
}

// ✅ ALSO RIGHT - single-threaded / same-strand event loop where
// destroy and the callback never overlap (sequencing is the protocol)
```

Prefer `std::weak_ptr::lock()` when you need an ownership upgrade instead of an
external mutex.

#### 3. Forgetting to Check isAlive() at All

```cpp
// ❌ WRONG - no liveness check
auto observer = obj.getObserver();
obj_method();  // UAF if obj was already destroyed on this strand

// ✅ RIGHT - check under your access protocol
std::lock_guard lock(mutex);
if (observer.isAlive()) {
    obj_method();
}
```

---

## Thread Safety and Concurrent Access

### Two-Level Lock Design

The allocator uses a **sophisticated two-level locking strategy** for optimal concurrency:

**Level 1: Pool Lock** - Protects pool-level state
- Guards the slab selection logic
- Protects access to the `small_slabs` vector
- **Released before actual allocation** to minimize contention

**Level 2: Per-Slab Locks** - Each slab has its own SpinLock
- Guards individual slab state (bitsets, availability maps)
- Allows parallel allocations from different slabs
- Independent locks mean no contention between size classes

**Implementation** (`pool.hpp:108-118, 151-160` and `slab.hpp:128, 196, 273`):
```cpp
class Pool
{
private:
    SpinLock lock;  // Pool-level lock

public:
    std::byte* allocate(std::size_t size, std::size_t alignment)
    {
        AbstractSlab* slab = nullptr;
        {
            std::scoped_lock<SpinLock> guard(lock);  // Pool lock
            auto slab_index = selectSlab(alloc_size);
            slab = small_slabs[slab_index].get();
        }  // Pool lock released

        // Slab lock acquired inside allocateItem()
        return slab->allocateItem(alloc_size);
    }
};

template<const std::size_t ElemSize>
class Slab : public AbstractSlab
{
private:
    SpinLock slab_lock;  // Per-slab lock

public:
    std::byte* allocateItem(std::size_t) override
    {
        std::scoped_lock<SpinLock> guard(slab_lock);  // Slab lock
        // ... bitset manipulation, slot allocation ...
    }  // Slab lock released
};
```

### Design Decisions

**Two-Level Lock Benefits**:

✅ **Minimized Pool Lock Contention**:
- Pool lock held only during slab selection (< 10 cycles)
- Released before expensive allocation operations
- Multiple threads can allocate simultaneously from different slabs

✅ **Per-Slab Parallelism**:
- Thread allocating 64B doesn't block thread allocating 256B
- Each size class has independent synchronization
- Scales well with diverse allocation patterns

✅ **Lock Ordering Simplicity**:
- Pool lock → Slab lock (always in this order)
- Locks never held simultaneously (pool released before slab acquired)
- **Zero deadlock risk** by design

✅ **Optimal Critical Sections**:
- Pool critical section: ~5-10 cycles (just slab lookup)
- Slab critical section: ~20-50 cycles (bitset + allocation)
- Total lock hold time minimized

**Performance Characteristics** (order-of-magnitude intuition, not measured benchmarks — profile with `perf` / sanitizer builds for real numbers):

| Scenario | Pool Lock | Slab Lock | Total Time |
|----------|-----------|-----------|------------|
| **No contention** | ~5–10 cycles | ~20–50 cycles | ~25–60 cycles |
| **Different slabs** | Serialized briefly | **Parallel** ✅ | ~same ballpark each |
| **Same slab** | Serialized briefly | Serialized | Contends on one slab |

**Why this design is excellent**:
1. ✅ Allows true parallelism for different size classes
2. ✅ Minimizes pool lock contention (held briefly)
3. ✅ No deadlock possible (locks never held together)
4. ✅ Simple to understand and verify
5. ✅ Scales well with typical workloads

### RAII Lock Management

Uses `std::scoped_lock` (C++17) for exception-safe lock management:
- Lock acquired in constructor
- Automatically released in destructor (even during exceptions)
- Works seamlessly with our `SpinLock` (BasicLockable concept)

**Educational value**: Demonstrates proper RAII patterns for resource management and why manual lock/unlock is error-prone.

### Smart Pointer Thread Safety

**`make_pool_unique`**: Not thread-safe (unique ownership)
- Each `unique_ptr` is owned by a single thread
- Moving between threads requires explicit synchronization
- Deletion happens when `unique_ptr` is destroyed (automatically thread-safe via pool lock)

**`make_pool_shared`**: Thread-safe reference counting
- `std::shared_ptr` uses atomic reference counting internally
- Multiple threads can safely copy and destroy `shared_ptr` instances
- Control block allocation and object deallocation use pool lock
- **Internally thread-safe** via standard library guarantees

### Performance Implications

**Lock Contention Analysis**:

**Best case** (no contention):
- Lock overhead: ~5-10 cycles (TTAS + acquire)
- Allocation: ~20-50 cycles
- Total: ~25-60 cycles

**Moderate contention** (2-4 threads):
- Backoff kicks in: ~100-500ns delays
- Still acceptable for most workloads

**High contention** (>8 threads):
- Escalating backoff and blocking
- Consider per-slab locking or lock-free paths

### Correctness Guarantees

With SpinLock integration:
- ✅ **Mutual Exclusion**: Only one thread modifies pool state at a time
- ✅ **Progress**: Eventually all threads make progress (no deadlock)
- ✅ **Memory Safety**: No data races on pool internals
- ✅ **Exception Safety**: Locks always released via RAII

### Testing Thread Safety

**ThreadSanitizer validation**:
```bash
make BUILD_TYPE=threadsan test
# or: ./obj/threadsan/tester
```

ThreadSanitizer confirms:
- No data races in pool operations
- Proper synchronization through SpinLock
- Correct happens-before relationships

See `spallocator/docs/SpinLock_Analysis.md` for detailed correctness analysis.

---

## SpinLock Design

### Test-and-Test-and-Set (TTAS)

TTAS is an optimization over naive test-and-set that dramatically reduces cache coherency traffic.

**Naive test-and-set:**
```cpp
void lock() {
    while (flag.test_and_set(memory_order_acquire)) {
        // busy wait
    }
}
```

Problem: Every thread continuously performs atomic read-modify-write operations, causing cache line to ping-pong between cores.

**TTAS version:**
```cpp
void lock() {
    while (flag.test(memory_order_relaxed)) {  // Read-only
        yield();
    }
    // Now try to acquire
    if (!flag.test_and_set(memory_order_acquire)) {
        return;  // Success
    }
    // Otherwise retry
}
```

Benefit: Threads mostly perform read-only operations (which can be cached), only attempting the expensive atomic write when lock appears free.

### Escalating Backoff

The backoff strategy prevents the "thundering herd" problem.

**The Problem:**
Without backoff, when a lock is released, all waiting threads immediately try to acquire it, causing:
- Cache line contention
- Wasted CPU cycles
- Unfair lock acquisition (same thread might keep winning)

**The Solution:**
```cpp
std::chrono::nanoseconds wait_time{dist(gen)};  // Random 1-100ns

for (std::size_t i = 0; i < 10; ++i) {
    // Try to acquire lock
    if (!flag.test_and_set(memory_order_acquire)) {
        return;  // Success
    }

    // Escalate wait time
    wait_time += wait_time;  // Double via addition

    std::this_thread::sleep_for(wait_time);
}

// After 10 iterations, block efficiently
flag.wait(true, memory_order_acquire);
```

**Doubling backoff** (`wait_time += wait_time`, same growth as `*= 2`):
- Iteration 1: random 1–100ns (OS timers often coarsen the smallest sleeps)
- Iteration 2: 2–200ns … Iteration 10 last interval: 512–51200ns (~51μs peak interval)
- Cumulative sleeps across 10 attempts ≈ `W·(2¹⁰−1)` (up to ~102μs for `W=100ns`), then `wait()`

The important property is **differentiation across threads**, not literal nanosecond accuracy. Cap the wait if you need a hard latency bound.

### Memory Ordering Semantics

The spinlock uses three memory orderings:

**1. Relaxed (`memory_order_relaxed`)**
```cpp
flag.test(memory_order_relaxed)
```
- No synchronization guarantees
- Cheapest operation
- Safe here because we're just checking state, not relying on synchronization

**2. Acquire (`memory_order_acquire`)**
```cpp
flag.test_and_set(memory_order_acquire)
```
- Ensures all subsequent reads/writes happen after this operation
- Prevents reordering of critical section code before lock acquisition
- Essential for correctness

**3. Release (`memory_order_release`)**
```cpp
flag.clear(memory_order_release)
```
- Ensures all prior reads/writes complete before this operation
- Makes critical section changes visible to other threads
- Paired with acquire to form happens-before relationship

**The Synchronization Contract:**
```
Thread A:                          Thread B:
lock() [acquire]
  x = 42;
  y = 99;
unlock() [release]
  ↓                                  ↓
  happens-before                     ↓
  ↓                                  ↓
                                   lock() [acquire]
                                     assert(x == 42);  // Guaranteed
                                     assert(y == 99);  // Guaranteed
                                   unlock() [release]
```

Without proper memory ordering, the compiler or CPU could reorder operations, breaking this guarantee.

### ThreadSanitizer annotations

Custom locks are invisible to TSan unless you publish happens-before edges. `spinlock.hpp` does that with:

```cpp
// after successful acquire
TSAN_ANNOTATE_HAPPENS_AFTER(this);   // → __tsan_acquire

// before clear(release) in unlock
TSAN_ANNOTATE_HAPPENS_BEFORE(this);  // → __tsan_release
```

Without these, TSan may report false races on data protected only by `SpinLock`. The macros are no-ops unless `TSAN_ENABLED` is set (today via Clang `__has_feature(thread_sanitizer)`). Full correctness write-up: [`spallocator/docs/SpinLock_Analysis.md`](spallocator/docs/SpinLock_Analysis.md).

---

## Performance Analysis

### Time Complexity

| Operation | Best Case | Worst Case | Amortized | Notes |
|-----------|-----------|------------|-----------|-------|
| Allocation | O(1) | O(n) | O(1) | n = slots per slab |
| Deallocation | O(log m) | O(log m) | O(log m) | m = number of slabs |
| Slab Selection | O(1) | O(1) | O(1) | Ladder of compares (`constexpr`-friendly API) |
| SpinLock lock() (no contention) | O(1) | O(1) | O(1) | Single atomic |
| SpinLock lock() (contention) | O(k) | O(k) | O(k) | k = backoff iterations (max 10) |
| SpinLock try_lock() | O(1) | O(1) | O(1) | Non-blocking |
| SpinLock unlock() | O(1) | O(1) | O(1) | Single atomic + notify |

### Why "Amortized O(1)" for Allocation?

The claim requires careful explanation:

**Best case**: Free slot in first available slab → O(1)

**Worst case**: Must scan all slots in a slab to find a free one → O(slots_per_slab). The implementation scans **from bit 0 upward** with no freelist cursor, so a single free bit at the end of a busy slab costs a full scan of that slab’s bitset.

**Real-world behavior**: The availability map provides coarse filtering, so we only scan slabs that are known to have free slots. Bitset ops are cheap on modern CPUs, so the educational “scan from zero” choice stays practical for 4KB slabs.

**Scan-from-zero vs free list** (see also Future Enhancements):
- **Bitset scan** — compact metadata, simple correctness, worse-case linear in slots per slab
- **Free list / bump cursor** — O(1) alloc when a free slot is known, extra storage and more complex free/coalesce logic

This project favors bitsets for clarity; a freelist would be the usual next optimization if profiling showed scan cost dominating.

**Why it works in practice:**
1. Temporal locality: Allocations and deallocations tend to cluster
2. Availability map: Skips completely full slabs
3. Hardware-friendly bitset tests

### Space Complexity

| Component | Overhead | Per-Allocation Impact |
|-----------|----------|----------------------|
| Allocation header | `max(8, alignment)` bytes (default 8) | Fixed per allocation |
| Slab map bitset | 1 bit per slot | Amortized across all slots in slab |
| Availability map (`DynBitset`) | 1 bit per live slab (uint64 words) | Grows with slab count; not fixed 128 KiB/class |
| Address map | ~24 bytes per slab | Amortized across all slots in slab |

**Growth vs reclaim (RSS):** Size-class slabs grow on demand and **retain empty slabs by default** (monotonic growth) so alloc/free does not mmap/munmap thrash. `Pool::trim()` / `Slab::reclaimEmptySlabs(keep_min=1)` delete fully empty slab buffers (swap-with-last, keep ≥1 slab per class) for explicit RSS recovery at quiet points. `SlabProxy` large allocations already return memory on each free. `Pool::getAllocatedMemory()` sums retained size-class buffers.

**Example Calculation** (128-byte **user** request, default alignment → 136-byte footprint → 192B class):
- 4KB slab of 192B slots holds 21 slots (4096 / 192)
- Per-allocation header: 8 bytes on a 128-byte request ≈ 6.25% before bitsets / class waste
- Additional internal fragmentation: 192 − 136 = 56 bytes of unused slot capacity

For smaller user requests the header percentage is higher (e.g. N=16 → 24-byte footprint in a 32B class), but this is acceptable because:
1. Small allocations are typically short-lived
2. The bitset overhead is shared across all allocations in the slab
3. Alternative approaches (free lists) would have similar or worse overhead

### Fragmentation Analysis

**Internal Fragmentation**: Wasted space within allocated slots after the header is included in the footprint.

Example: user requests 100 bytes → footprint 108 → 128B class → 20 bytes unused in the slot (~16% of the slot). Comparing 100 to 128 without the header understates how `selectSlab` actually chooses.

Mitigation: Multiple size classes reduce waste. Intermediate sizes (48, 96, 192, 384, 768) are specifically chosen to minimize gaps.

Worst case: Just over 50% in the gap between size classes (e.g., footprint 129 → 192B class).

**External Fragmentation**: Inability to satisfy large allocations despite free memory

Not applicable to fixed-size allocations within slabs. Each size class operates independently.

Slab-level consideration: Partially occupied slabs cannot be released; fully empty slabs remain until `trim()`/`reclaimEmptySlabs()`, then memory returns to the OS.

**Key Insight**: This is a fundamental advantage of slab allocators - they eliminate external fragmentation at the cost of some internal fragmentation. The size class selection is critical: more classes reduce internal fragmentation but increase metadata overhead and complexity.

---

## Advanced Topics

### When to Use Slab Allocation

Slab allocators excel in scenarios with:
- **Frequent allocation/deallocation**: Reduces overhead vs. general allocators
- **Known size patterns**: Size classes can be optimized for workload
- **Temporal locality**: Recently freed objects are hot in cache
- **Reduced fragmentation**: Fixed sizes prevent external fragmentation

### When NOT to Use Slab Allocation

- **Highly variable allocation sizes**: Wastes memory in internal fragmentation
- **Infrequent allocations**: Overhead of pre-allocation not amortized
- **Extremely large objects**: Pooling doesn't help (use SlabProxy/standard allocator)
- **Unknown or dynamic size requirements**: Can't pre-optimize size classes

### Comparison to Other Allocators

| Allocator Type | Strengths | Weaknesses | Best Use Cases |
|----------------|-----------|------------|----------------|
| **Slab** (this project) | Fast, low fragmentation, cache-friendly | Internal fragmentation, size limits | Kernel object caching, game engines, network buffers |
| **Buddy** | Flexible sizes, coalescing | External fragmentation, complexity | Operating system page allocation |
| **Free List** | Simple, general-purpose | Can be slow, fragmentation | Small embedded systems, simple allocators |
| **TCMalloc/jemalloc** | Excellent general performance | Complex, large codebase | Production applications, general purpose |
| **Arena** | Ultra-fast, simple | No individual deallocation | Temporary computations, parsers |

### Connection to Real-World Systems

This allocator demonstrates concepts used in:

**Operating Systems:**
- **Linux SLAB/SLUB/SLOB**: Kernel object caching (inodes, dentries, network buffers)
- **Solaris Slab Allocator**: Original Bonwick design (1994)
- **FreeBSD UMA**: Unified Memory Allocator
- **Windows Lookaside Lists**: Fast path for common kernel allocations

**User-Space Applications:**
- **memcached**: Distributed memory caching system (popularized slab allocation for user-space)
- **Redis**: Uses slab-like allocation for certain data structures
- **Nginx**: Memory pools for per-request allocations
- **Game Engines**: Object pools for entities, particles, etc.

**Historical Context**:

While Jeff Bonwick introduced the slab allocator concept for the Solaris kernel in 1994, **memcached** (created by Brad Fitzpatrick in 2003) brought widespread attention to slab allocation in user-space applications. memcached's use of slab allocation for caching frequently-accessed objects demonstrated the pattern's effectiveness beyond kernel memory management, inspiring countless implementations in web applications, databases, and high-performance systems.

---

## Learning Objectives

This codebase teaches several important concepts across multiple domains:

### 1. Template Metaprogramming

- **Compile-time computation**: Using `constexpr` to move work to compile time
- **Template specialization**: Different behavior for different sizes
- **Type-level programming**: Templates as a functional programming language
- **Zero-cost abstraction**: Template code that compiles to optimal machine code

**Example from codebase:**
```cpp
template<const std::size_t ElemSize>
static constexpr std::size_t slab_alloc_size =
    (ElemSize < 1_KB) ? 4_KB : (ElemSize * 4);
```

This computes the slab size at compile time, with zero runtime overhead.

### 2. Modern C++ Features (C++20/23)

- **User-defined literals**: Domain-specific syntax (`4_KB`)
- **std::format**: Type-safe formatting
- **std::source_location**: Automatic debugging context
- **std::optional**: Explicit handling of optional values
- **std::atomic_flag::wait()/notify_one()**: Efficient blocking primitives (C++20)
- **thread_local**: Thread-specific storage

### 3. Memory Management Patterns

- **Slab allocation**: Fixed-size pooling strategy
- **Free list vs. bitset**: Different tracking approaches
- **Internal vs. external fragmentation**: Trade-offs in allocator design
- **RAII**: Resource acquisition is initialization
- **Metadata strategies**: Headers, alignment, lookup tables

### 4. Data Structure Design

- **Bitsets**: Compact state tracking
- **Two-level indexing**: Hierarchical optimization
- **Interval lookup**: Address-to-slab mapping
- **Virtual interfaces**: Polymorphism for flexibility

### 5. Performance Optimization

- **Cache-friendly layout**: Minimize cache misses
- **Amortized constant time**: Average-case optimization
- **Space-time trade-offs**: Bitsets vs. free lists
- **Compile-time optimization**: `constexpr` and templates

### 6. Concurrency and Synchronization

- **Lock-free operations**: Using atomics correctly
- **Memory ordering**: Acquire/release semantics
- **TTAS**: Reducing cache coherency traffic
- **Backoff strategies**: Avoiding thundering herd
- **STL compatibility**: BasicLockable concept

### 7. Asynchronous Patterns and Lifetime Management

- **Observer pattern**: Decoupling object lifetime from observers
- **Mediator pattern**: Control blocks coordinating strong/weak references
- **Reference counting**: Manual lifetime without garbage collection
- **Move semantics**: Efficient ownership transfer with valid moved-from state
- **Exception safety**: Assignment and cleanup under error conditions
- **Async callback patterns**: Safe invocation of callbacks on possibly-destroyed objects

### 8. Software Engineering Practices

- **Separation of concerns**: Clear module boundaries
- **Resource management**: Non-copyable/non-moveable semantics
- **Comprehensive testing**: Unit tests, stress tests, concurrent tests
- **Observability**: Debug output for understanding behavior
- **Documentation**: Explaining "why" not just "what"

---

## Further Reading

### Academic Papers

- **Bonwick, Jeff (1994)**. ["The Slab Allocator: An Object-Caching Kernel Memory Allocator"](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps) - Original USENIX paper
- **Berger et al. (2000)**. "Hoard: A Scalable Memory Allocator for Multithreaded Applications" - Influential user-space allocator
- **Evans, Jason (2006)**. "A Scalable Concurrent malloc(3) Implementation for FreeBSD" - jemalloc paper

### Books

- **"C++ Concurrency in Action" by Anthony Williams** - Comprehensive coverage of C++ threading and atomics
- **"The Art of Multiprocessor Programming" by Herlihy & Shavit** - Theory and practice of concurrent data structures
- **"Modern C++ Design" by Andrei Alexandrescu** - Template metaprogramming techniques

### Online Resources

**Concepts:**
- [Slab Allocation](https://en.wikipedia.org/wiki/Slab_allocation)
- [Memory Pool](https://en.wikipedia.org/wiki/Memory_pool)
- [Buddy Memory Allocation](https://en.wikipedia.org/wiki/Buddy_memory_allocation)

**Real-World Implementations:**
- [Linux SLAB/SLUB](https://www.kernel.org/doc/gorman/html/understand/understand011.html)
- [jemalloc](https://github.com/jemalloc/jemalloc)
- [TCMalloc](https://github.com/google/tcmalloc)
- [mimalloc](https://github.com/microsoft/mimalloc)
- [memcached](https://memcached.org/)

**C++ Standards:**
- [C++20 Features](https://en.cppreference.com/w/cpp/20)
- [C++23 Features](https://en.cppreference.com/w/cpp/23)
- [Memory Ordering](https://en.cppreference.com/w/cpp/atomic/memory_order)
- [BasicLockable](https://en.cppreference.com/w/cpp/named_req/BasicLockable)
- [Allocator Requirements](https://en.cppreference.com/w/cpp/named_req/Allocator)

---

## Debugging and Instrumentation

### Observability & builds cheat sheet

| Knob | Effect |
|------|--------|
| `NDEBUG` / `DEBUG_BUILD` | Without `NDEBUG`, `runtime_assert` is active; with `NDEBUG`, asserts are compile-time no-ops |
| `-DVDEBUG` / `VERBOSE_DEBUG` | Enables `debug_print` / `debug_println` (slab alloc/free, unknown-pointer notes, etc.) |
| `BUILD_TYPE=debug` | Unoptimized debug binary under `obj/debug/` |
| `BUILD_TYPE=addrsan` | **Default** — ASan + LSan + UBSan (`obj/addrsan/`) |
| `BUILD_TYPE=memsan` | MemorySanitizer (Clang; `obj/memsan/`) — valid locally when the host provides an MSan-instrumented C++ runtime/gtest; omitted from GitHub CI |
| `BUILD_TYPE=threadsan` | ThreadSanitizer + UBSan (`obj/threadsan/`) — exercise `SpinLock` races; `-fsanitize=thread` is implied by this build type |
| `BUILD_TYPE=release` | `-O3 -DNDEBUG` (`obj/release/`) — covered in CI; coverage (`--coverage`) is on by default for this type (`make coverage`) |
| `make test` / `make demo` | Run tester or shared_ptr demo for current build type |

SpinLock happens-before design and TSan annotation details: [`spallocator/docs/SpinLock_Analysis.md`](spallocator/docs/SpinLock_Analysis.md).

**Typical workflow:**
```bash
make BUILD_TYPE=addrsan test          # memory bugs
make BUILD_TYPE=threadsan test        # data races / lock misuse
# rebuild with -DVDEBUG for verbose pool/slab traces
```

**What's logged** (when verbose debug is on): slab create/destroy, alloc/free addresses, bitset snapshots, slab selection, unknown-pointer misses in `findSlabForItem`.

**Educational value:** Instrument low-level code with `if constexpr` gates so release builds pay zero cost for debug paths.

---

## Future Enhancements

### Planned Features

**Enhanced Smart Pointer Support**
- ✅ `std::unique_ptr` support via `make_pool_unique` (COMPLETED)
- ✅ Custom deleter with array specialization (COMPLETED)
- ✅ `std::shared_ptr` support via `make_pool_shared` (COMPLETED)
- ✅ `PoolAllocator<T>` for STL integration (COMPLETED)
- ✅ Object lifetime observer for async callbacks (COMPLETED)
- Educational value: Integrating custom allocators with STL, safe async patterns

**Thread-Safe Pool**
- ✅ SpinLock integration with Pool for concurrent allocations (COMPLETED)
- ✅ Two-level locking strategy: pool + per-slab locks (COMPLETED)
- ✅ Parallel allocations from different size classes (COMPLETED)
- ✅ Zero-deadlock design through lock ordering (COMPLETED)
- Lock-free allocation paths for common cases (future)
- Educational value: Concurrent data structure design, lock granularity trade-offs, and deadlock prevention

**Statistics and Profiling**
- Allocation count, peak usage, fragmentation ratio
- Per-size-class metrics
- Real-time monitoring capabilities
- Educational value: Performance analysis techniques

**Memory Alignment**
- ✅ Power-of-two alignments 4…64 via `allocate(size, alignment)` (COMPLETED; see [Memory Layout](#memory-layout))
- ✅ Compound-key `Slab<ElemSize, Alignment>` ladders (16 eager; 32/64 lazy); `SlabProxy` always 64-align
- Educational value: Hardware-aware programming

**STL Allocator Interface**
- ✅ C++ standard library allocator compatibility via `PoolAllocator<T>` (COMPLETED)
- ✅ Use with `std::vector`, `std::list`, `std::map`, etc. (COMPLETED)
- ✅ Proper rebind support for container node allocations (COMPLETED)
- Educational value: Allocator requirements, concepts, and stateful allocator design

### Optimization Opportunities

**Custom Bitset Implementation**
- 64-bit atomic operations for lock-free slot allocation
- SIMD-based scanning for free slots
- Learning: Atomics, vectorization, lock-free programming

**Free Lists as Alternative**
- Compare performance vs. bitsets
- Trade-offs: O(1) guaranteed vs. memory overhead
- Learning: Different allocation tracking strategies

**Thread-Local Caching**
- Small per-thread cache of pre-allocated objects
- Reduces contention on shared data structures
- Learning: Thread-local storage, cache locality

**NUMA Awareness**
- Allocate from local memory node when possible
- Platform-specific optimizations
- Learning: Hardware architecture impact on software design

---

## Demo Programs

Tests live under `test/`; demos under `demos/`. Default `BUILD_TYPE` is `addrsan`.

### 1. `test/tester.cpp` - Comprehensive Unit Tests

The main test suite covering:
- **Core Slab Operations**: Allocation, deallocation, growth
- **Pool Size Class Selection**: Correct slab assignment for various sizes
- **Thread Safety**: Concurrent allocations from multiple threads
- **Smart Pointers**: `unique_ptr` and `shared_ptr` lifecycle management
- **Array Support**: Proper reverse-order destruction for arrays
- **Destructor Invocation**: Ensuring user-defined destructors are called
- **PoolAllocator Integration**: STL container compatibility
- **SpinLock Contention**: High-contention scenarios with backoff testing

**Build and run:**
```bash
make
make test
# or: ./obj/addrsan/tester
```

**Sanitizer builds:**
```bash
# AddressSanitizer + LSan + UBSan (default)
make BUILD_TYPE=addrsan test

# MemorySanitizer + UBSan (Clang)
make BUILD_TYPE=memsan test

# ThreadSanitizer (for race condition detection)
make BUILD_TYPE=threadsan test
```

### 2. `demos/demo_spalloc_shared_ptr.cpp` - Shared Pointer Usage

Demonstrates `make_pool_shared` and `PoolAllocator<T>`:

```cpp
// Create shared objects from pool
auto obj = spallocator::make_pool_shared<MyClass>(pool, arg1, arg2);
auto copy = obj;  // Reference counting

// Use with STL containers
std::vector<MyClass, spallocator::PoolAllocator<MyClass>> vec(pool);
vec.push_back(MyClass());

// Array support
auto arr = spallocator::make_pool_shared<int[]>(pool, 100);
```

**Highlights:**
- Single allocation for both object and control block
- Automatic reference counting
- Works seamlessly with `weak_ptr`
- STL container integration

**Build and run:**
```bash
make demo
# or: ./obj/addrsan/demo_spalloc_shared_ptr
```

### 3. `demos/demo_lifetime_observer.cpp` - Object Lifetime Tracking

Demonstrates both the common single-strand callback pattern and the
cross-thread mutex pattern when destroy can race with use:

```cpp
// Example 3 — single event thread (e.g. Asio): sequencing is enough
engine.on_event([&obj, alive = obj->getObserver()]() {
    if (alive)
        obj->do_work();
});

// Example 4 — cross-thread: external mutex covers check/use and delete
engine.on_event([&mu, &obj, alive = obj->getObserver()]() {
    std::lock_guard lock(mu);
    if (alive)
        obj->do_work();
});
{
    std::lock_guard lock(mu);
    delete obj;
}
```

**Key Examples:**
1. **Basic Ownership Tracking** - Creating and destroying owner references
2. **Weak References** - Observers that don't prevent object destruction
3. **Single-strand Async Callbacks** - Asio-style one event thread; no mutex required
4. **Cross-thread Callbacks** - Mutex-guarded check-then-use (avoids TOCTOU)
5. **Multiple Observers** - Many observers tracking a single object
6. **Stack-Based Objects** - RAII with automatic lifetime management

**Features Demonstrated:**
- Creating weak observer references via `getObserver()`
- Race-free liveness checking via `isAlive()` / `operator bool()`
- Single-strand vs cross-thread access protocols
- Reference counting mechanics (owner vs. observer counts)
- Move semantics and moved-from object validity
- Control block survival after original object destruction

**Build and run:**
```bash
make
./obj/addrsan/demo_lifetime_observer
```

### Running All Demos

Quick verification that everything works:
```bash
make clean
make
make test
./obj/addrsan/demo_spalloc_shared_ptr
./obj/addrsan/demo_lifetime_observer
```

### Educational Value

These demo programs teach:
- **Integration**: How components work together
- **Patterns**: Real-world usage patterns and idioms
- **Safety**: Correct usage of smart pointers and lifetime tracking
- **Performance**: When and how to use each feature
- **Debugging**: Output traces showing internal behavior

---

*For project overview and quick start, see [README.md](README.md)*
