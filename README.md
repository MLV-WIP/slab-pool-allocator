# Slab Pool Allocator

A modern C++ implementation of a slab-based memory pool allocator designed for efficient allocation and deallocation of fixed-size memory blocks.

## Overview

This project demonstrates a high-performance memory allocation strategy using the slab allocator pattern. The allocator pre-allocates large blocks of memory (slabs) and divides them into fixed-size chunks, enabling constant-time allocation and deallocation with minimal fragmentation.

**Educational Focus**: This codebase is designed to be both production-quality and educational, demonstrating advanced C++ techniques and memory management concepts. Each section includes insights into design decisions and implementation patterns.

### Key Features

- **Multiple Size Classes**: Optimized slabs for common allocation sizes (16 bytes to 1 KB)
- **Automatic Slab Management**: Dynamic allocation of new slabs as needed
- **Fast Allocation/Deallocation**: O(1) operations using bitset-based tracking
- **Large Allocation Support**: Seamless fallback to standard allocation for sizes > 1 KB
- **Memory Reuse**: Efficient tracking and reuse of freed memory slots
- **Type Safety**: Modern C++ with strong type safety and constexpr support

## Architecture

### Components

#### 1. **`Slab<ElemSize>`** (`spallocator/slab.hpp`)

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
- **Why bitsets?** Bitsets provide extremely compact memory tracking (1 bit per slot vs 1 byte for bool arrays)
- **Slab growth strategy**: Initial 4KB allocation balances memory overhead with allocation frequency
- **Address mapping**: The `base_address_map` demonstrates how to locate which slab owns a given pointer

#### 2. **`Pool`** (`spallocator/pool.hpp`)

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
- **Allocation metadata trick**: The 4-byte prefix stores size, allowing `deallocate()` to work without size parameter
- **Size class selection**: The ladder of sizes (16, 32, 48, 64...) balances fragmentation vs. number of slabs
- **Constexpr function**: `selectSlab()` can be evaluated at compile-time when possible

#### 3. **`SlabProxy`** (`spallocator/slab.hpp`)

Handles large allocations by delegating to standard allocators.

**Key Concepts Demonstrated**:
- **Adapter Pattern**: Provides uniform interface while delegating to different backend
- **Separation of Concerns**: Large allocations have different performance characteristics

**Design Insights**:
- Allocations > 1 KB are typically too large to benefit from pooling
- Inherits from `AbstractSlab` to maintain uniform interface
- No internal state needed - pure delegation

**Educational Highlights**:
- **When NOT to pool**: Large allocations are infrequent and don't benefit from pre-allocation overhead
- **Interface uniformity**: Same `allocateItem()`/`deallocateItem()` API regardless of backend

#### 4. **Helper Utilities** (`spallocator/helper.hpp`)

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
- **User-defined literals**: Enable `4_KB` instead of `4096` for better readability
- **Runtime assertions**: Unlike `assert()`, these work in release builds and provide rich context
- **Modern formatting**: `std::format` is type-safe unlike `printf`, and more efficient than streams

### Size Classes

The pool allocator uses 12 optimized size classes:

| Size Class | Max Size | Slab Index | Slots per 4KB | Design Rationale |
|------------|----------|------------|---------------|------------------|
| 16 bytes   | 16 B     | 0          | 256           | Tiny objects (flags, counters) |
| 32 bytes   | 32 B     | 1          | 128           | Small structs |
| 48 bytes   | 48 B     | 2          | 85            | Small strings, short vectors |
| 64 bytes   | 64 B     | 3          | 64            | Cache line size, small objects |
| 96 bytes   | 96 B     | 4          | 42            | Medium structs |
| 128 bytes  | 128 B    | 5          | 32            | Common allocation size |
| 192 bytes  | 192 B    | 6          | 21            | Larger structs |
| 256 bytes  | 256 B    | 7          | 16            | Small buffers |
| 384 bytes  | 384 B    | 8          | 10            | Medium buffers |
| 512 bytes  | 512 B    | 9          | 8             | Half-KB allocations |
| 768 bytes  | 768 B    | 10         | 5             | Large buffers |
| 1024 bytes | 1 KB     | 11         | 4             | Maximum pooled size |

**Educational Note**: Size classes aren't powers of 2 alone - intermediate sizes (48, 96, 192, 384, 768) reduce internal fragmentation. For example, a 100-byte allocation in a 128-byte slot wastes only 28 bytes (22%), versus 156 bytes (61%) in a 256-byte slot.

Allocations larger than 1 KB are handled by `SlabProxy` using standard allocation.

## Usage

### Basic Example

```cpp
#include "spallocator/spallocator.hpp"

int main() {
    spallocator::Pool pool;

    // Allocate memory
    std::byte* ptr = pool.allocate(128);

    // Use the memory...

    // Deallocate when done
    pool.deallocate(ptr);

    return 0;
}
```

### Multiple Allocations

```cpp
#include "spallocator/spallocator.hpp"
#include <vector>

int main() {
    spallocator::Pool pool;
    std::vector<std::byte*> allocations;

    // Allocate various sizes
    for (std::size_t size : {16, 32, 64, 128, 256, 512}) {
        allocations.push_back(pool.allocate(size));
    }

    // Free all allocations
    for (auto ptr : allocations) {
        pool.deallocate(ptr);
    }

    return 0;
}
```

## Implementation Details

### Memory Layout

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

**Educational Insight**: This header technique is common in allocators. The 4-byte overhead is minimal (~3% for 128-byte allocation), and eliminates the need to pass size to `deallocate()`, matching the standard `delete` interface. The size is masked to 16 bits (`alloc_size & 0xFFFF`), limiting individual allocations to 64KB, which is acceptable given the 1KB pool limit.

### Slab Allocation Strategy

**Initial Allocation Sizes**:
- **Small sizes (< 1 KB)**: Initial slab size of 4 KB
- **Large sizes (≥ 1 KB)**: Initial slab size of `ElemSize * 4`

**Growth Policy**:
- New slabs allocated on-demand when current slabs are full
- Each slab is the same size (no exponential growth)
- Maximum of `4 GB / slab_alloc_size` slabs per size class

**Educational Insight**: The 4KB initial size is chosen because:
1. It's a common memory page size, potentially reducing allocation overhead
2. It provides enough slots to amortize the slab management overhead
3. It's small enough to avoid wasting memory for infrequently-used size classes

The growth model is linear (same-sized slabs) rather than exponential because:
- Predictable memory usage patterns
- Simpler to reason about
- Prevents excessive over-allocation

### Bitset Tracking System

Each slab maintains two types of bitsets:

#### 1. **Slab Map** (`slab_map`)
Tracks individual slot allocation status within each slab.

```cpp
std::vector<std::bitset<slab_alloc_size / ElemSize>> slab_map;
```

- **Size**: One bit per slot (e.g., 256 bits for 16-byte elements in 4KB slab)
- **Semantics**: 1 = allocated, 0 = free
- **Usage**: Scanned to find free slots during allocation

#### 2. **Availability Map** (`slab_available_map`)
Tracks which slabs have at least one free slot.

```cpp
std::bitset<max_slabs> slab_available_map;
```

- **Size**: One bit per slab
- **Semantics**: 1 = has free slots, 0 = completely full
- **Usage**: Fast filtering to skip full slabs

**Educational Insight**: This two-level bitmap approach is a classic optimization:
- Without it: Must scan all slabs, checking each slot → O(total_slots)
- With it: Only scan slabs marked available → O(available_slabs × slots_per_slab)

When most slabs are full, this dramatically reduces allocation time. This pattern appears in Linux's buddy allocator, page frame allocators, and many other memory management systems.

### Address Lookup for Deallocation

**Problem**: Given a pointer, which slab does it belong to?

**Solution**: Maintain a map of slab base addresses.

```cpp
std::map<std::byte*, std::size_t> base_address_map;
```

**Algorithm** (in `findSlabForItem()`):
1. Use `upper_bound()` to find the first slab starting AFTER the pointer
2. Back up one entry to find the slab starting BEFORE the pointer
3. Verify pointer is within [slab_start, slab_end) range
4. Return slab index

**Educational Insight**: This demonstrates a classic computational geometry technique - finding which interval contains a point. The `std::map` (red-black tree) provides O(log n) lookup. Alternative approaches include:
- **Hash table**: O(1) but requires alignment or metadata
- **Linear search**: O(n) but simpler code
- **Binary search on sorted vector**: O(log n) with better cache locality

The map approach balances performance with code clarity.

## Building and Testing

### Prerequisites

- C++23 compatible compiler (for `std::format`, `std::source_location`)
- Google Test framework (for unit tests)
- Make

### Build

```bash
make
```

### Run Tests

```bash
./tester
```

### Test Coverage

The test suite (`tester.cpp`) includes:

- **Slab Creation Tests**: Verify proper initialization of various size classes
  - Tests compile-time size calculations
  - Validates `constexpr` functionality

- **Allocation Tests**: Test allocation, reuse, and multi-slab scenarios
  - Verifies slab growth when capacity exhausted
  - Tests memory reuse after deallocation
  - Validates bitset tracking correctness

- **Pool Selector Tests**: Validate size-to-slab mapping logic
  - Tests all boundary conditions (e.g., size 64 vs 65)
  - Ensures consistent routing to size classes

- **Integration Tests**: End-to-end allocation/deallocation workflows
  - Mixed size allocations
  - Deallocate-in-different-order scenarios
  - Large allocation handling

## Learning Objectives

This codebase teaches several important concepts:

### 1. **Template Metaprogramming**
- Compile-time computation with `constexpr`
- Template specialization for size-specific behavior
- `static_assert` for compile-time validation

### 2. **Modern C++ Features** (C++20/23)
- User-defined literals for domain-specific syntax
- `std::format` for type-safe formatting
- `std::source_location` for debugging context
- `std::optional` for optional return values

### 3. **Memory Management Patterns**
- Slab allocation vs. general-purpose allocation
- Internal vs. external fragmentation tradeoffs
- Memory pooling strategies
- RAII and resource ownership

### 4. **Data Structure Design**
- Bitsets for compact state tracking
- Two-level indexing for performance
- Address-to-index mapping techniques
- Virtual interfaces for polymorphism

### 5. **Performance Optimization**
- Cache-friendly data layout considerations
- Reducing allocation frequency via pooling
- Amortized constant-time operations
- Space-time tradeoffs (e.g., bitsets vs. free lists)

### 6. **Software Engineering Practices**
- Clear separation of concerns (Slab vs Pool vs Proxy)
- Non-copyable/non-moveable resource classes
- Comprehensive unit testing
- Debug output for observability

## Future Enhancements

### Planned Features

- [ ] **Smart Pointer Support**: Custom allocator adapters for `std::shared_ptr` and `std::unique_ptr`
  - **Educational Value**: Demonstrates how to integrate custom allocators with STL components
  - **Implementation**: Requires allocator traits and proper rebinding

- [ ] **Thread Safety**: Mutex protection for concurrent allocations
  - **Educational Value**: Shows thread-safe data structure design patterns
  - **Consideration**: Per-slab locking vs. global locking tradeoffs

- [ ] **Statistics**: Memory usage tracking and reporting
  - **Metrics**: Allocation count, fragmentation ratio, peak usage
  - **Educational Value**: Performance monitoring and profiling techniques

- [ ] **Memory Alignment**: Support for aligned allocations
  - **Educational Value**: Cache line alignment, SIMD requirements
  - **Challenge**: Maintaining alignment while preserving 4-byte header

- [ ] **STL Allocator Interface**: C++ standard library allocator compatibility
  - **Educational Value**: Understanding allocator requirements and concepts
  - **Use Case**: Custom allocator for `std::vector`, `std::map`, etc.

### Optimization Opportunities

- **Custom bitset implementation** with 64-bit atomic operations
  - **Benefit**: Lock-free slot allocation in multithreaded scenarios
  - **Learning**: Atomic operations, memory ordering, lock-free programming

- **SIMD-based bitset scanning** for faster free slot discovery
  - **Benefit**: Process 128/256 bits per instruction
  - **Learning**: Vectorization, intrinsics, data parallelism

- **Free lists** as an alternative to bitsets
  - **Tradeoff**: Faster allocation (O(1) guaranteed) but more memory overhead
  - **Learning**: Different allocation tracking strategies

- **Thread-local caching** to reduce contention
  - **Pattern**: Each thread has a small cache of pre-allocated objects
  - **Learning**: Thread-local storage, cache locality

## Performance Characteristics

### Time Complexity

| Operation | Complexity | Notes |
|-----------|------------|-------|
| Allocation (best case) | O(1) | Free slot in first available slab |
| Allocation (worst case) | O(n) | n = slots per slab, must scan to find free slot |
| Allocation (amortized) | O(1) | Typically finds slot quickly due to availability map |
| Deallocation | O(log m) | m = number of slabs, due to map lookup |
| Slab Selection | O(1) | Compile-time constexpr lookup table |

**Educational Insight**: The "amortized O(1)" claim requires explanation:
- If slabs are mostly empty, free slots are found immediately → O(1)
- If slabs are mostly full, the availability map filters out full slabs quickly
- Only in pathological cases (many partially-full slabs) does it approach O(n)

Real-world performance is typically excellent because:
1. Allocations and deallocations tend to follow patterns (temporal locality)
2. The availability map provides coarse-grained filtering
3. Bitset operations are extremely fast (hardware-supported)

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

**Educational Insight**: The overhead is minimal because:
- Bitsets are extremely space-efficient (1 bit vs 1 byte for boolean arrays)
- Metadata is amortized across all allocations in a slab
- For larger allocations, the 4-byte header becomes negligible

### Fragmentation Analysis

**Internal Fragmentation**: Wasted space within allocated slots
- **Example**: 100-byte allocation in 128-byte slot wastes 28 bytes (22%)
- **Mitigation**: Multiple size classes reduce waste
- **Worst case**: Just over 50% in the gap between size classes

**External Fragmentation**: Inability to satisfy large allocations despite free memory
- **Not applicable**: Fixed-size slots prevent external fragmentation within slabs
- **Slab-level**: Fragmented slabs may prevent slab deallocation, but memory remains usable

**Educational Insight**: This is a key advantage of slab allocators - they eliminate external fragmentation at the cost of some internal fragmentation. The size class selection is critical: more classes reduce internal fragmentation but increase metadata overhead.

## Advanced Topics

### Why Slab Allocation?

Slab allocators excel in scenarios with:
- **Frequent allocation/deallocation**: Reduces overhead vs. general allocators
- **Known size patterns**: Size classes can be optimized for workload
- **Temporal locality**: Recently freed objects are hot in cache
- **Reduced fragmentation**: Fixed sizes prevent external fragmentation

**When NOT to use slab allocation**:
- Highly variable allocation sizes (wastes memory in internal fragmentation)
- Infrequent allocations (overhead of pre-allocation not amortized)
- Extremely large objects (pooling doesn't help)

### Comparison to Other Allocators

| Allocator Type | Strengths | Weaknesses |
|----------------|-----------|------------|
| **Slab** (this project) | Fast, low fragmentation, cache-friendly | Internal fragmentation, size limits |
| **Buddy** | Flexible sizes, coalescing | Fragmentation, complexity |
| **Free List** | Simple, general-purpose | Can be slow, fragmentation |
| **TCMalloc/jemalloc** | Excellent general performance | Complex, large codebase |

### Connection to Operating Systems and Real-World Applications

This allocator demonstrates concepts used in:
- **Linux SLAB/SLUB/SLOB**: Kernel object caching
- **Solaris Slab Allocator**: Original Bonwick design (1994)
- **FreeBSD UMA**: Unified Memory Allocator
- **memcached**: The distributed memory caching system that popularized slab allocation for user-space applications

**Historical Context**: While Jeff Bonwick introduced the slab allocator concept for the Solaris kernel in 1994, **memcached** (created by Brad Fitzpatrick in 2003) brought widespread attention to slab allocation in user-space applications. memcached's use of slab allocation for caching frequently-accessed objects demonstrated the pattern's effectiveness beyond kernel memory management, inspiring countless implementations in web applications, databases, and high-performance systems.

**Educational Value**: Understanding user-space allocators provides insight into how operating systems manage kernel memory. The same principles (size classes, bitsets, pooling) appear throughout systems programming, from kernel allocators to application-level caching systems like memcached.

## Debugging and Instrumentation

The code includes extensive debug output (via `println()`):
- Slab creation and destruction
- Individual allocation/deallocation events
- Bitset states after operations
- Slab selection decisions

**How to use**:
- Review console output to understand allocation patterns
- Trace memory lifecycle of specific objects
- Verify bitset correctness visually (via `printHex()`)

**Educational Value**: This demonstrates the importance of observability in systems programming. The debug output can be disabled in production via compile-time flags or preprocessor macros.

## References and Further Reading

### Academic Papers
- Bonwick, Jeff (1994). ["The Slab Allocator: An Object-Caching Kernel Memory Allocator"](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps) - Original USENIX paper introducing slab allocation

### Concepts
- [Slab Allocation](https://en.wikipedia.org/wiki/Slab_allocation) - Wikipedia overview
- [Memory Pool](https://en.wikipedia.org/wiki/Memory_pool) - General memory pool concepts
- [Buddy Memory Allocation](https://en.wikipedia.org/wiki/Buddy_memory_allocation) - Alternative approach

### Real-World Implementations
- [memcached](https://memcached.org/) - Distributed memory caching system using slab allocation
- [jemalloc](https://github.com/jemalloc/jemalloc) - Production allocator using similar concepts
- [TCMalloc](https://github.com/google/tcmalloc) - Google's thread-caching malloc
- [mimalloc](https://github.com/microsoft/mimalloc) - Microsoft's performance allocator

### C++ Resources
- [C++23 Features](https://en.cppreference.com/w/cpp/23) - Reference for modern C++ used in this project
- [Allocator Requirements](https://en.cppreference.com/w/cpp/named_req/Allocator) - Standard allocator interface

## Contributing

This is an educational project. When contributing:
- Maintain code clarity over micro-optimizations
- Add comments explaining "why" not just "what"
- Include test cases demonstrating new features
- Update this README with educational insights

## Requirements

### C++ Standard
C++23 or later required for:
- `std::format` (string formatting)
- `std::source_location` (debugging context)

### Compiler Support

| Compiler | Minimum Version | Notes |
|----------|----------------|-------|
| GCC      | 13.0           | Full C++23 support |
| Clang    | 17.0           | Full C++23 support |
| MSVC     | 19.34 (VS 2022 17.4) | Partial C++23 support |

### Dependencies
- **Google Test**: Unit testing framework
- **Standard Library**: C++23 standard library implementation

## License

BSD 3-Clause License

Copyright (c) 2025, Michael VanLoon

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## AI assistance declaration

README were generated largely with AI.

AI (Claude code and Github Copilot) used for API and completion assistance.

Architecture, design, and overall implementation is human generated.

## To do

- Add smart-pointer (shared_ptr, unique_ptr) support
- Add an observer-pattern proxy for memory objects owned by shared pointers
  where the pointed-to object is only destroyed if the underlying memory
  pool is still valid
