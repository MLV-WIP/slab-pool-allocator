# Slab Pool Allocator

A modern C++ implementation of a slab-based memory pool allocator with advanced features for efficient fixed-size memory management.

## Overview

This project demonstrates high-performance memory allocation using the slab allocator pattern, featuring multiple size classes, automatic slab management, and a production-quality spinlock implementation. Designed to be both educational and production-ready, showcasing advanced C++ techniques and memory management concepts.

**Directory map:**
```
spallocator/          # header-only library (+ docs/SpinLock_Analysis.md)
test/                 # Google Test suite (tester.cpp)
demos/                # example programs
IMPLEMENTATION.md     # deep design and learning guide
README.md             # this overview + build/run
```

## Key Features

- **Compound-key size classes** — native 16-byte ladder (16B–1KB); lazy 32/64-byte ladders up to 2KB/4KB
- **Thread-safe pool** with two-level locking and large-allocation fallback (>1KB)
- **Smart pointers & STL** — `make_pool_unique` / `make_pool_shared` / `PoolAllocator<T>`
- **LifetimeObserver** with atomic liveness checks for async callbacks (object use still needs sync); **SpinLock** (TTAS → backoff → block)
- **Modern C++20/23** — templates, atomics, ranges, concepts

Deep design notes live in [IMPLEMENTATION.md](IMPLEMENTATION.md), not here.

## Quick Start

### Build and Test

The Makefile works with defaults (`BUILD_TYPE=addrsan`) or with `./configure` for a persisted `config.mk`.

```bash
# Optional: generate config.mk
./configure --enable-tests --enable-demo

# Build all enabled targets (tests + demos)
make

# Run the unit test suite
make test
```

Binaries land under `obj/<build_type>/` (default `obj/addrsan/`), with convenience symlinks in `obj/`:

```bash
./obj/addrsan/tester
# or
./obj/tester
```

**Build types** (override without re-running configure):

```bash
make BUILD_TYPE=debug       # unoptimized debug
make BUILD_TYPE=release     # optimized
make BUILD_TYPE=addrsan     # AddressSanitizer + LSan + UBSan (default)
make BUILD_TYPE=memsan      # MemorySanitizer + UBSan (Clang)
make BUILD_TYPE=threadsan   # ThreadSanitizer + UBSan
```

Google Test is required for `make test`. If GTest is not found, tests are disabled automatically.

### Demos

Sources live in `demos/`. After `make`:

| Program | Source | What it shows |
|---------|--------|----------------|
| `demo_spalloc_shared_ptr` | `demos/demo_spalloc_shared_ptr.cpp` | `make_pool_shared`, sharing, `PoolAllocator` with `std::vector` |
| `demo_lifetime_observer` | `demos/demo_lifetime_observer.cpp` | `LifetimeObserver`: single-strand callbacks + cross-thread mutex pattern |

```bash
# Build everything, then run the shared_ptr demo (Makefile `demo` target)
make demo

# Or run either binary directly (default build type = addrsan)
./obj/addrsan/demo_spalloc_shared_ptr
./obj/addrsan/demo_lifetime_observer

# Same via obj/ symlinks
./obj/demo_spalloc_shared_ptr
./obj/demo_lifetime_observer
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
| **LifetimeObserver** | `spallocator/lifetimeobserver.hpp` | Intrusive weak liveness tracking; pair `isAlive()` with a mutex/strand for object use |
| **SpinLock** | `spallocator/spinlock.hpp` | Production-ready lock with TTAS, backoff, and TSan annotations |
| **Helper** | `spallocator/helper.hpp` | User-defined literals, formatting, assertions |

### Size Classes

Per-alignment ladders (`Slab<ElemSize, Alignment>`). Align 4/8 use the 16-byte axis.

```
Axis 16 (eager): 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024
Axis 32 (lazy):  32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
Axis 64 (lazy):  64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
```

Beyond an axis max → `SlabProxy` (always 64-byte aligned `new[]`).

Intermediate sizes (48, 96, 192, 384, 768) reduce internal fragmentation. Slabs are selected by **total footprint** (requested size + allocation header), not the user size alone — see [IMPLEMENTATION.md](IMPLEMENTATION.md).

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|------------|-------|
| Allocation | O(1) amortized | Two-level bitmap optimization |
| Deallocation | O(log n) | Address map lookup (n = slabs) |
| SpinLock lock/unlock | O(1) | No contention case |

**Space Overhead**: Default header is 8 bytes (`header_size = max(8, alignment)`), so ~6% for a 128-byte user request before bitset amortization. Details in [IMPLEMENTATION.md](IMPLEMENTATION.md#memory-layout).

## Requirements

- **Minimum**: C++20 (for `atomic_flag::wait()`/`notify_one()`)
- **Recommended**: C++23 (for `std::format`, `std::source_location`)
- **Compilers**: GCC 11+, Clang 14+, MSVC 19.29+
- **Dependencies**: Google Test (for unit tests)

## Documentation

- **[IMPLEMENTATION.md](IMPLEMENTATION.md)** — Detailed implementation guide: memory layout, size-class selection, smart pointers, concurrency, and educational notes. Start here after the Quick Start above.
- **[SpinLock Analysis](spallocator/docs/SpinLock_Analysis.md)** — SpinLock correctness and performance analysis
- **Source Files** — Inline comments explaining design decisions

## Testing

The suite in `test/tester.cpp` covers slab growth, size-class routing, thread safety, smart pointers, destructor invocation, SpinLock contention, and `PoolAllocator` / STL integration.

```bash
make test
# equivalent:
make && ./obj/addrsan/tester
```

## Completed Features

- ✅ SpinLock implementation with TTAS and escalating backoff
- ✅ Smart pointer support: `make_pool_unique` and `make_pool_shared`
- ✅ Thread-safe Pool with two-level locking strategy
- ✅ STL allocator interface: `PoolAllocator<T>`
- ✅ Object lifetime observer (atomic liveness; synchronize object use)

## Future Work

- Memory statistics and profiling API
- Configurable SpinLock parameters

## References

- [Bonwick (1994) - The Slab Allocator](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps) - Original USENIX paper
- [Linux SLAB/SLUB/SLOB](https://www.kernel.org/doc/gorman/html/understand/understand011.html) - Kernel implementations
- [memcached](https://memcached.org/) - Popularized slab allocation for user-space

## License

BSD 3-Clause License - Copyright (c) 2025-2026, Michael VanLoon

See LICENSE section in source files for full text.

## Contributing

This is an educational project. Contributions should:
- Maintain code clarity over micro-optimizations
- Add comments explaining "why" not just "what"
- Include comprehensive test cases
- Update documentation with insights

## AI Assistance Declaration

README and documentation generated with AI assistance (Claude Code, Grok, Cursor, GitHub Copilot).

Architecture, design, and core implementation are human-generated. Initial
implementation exclusively by Michael VanLoon. Standard library
adaptations (make_unique and make_shared support, along with standard allocator
support) also developed with AI assistance, but in all cases, all code has
been validated, reviewed, and forged by human hands and inspired by human
software development experience.
