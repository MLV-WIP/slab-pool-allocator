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
- [LifetimeObserver - Asynchronous Object Lifetime Tracking](#lifetimeobserver---asynchronous-object-lifetime-tracking)
- [Thread Safety and Concurrent Access](#thread-safety-and-concurrent-access)
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

**Allocation metadata trick**: The 4-byte prefix stores allocation size, allowing `deallocate()` to work without a size parameter. This matches the standard `delete` interface and simplifies the API.

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

    explicit PoolAllocator(Pool& pool) noexcept : pool_ref(pool) {}

    T* allocate(std::size_t n) {
        std::byte* mem = pool_ref.allocate(n * sizeof(T), alignof(T));
        return reinterpret_cast<T*>(mem);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        pool_ref.deallocate(reinterpret_cast<std::byte*>(p));
    }

private:
    Pool& pool_ref;
};

template<typename T, typename... Args>
std::shared_ptr<T> make_pool_shared(Pool& pool, Args&&... args)
{
    PoolAllocator<T> alloc(pool);
    return std::allocate_shared<T>(alloc, std::forward<Args>(args)...);
}
```

**How It Works**:
- `std::allocate_shared` uses our custom allocator for **both** the object and control block
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

Unlike `make_pool_unique` which needs size tracking in a header, `shared_ptr` tracks array size internally in the control block.

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
- **Async-Safe Operations**: Checking liveness without locks in many cases

### Design Architecture

```cpp
class LifetimeObserver
{
    struct ControlBlock {
        int64_t owner_count;      // Strong references
        int64_t observer_count;   // Weak references
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

```cpp
class AsyncService: public LifetimeObserver
{
public:
    AsyncService() {  // Create owner reference
        // Register callback that captures observer
        event_loop.on_data([alive = getObserver()](const Data& d)
        {
            // Safe to check liveness without locks
            if (alive)
            {
                // Object still exists, safe to access
                process_data(d);
            }
        });
    }

    ~AsyncService()
    {
        // Control block survives destruction
        // Callbacks can still safely check observer.isAlive() → false
    }
};
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

**Why this is safe**:
- Lock-free check: No synchronization needed (atomic-like semantics expected)
- Observer can outlive object: Control block never destroyed while references exist
- Simple: Just checks owner count > 0

**Usage in Callbacks**:
```cpp
auto callback = [observer = obj.getObserver()]() {
    if (observer) {  // operator bool() = isAlive()
        // Safe to proceed
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

### Comparison to std::shared_ptr/std::weak_ptr

| Feature | `std::shared_ptr`/`std::weak_ptr` | `LifetimeObserver` |
|---------|-----------------------------------|-------------------|
| **Purpose** | General-purpose shared ownership | Lightweight object lifetime tracking |
| **Space overhead** | 16+ bytes (pointer + control block ref) | 16 bytes (pointer + ownership type) |
| **Thread safety** | Full atomic reference counting | Application-dependent |
| **Flexibility** | Works with any object | Object must derive from LifetimeObserver |
| **Liveness check** | `use_count() > 0` | `isAlive()` |
| **Complexity** | More sophisticated (deleter, allocator) | Simpler, educational |
| **Use case** | Production shared ownership | Async callback safety, teaching |

### Learning Value

This implementation teaches:

1. **Observer Pattern**: Loose coupling between object and lifetime observers
2. **Mediator Pattern**: Control block mediates strong/weak reference operations
3. **Reference Counting**: Manual lifetime management without garbage collection
4. **Move Semantics**: Efficient ownership transfer and moved-from object validity
5. **Copy Assignment**: Exception-safe assignment with reference swapping
6. **RAII**: Automatic cleanup through scope (destructor calls release)
7. **Async Safety**: Safe callback patterns without locks
8. **Control Flow**: Checking preconditions before accessing objects

### Common Pitfalls

#### 1. Holding Reference to Original, Not Observer

```cpp
// ❌ WRONG - callback holds reference to owner
LifetimeObserver obj;
event_loop.on_event([obs = obj]() {  // Wrong: makes owner copy
    // This prevents obj from being destroyed!
});

// ✅ RIGHT - callback holds weak reference
LifetimeObserver obj;
event_loop.on_event([obs = obj.getObserver()]() {  // Correct: weak reference
    if (obs.isAlive()) {
        // Safe to proceed
    }
});
```

#### 2. Forgetting to Check isAlive()

```cpp
// ❌ WRONG - object might be destroyed
auto observer = obj.getObserver();
// ... later ...
obj_method();  // CRASH if obj was destroyed

// ✅ RIGHT - always check before accessing
if (observer.isAlive()) {
    obj_method();  // Safe
}
```

#### 3. Race Conditions (Multi-threaded)

```cpp
// ❌ Potential race: object destroyed between check and use
if (observer.isAlive()) {
    // Object might be destroyed here by another thread
    use_object();  // RACE CONDITION
}

// ✅ Use synchronization if accessing from multiple threads
std::lock_guard lock(mutex);
if (observer.isAlive()) {
    use_object();  // Protected
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

**Performance Characteristics**:

| Scenario | Pool Lock | Slab Lock | Total Time |
|----------|-----------|-----------|------------|
| **No contention** | 5-10 cycles | 20-50 cycles | 25-60 cycles |
| **Different slabs** | Serialized (5-10 cycles) | **Parallel** ✅ | 25-60 cycles each |
| **Same slab** | Serialized (5-10 cycles) | Serialized (20-50 cycles) | 25-60 cycles each |

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
BUILD=threadsan make
./obj/threadsan/tester
```

ThreadSanitizer confirms:
- No data races in pool operations
- Proper synchronization through SpinLock
- Correct happens-before relationships

See `docs/SpinLock_Analysis.md` for detailed correctness analysis.

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
- Support for aligned allocations (cache lines, SIMD)
- Maintaining alignment with 4-byte header
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

*For project overview and quick start, see [README.md](README.md)*
