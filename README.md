# Slab Pool Allocator

A modern C++ implementation of a slab-based memory pool allocator with advanced features for efficient fixed-size memory management.

## Overview

This project demonstrates high-performance memory allocation using the slab allocator pattern, featuring multiple size classes, automatic slab management, and a production-quality spinlock implementation. Designed to be both educational and production-ready, showcasing advanced C++ techniques and memory management concepts.

## Key Features

- **12 Optimized Size Classes** - 16 bytes to 1 KB with intelligent intermediate sizes (48, 96, 192, 384, 768)
- **O(1) Allocation/Deallocation** - Bitset-based tracking with two-level availability maps
- **Automatic Slab Growth** - Dynamic allocation of new slabs on demand
- **Large Allocation Fallback** - Seamless handling of allocations > 1 KB
- **Smart Pointer Support** - `make_pool_unique` for RAII-based memory management with automatic pool deallocation
- **Custom SpinLock** - TTAS with escalating backoff and STL compatibility
- **Modern C++20/23** - Template metaprogramming, user-defined literals, atomic operations, ranges

## Quick Start

### Build and Test

```bash
make
./tester
```

### Basic Usage

```cpp
#include "spallocator/spallocator.hpp"

int main() {
    spallocator::Pool pool;

    // Raw allocation
    std::byte* ptr = pool.allocate(128);
    // ... use memory ...
    pool.deallocate(ptr);

    // RAII with smart pointers (recommended)
    auto obj = spallocator::make_pool_unique<MyClass>(pool, arg1, arg2);
    // Automatically deallocated when obj goes out of scope

    // Array support
    auto arr = spallocator::make_pool_unique<int[]>(pool, 100);
    // Array of 100 ints, automatically cleaned up

    return 0;
}
```

## Architecture

### Core Components

| Component | File | Description |
|-----------|------|-------------|
| **Slab** | `spallocator/slab.hpp` | Template class managing fixed-size allocations with bitset tracking |
| **Pool** | `spallocator/pool.hpp` | High-level interface routing allocations to appropriate slabs |
| **SlabProxy** | `spallocator/slab.hpp` | Handles large allocations (>1KB) via standard allocators |
| **Smart Pointers** | `spallocator/spallocator.hpp` | `make_pool_unique`, `PoolDeleter` for RAII memory management |
| **SpinLock** | `spallocator/spinlock.hpp` | Lightweight lock with TTAS and escalating backoff |
| **Helper** | `spallocator/helper.hpp` | User-defined literals, formatting, assertions |

### Size Classes

12 size classes from 16B to 1KB, optimized for common allocation patterns:

```
16B, 32B, 48B, 64B, 96B, 128B, 192B, 256B, 384B, 512B, 768B, 1024B
```

Intermediate sizes (48, 96, 192, 384, 768) reduce internal fragmentation significantly.

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|------------|-------|
| Allocation | O(1) amortized | Two-level bitmap optimization |
| Deallocation | O(log n) | Address map lookup (n = slabs) |
| SpinLock lock/unlock | O(1) | No contention case |

**Space Overhead**: ~3% for typical allocations (4-byte header + bitset amortization)

## SpinLock Features

- **TTAS (Test-and-Test-and-Set)** - Reduces cache coherency traffic
- **Escalating Backoff** - Randomized initial wait, doubles on contention
- **STL Compatible** - Works with `std::scoped_lock` and `std::unique_lock`
- **Memory Ordering** - Proper acquire/release semantics
- **Efficient Blocking** - Uses C++20 `atomic_flag::wait()` after 10 iterations

## Requirements

- **Minimum**: C++20 (for `atomic_flag::wait()`/`notify_one()`)
- **Recommended**: C++23 (for `std::format`, `std::source_location`)
- **Compilers**: GCC 11+, Clang 14+, MSVC 19.29+
- **Dependencies**: Google Test (for unit tests)

## Documentation

- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** - Detailed implementation guide with educational insights
- **Source Files** - Extensive inline comments explaining design decisions

## Testing

Comprehensive test suite covering:
- Slab creation and allocation/deallocation
- Pool size class selection
- Smart pointer lifecycle (single objects and arrays)
- Proper destructor invocation and memory cleanup
- SpinLock contention handling and multi-threading
- STL compatibility

Note: `SpinLockTest.Backoff` may occasionally fail due to intentional race conditions with probabilistic timing.

## Future Work

- ✅ SpinLock implementation (COMPLETED)
- ✅ Smart pointer support with `make_pool_unique` (COMPLETED)
- Integrate SpinLock with Pool for thread-safe allocations
- `shared_ptr` support with pool-based allocator
- Memory statistics and profiling
- STL allocator interface compatibility

## Learning Value

This project demonstrates:
- Template metaprogramming and compile-time optimization
- Modern C++20/23 features (atomics, literals, formatting, ranges, concepts)
- Memory management patterns (slab allocation, bitset tracking, RAII)
- Custom deleters and smart pointer integration
- Concurrency primitives (TTAS, memory ordering, backoff strategies)
- Performance optimization techniques

## References

- [Bonwick (1994) - The Slab Allocator](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps) - Original USENIX paper
- [Linux SLAB/SLUB/SLOB](https://www.kernel.org/doc/gorman/html/understand/understand011.html) - Kernel implementations
- [memcached](https://memcached.org/) - Popularized slab allocation for user-space

## License

BSD 3-Clause License - Copyright (c) 2025, Michael VanLoon

See LICENSE section in source files for full text.

## Contributing

This is an educational project. Contributions should:
- Maintain code clarity over micro-optimizations
- Add comments explaining "why" not just "what"
- Include comprehensive test cases
- Update documentation with insights

## AI Assistance Declaration

README and documentation generated with AI assistance (Claude Code, GitHub Copilot).

Architecture, design, and core implementation are human-generated. Standard library
adaptations (make_unique and make_shared support, along with standard allocator
support) also developed with AI assistance, but in all cases, all code has
been validated, reviewed, and forged by human hands.
