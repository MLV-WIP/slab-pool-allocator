# Implementation Guide

This document provides an in-depth analysis of the slab pool allocator implementation, design decisions, and educational insights. For a quick overview, see [README.md](README.md).

**Educational Focus**: This guide is designed to teach advanced C++ techniques, memory management concepts, and concurrent programming patterns through detailed explanations and real-world examples.

---

## Table of Contents

- [Architecture Deep Dive](#architecture-deep-dive)
- [Memory Layout](#memory-layout)
- [Slab Allocation Strategy](#slab-allocation-strategy)
- [Bitset Tracking System](#bitset-tracking-system)
- [Address Lookup for Deallocation](#address-lookup-for-deallocation)
- [Smart Pointer Integration](#smart-pointer-integration)
- [SpinLock Design](#spinlock-design)
- [Performance Analysis](#performance-analysis)
- [Advanced Topics](#advanced-topics)
- [Learning Objectives](#learning-objectives)

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
- **Strategy Pattern**: Selects appropriate slab based on allocation size
- **Memory Metadata**: Stores allocation size in 4-byte header for transparent deallocation
- **Size Class Optimization**: Pre-defined size classes reduce fragmentation

```cpp
constexpr std::size_t Pool::selectSlab(std::size_t size) const
```

**Design Insights**:
- Size classes chosen based on common allocation patterns (powers of 2, with intermediate steps)
- Allocation header stores size for O(1) slab lookup during deallocation
- Non-copyable and non-moveable (proper resource management semantics)

**Educational Highlights**:

**Allocation metadata trick**: The 4-byte prefix stores allocation size, allowing `deallocate()` to work without a size parameter. This matches the standard `delete` interface and simplifies the API. The size is masked to 16 bits (`alloc_size & 0xFFFF`), limiting individual allocations to 64KB, which is acceptable given the 1KB pool limit.

**Size class selection**: The ladder of sizes (16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024) balances fragmentation vs. number of slabs. Intermediate sizes (48, 96, 192, 384, 768) significantly reduce waste. For example:
- 100-byte allocation in 128-byte slot: 28 bytes wasted (22%)
- 100-byte allocation in 256-byte slot: 156 bytes wasted (61%)

**Constexpr function**: `selectSlab()` can be evaluated at compile-time when possible, enabling zero-overhead abstraction for constant-size allocations.

---

### 3. SlabProxy (`spallocator/slab.hpp`)

Handles large allocations by delegating to standard allocators.

**Key Concepts Demonstrated**:
- **Adapter Pattern**: Provides uniform interface while delegating to different backend
- **Separation of Concerns**: Large allocations have different performance characteristics

**Design Insights**:
- Allocations > 1 KB are typically too large to benefit from pooling
- Inherits from `AbstractSlab` to maintain uniform interface
- No internal state needed - pure delegation

**Educational Highlights**:

**When NOT to pool**: Large allocations are infrequent and don't benefit from pre-allocation overhead. The bookkeeping cost of maintaining a pool for large objects often exceeds the allocation cost itself. For such allocations, deferring to the system allocator (which is already optimized for varied sizes) is the right choice.

**Interface uniformity**: Same `allocateItem()`/`deallocateItem()` API regardless of backend. This demonstrates the power of polymorphism - the Pool class doesn't need to know which strategy is used.

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

**Runtime assertions**: Unlike `assert()`, these work in release builds and provide rich context. Production systems need verification even in optimized builds, making runtime assertions essential for catching bugs early.

**Modern formatting**: `std::format` is type-safe unlike `printf`, and more efficient than streams. It combines the best of both worlds: compile-time format string checking with runtime efficiency.

---

## Memory Layout

Each allocation includes a 4-byte header storing the allocation size, enabling the pool to determine which slab to use during deallocation:

```
Memory Layout:
┌─────────────────┬────────────────────────────────┐
│  4-byte header  │   Requested memory (N bytes)   │
│  (alloc size)   │                                │
└─────────────────┴────────────────────────────────┘
^                 ^
│                 └─ User pointer (returned)
└─ Internal pointer (stored in slab)
```

**Why this design?**

The 4-byte overhead is minimal (~3% for 128-byte allocation), and eliminates the need to pass size to `deallocate()`. This matches the standard `delete` interface and prevents errors from size mismatches.

Alternative approaches:
1. **Require size in deallocate()**: Error-prone, easy to pass wrong size
2. **Use pointer alignment tricks**: Limits allocator flexibility, harder to debug
3. **Separate size table**: Extra indirection, more memory overhead for large numbers of allocations

The header approach is simple, efficient, and robust.

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
std::bitset<max_slabs> slab_available_map;
```

- **Size**: One bit per slab
- **Semantics**: 1 = has free slots, 0 = completely full
- **Usage**: Fast filtering to skip full slabs

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
3. Verify pointer is within [slab_start, slab_end) range
4. Return slab index

### Why This Works

The map is keyed by slab start addresses. For any valid pointer:
- It must fall within some slab's range [start, start+size)
- `upper_bound(ptr)` finds the next slab after ptr
- Backing up one entry finds the slab that could contain ptr
- Final bounds check confirms it's actually in that slab

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

Handling arrays requires special care: destructors must run in **reverse order** (LIFO).

```cpp
template<typename T>
struct PoolDeleter<T[]> {
    spallocator::Pool& pool;

    void operator()(T* p) const {
        if (p) {
            // Read size from header (stored before array)
            std::byte* header_ptr = reinterpret_cast<std::byte*>(p) - sizeof(std::size_t);
            std::size_t size = *reinterpret_cast<std::size_t*>(header_ptr);

            // Call destructors in reverse order using ranges
            std::ranges::for_each(
                std::views::counted(p, size) | std::views::reverse,
                [](T& elem){ elem.~T(); }
            );

            // Deallocate from header start
            pool.deallocate(header_ptr);
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

**Size header trick**: Arrays store their size in a header immediately before the array data. This enables the deleter to know how many destructors to call without external metadata.

Memory layout for arrays:
```
┌─────────────┬────────────────────────────────┐
│ std::size_t │   Array elements (N × sizeof(T)) │
│   (count)   │                                │
└─────────────┴────────────────────────────────┘
^             ^
│             └─ User pointer (returned to caller)
└─ Header pointer (used for deallocation)
```

**Alignment considerations**: The header size is included in the alignment calculation to ensure the array itself is properly aligned:
```cpp
std::size_t alignment = alignof(ElementType) > alignof(std::size_t) ?
                        alignof(ElementType) : alignof(std::size_t);
```

This ensures both the header can store `std::size_t` safely AND the array meets its type's alignment requirements.

#### 3. make_pool_unique - Factory Functions

**Single Object Version**:
```cpp
template<typename T, typename... Args>
    requires (!std::is_array_v<T>)
constexpr unique_pool_ptr<T> make_pool_unique(spallocator::Pool& pool, Args&&... args)
{
    void* mem = pool.allocate(sizeof(T), alignof(T));
    T* obj = new (mem) T(std::forward<Args>(args)...);
    return unique_pool_ptr<T>(obj, PoolDeleter<T>{pool});
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

**Alignment specification**: Passing `alignof(T)` ensures properly aligned memory for the type. Misaligned access can cause crashes on some architectures (e.g., ARM) or performance degradation on others (e.g., x86).

**Array Version**:
```cpp
template<typename T>
    requires std::is_unbounded_array_v<T>
constexpr unique_pool_ptr<T> make_pool_unique(spallocator::Pool& pool, std::size_t size)
{
    using ElementType = std::remove_extent_t<T>;

    // Allocate header + array
    std::size_t header_size = sizeof(std::size_t);
    std::size_t array_size = sizeof(ElementType) * size;
    std::size_t alignment = alignof(ElementType) > alignof(std::size_t) ?
                            alignof(ElementType) : alignof(std::size_t);

    std::byte* mem = pool.allocate(header_size + array_size, alignment);

    // Store size in header
    *reinterpret_cast<std::size_t*>(mem) = size;

    // Construct elements
    ElementType* array_ptr = reinterpret_cast<ElementType*>(mem + header_size);
    for (std::size_t i = 0; i < size; ++i) {
        new (&array_ptr[i]) ElementType();
    }

    return unique_pool_ptr<T>(array_ptr, PoolDeleter<T>{pool});
}
```

**Educational Highlights**:

**Type extraction**: `std::remove_extent_t<T>` extracts element type from array type:
- Input: `int[]` → Output: `int`
- Input: `MyClass[]` → Output: `MyClass`

**Default initialization**: Each array element is default-constructed with `ElementType()`. For types with non-trivial constructors, this ensures proper initialization. For primitive types (int, float), this zero-initializes.

**Exception safety consideration**: If construction of element N throws, elements 0..N-1 need cleanup. This implementation doesn't handle that (educational simplification). Production code would use a guard:
```cpp
std::size_t constructed = 0;
try {
    for (std::size_t i = 0; i < size; ++i) {
        new (&array_ptr[i]) ElementType();
        ++constructed;
    }
} catch (...) {
    // Destroy already-constructed elements in reverse
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
| **Array support** | Yes (`T[]`) | Yes, with size header for reverse destruction |
| **Exception safety** | Full | Partial (educational implementation) |
| **Alignment** | Standard alignment | Explicit alignment specification |
| **Performance** | General-purpose | Optimized for specific size classes |

### Why Not std::shared_ptr?

`std::shared_ptr` requires different handling due to reference counting and the control block. Future work may include:

```cpp
template<typename T, typename... Args>
std::shared_ptr<T> make_pool_shared(Pool& pool, Args&&... args);
```

Challenges:
- Control block allocation (also needs pool allocation)
- Weak pointer support
- Thread-safe reference counting
- Deleter must outlive control block

This is more complex and left for future enhancement.

### Learning Value

This implementation teaches:

1. **Custom deleters**: How to extend standard library smart pointers
2. **Template specialization**: Providing different behavior for related types
3. **Concepts and constraints**: Modern C++ type restrictions with clear errors
4. **Perfect forwarding**: Preserving value categories through forwarding references
5. **Placement new/explicit destructors**: Manual object lifetime management
6. **C++20 ranges**: Composable, lazy view transformations
7. **RAII principles**: Automatic resource management through scope
8. **Type traits**: Compile-time type introspection and manipulation
9. **Memory alignment**: Hardware requirements for safe memory access
10. **Exception safety**: Resource leak prevention during unwinding (concept, if not fully implemented)

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

**Why additive doubling instead of exponential?**
- Iteration 1: random 1-100ns
- Iteration 2: 2-200ns
- Iteration 3: 4-400ns
- Iteration 10: 512-51200ns (max ~51μs)

True exponential (2^n) would reach milliseconds quickly, adding unnecessary latency. Additive doubling (`wait += wait`) provides enough differentiation without excessive delays.

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

---

## Performance Analysis

### Time Complexity

| Operation | Best Case | Worst Case | Amortized | Notes |
|-----------|-----------|------------|-----------|-------|
| Allocation | O(1) | O(n) | O(1) | n = slots per slab |
| Deallocation | O(log m) | O(log m) | O(log m) | m = number of slabs |
| Slab Selection | O(1) | O(1) | O(1) | Compile-time lookup |
| SpinLock lock() (no contention) | O(1) | O(1) | O(1) | Single atomic |
| SpinLock lock() (contention) | O(k) | O(k) | O(k) | k = backoff iterations (max 10) |
| SpinLock try_lock() | O(1) | O(1) | O(1) | Non-blocking |
| SpinLock unlock() | O(1) | O(1) | O(1) | Single atomic + notify |

### Why "Amortized O(1)" for Allocation?

The claim requires careful explanation:

**Best case**: Free slot in first available slab → O(1)

**Worst case**: Must scan all slots in a slab to find a free one → O(slots_per_slab)

**Real-world behavior**: The availability map provides coarse filtering, so we only scan slabs that are known to have free slots. Bitset operations are extremely fast (hardware-supported), making even the worst case quite fast in practice.

**Why it works in practice:**
1. Temporal locality: Allocations and deallocations tend to cluster
2. Availability map: Skips completely full slabs
3. Hardware optimization: Bitset operations are highly optimized (often using SIMD)

### Space Complexity

| Component | Overhead | Per-Allocation Impact |
|-----------|----------|----------------------|
| Allocation header | 4 bytes | Fixed per allocation |
| Slab map bitset | 1 bit per slot | Amortized across all slots in slab |
| Availability map | 1 bit per slab | Amortized across all slabs |
| Address map | ~24 bytes per slab | Amortized across all slots in slab |

**Example Calculation** (for 128-byte allocations):
- 4KB slab holds 32 slots
- Per-allocation overhead: 4 bytes (header) + 1/32 bit (slab map) ≈ 4.004 bytes
- Overhead percentage: 4.004 / 128 ≈ 3.1%

For smaller allocations, the percentage is higher (25% for 16-byte allocations), but this is acceptable because:
1. Small allocations are typically short-lived
2. The bitset overhead is shared across all allocations in the slab
3. Alternative approaches (free lists) would have similar or worse overhead

### Fragmentation Analysis

**Internal Fragmentation**: Wasted space within allocated slots

Example: 100-byte allocation in 128-byte slot wastes 28 bytes (22%)

Mitigation: Multiple size classes reduce waste. Intermediate sizes (48, 96, 192, 384, 768) are specifically chosen to minimize gaps.

Worst case: Just over 50% in the gap between size classes (e.g., 129-byte allocation in 192-byte slot).

**External Fragmentation**: Inability to satisfy large allocations despite free memory

Not applicable to fixed-size allocations within slabs. Each size class operates independently.

Slab-level consideration: Fragmented slabs may prevent slab deallocation, but memory remains usable for that size class.

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

### 7. Software Engineering Practices

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

The codebase includes extensive debug output capabilities via `println()`:

**What's logged:**
- Slab creation and destruction with sizes
- Individual allocation/deallocation events with addresses
- Bitset states after operations (via `printHex()`)
- Slab selection decisions

**How to use:**
1. Build with debug output enabled
2. Run tests or your application
3. Review console output to understand allocation patterns
4. Trace memory lifecycle of specific objects
5. Verify bitset correctness visually

**Educational value:**

Observability is critical in systems programming. The debug output demonstrates:
- How to instrument low-level code
- What information is useful for debugging
- Trade-offs between verbosity and clarity
- How to make internals visible without affecting performance

In production, this output can be disabled via compile-time flags or preprocessor macros, achieving zero-overhead when not needed.

---

## Future Enhancements

### Planned Features

**Enhanced Smart Pointer Support**
- ✅ `std::unique_ptr` support via `make_pool_unique` (COMPLETED)
- ✅ Custom deleter with array specialization (COMPLETED)
- Custom allocator adapters for `std::shared_ptr` (future)
- Observer-pattern proxy for pool lifetime management (future)
- Educational value: Integrating custom allocators with STL

**Thread-Safe Pool**
- Integrate SpinLock with Pool for concurrent allocations
- Per-slab locking vs. global locking trade-offs
- Lock-free allocation paths for common cases
- Educational value: Concurrent data structure design

**Statistics and Profiling**
- Allocation count, peak usage, fragmentation ratio
- Per-size-class metrics
- Real-time monitoring capabilities
- Educational value: Performance analysis techniques

**Memory Alignment**
- Support for aligned allocations (cache lines, SIMD)
- Maintaining alignment with 4-byte header
- Educational value: Hardware-aware programming

**STL Allocator Interface**
- C++ standard library allocator compatibility
- Use with `std::vector`, `std::map`, etc.
- Educational value: Allocator requirements and concepts

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

*For project overview and quick start, see [README.md](README.md)*
