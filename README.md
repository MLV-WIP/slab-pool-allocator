# Slab Pool Allocator

A modern C++ implementation of a slab-based memory pool allocator with advanced features for efficient fixed-size memory management.

## Overview

This project demonstrates high-performance memory allocation using the slab allocator pattern, featuring multiple size classes, automatic slab management, and a production-quality spinlock implementation. Designed to be both educational and production-ready, showcasing advanced C++ techniques and memory management concepts.

## Key Features

- **12 Optimized Size Classes** - 16 bytes to 1 KB with intelligent intermediate sizes (48, 96, 192, 384, 768)
- **O(1) Allocation/Deallocation** - Bitset-based tracking with two-level availability maps
- **Thread-Safe Pool** - Two-level locking (pool + per-slab) for optimal concurrency
- **Automatic Slab Growth** - Dynamic allocation of new slabs on demand
- **Large Allocation Fallback** - Seamless handling of allocations > 1 KB
- **Smart Pointer Support** - `make_pool_unique` and `make_pool_shared` for RAII-based memory management
- **Standard Allocator Interface** - `PoolAllocator<T>` for STL container integration
- **Production-Ready SpinLock** - TTAS with three-phase contention handling (spin → backoff → block)
- **Modern C++20/23** - Template metaprogramming, user-defined literals, atomic operations, ranges, concepts

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
    spallocator::Pool pool;  // Thread-safe by default

    // Raw allocation
    std::byte* ptr = pool.allocate(128);
    // ... use memory ...
    pool.deallocate(ptr);

    // unique_ptr with automatic cleanup
    auto obj = spallocator::make_pool_unique<MyClass>(pool, arg1, arg2);
    auto arr = spallocator::make_pool_unique<int[]>(pool, 100);

    // shared_ptr with reference counting
    auto shared = spallocator::make_pool_shared<MyClass>(pool, arg1, arg2);
    auto copy = shared;  // Both point to same object

    // STL containers with pool allocator
    std::vector<int, spallocator::PoolAllocator<int>> vec(pool);
    vec.push_back(42);

    return 0;
}
```

## Architecture

### Core Components

| Component | File | Description |
|-----------|------|-------------|
| **Slab** | `spallocator/slab.hpp` | Template class managing fixed-size allocations with bitset tracking |
| **Pool** | `spallocator/pool.hpp` | Thread-safe interface routing allocations to appropriate slabs |
| **SlabProxy** | `spallocator/slab.hpp` | Handles large allocations (>1KB) via standard allocators |
| **Smart Pointers** | `spallocator/spallocator.hpp` | `make_pool_unique`, `make_pool_shared` for RAII memory management |
| **PoolAllocator** | `spallocator/spallocator.hpp` | Standard C++ allocator for STL container integration |
| **SpinLock** | `spallocator/spinlock.hpp` | Production-ready lock with TTAS, backoff, and TSan annotations |
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

- **Three-Phase Contention** - Active spin → escalating backoff → efficient blocking
- **TTAS (Test-and-Test-and-Set)** - Reduces cache coherency traffic
- **Escalating Backoff** - Randomized 1-100ns initial wait, additive doubling on contention
- **STL Compatible** - Works with `std::scoped_lock` and `std::unique_lock` (BasicLockable)
- **Memory Ordering** - Proper acquire/release semantics with relaxed optimization
- **Efficient Blocking** - Uses C++20 `atomic_flag::wait()` after backoff exhaustion
- **TSan Annotations** - ThreadSanitizer integration for race detection
- **Production Ready** - Analyzed and validated (see `docs/SpinLock_Analysis.md`)

## Requirements

- **Minimum**: C++20 (for `atomic_flag::wait()`/`notify_one()`)
- **Recommended**: C++23 (for `std::format`, `std::source_location`)
- **Compilers**: GCC 11+, Clang 14+, MSVC 19.29+
- **Dependencies**: Google Test (for unit tests)

## Documentation

- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** - Detailed implementation guide with educational insights
- **[docs/SpinLock_Analysis.md](spallocator/docs/SpinLock_Analysis.md)** - Comprehensive SpinLock correctness and performance analysis
- **Source Files** - Extensive inline comments explaining design decisions

## Testing

Comprehensive test suite covering:
- Slab creation and allocation/deallocation
- Pool size class selection and thread safety
- Smart pointer lifecycle (`unique_ptr` and `shared_ptr`)
- Proper destructor invocation and memory cleanup
- SpinLock contention handling and multi-threading
- STL compatibility and `PoolAllocator` integration

**Build targets**:
- `make` - Build all (tester + demo_shared_ptr)
- `BUILD=threadsan make` - Build with ThreadSanitizer for race detection
- `BUILD=memsan make` - Build with AddressSanitizer + UBSanitizer (default)

**Demo programs**:
- `./obj/memsan/tester` - Run all unit tests
- `./obj/memsan/demo_shared_ptr` - Demonstrate shared_ptr usage

## Future Work

- ✅ SpinLock implementation (COMPLETED)
- ✅ Smart pointer support: `make_pool_unique` and `make_pool_shared` (COMPLETED)
- ✅ Thread-safe Pool with SpinLock integration (COMPLETED)
- ✅ STL allocator interface: `PoolAllocator<T>` (COMPLETED)
- Memory statistics and profiling API
- Per-slab locking for improved concurrency
- Lock-free allocation paths for common cases
- Configurable SpinLock parameters

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
