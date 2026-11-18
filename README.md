# Slab Pool Allocator

A C++ memory **pool** backed by **slab** size-class storage — for production services that need fast, thread-safe allocation, with a clear design that also teaches well.

## Overview

A memory **pool** (disposable / isolatable allocation domain) backed by **slabs** (fixed-size slot recyclers). Pool and slab are separate ideas — the pool owns lifetime and routing; slabs implement size-class storage.

**Readiness:** The pool, SpinLock, smart-pointer helpers, and `PoolAllocator` are built for concurrent service use. Pool lifetime vs clients is defined: orderly RAII (pointers first), optional arena bulk reclaim (`unique_pool_ptr` no-ops when the pool is already dead), and `PoolHandle::create()` for heap domains that retire when idle. `LifetimeObserver` provides race-free atomic liveness checks that are complete for same-thread / single-strand event loops; when callbacks and teardown can race across threads, pair object use with the same mutex/strand that covers destroy (or use `shared_ptr`/`weak_ptr` when you need upgrade-to-own). Known follow-ons (per-class metrics, freelist) are listed under Future Work.

**Directory map:**
```
spallocator/          # header-only library
spallocator/docs/     # SpinLock_Analysis.md (SpinLock deep dive)
docs/                 # API.md, CONCEPTS.md, IMPLEMENTATION.md
test/                 # Google Test suite (tester.cpp)
demos/                # example programs
README.md             # this overview + build/run
```

## Key Features

- **Pool as allocation domain** — construct, share `Pool&` / `PoolHandle`, destroy to dispose; shard pools to isolate or reduce contention
- **Slab size classes** — native 16-byte ladder (16B–1KB); lazy 32/64-byte ladders up to 2KB/4KB; large fallback via `SlabProxy`
- **Thread-safe routing** — pool + per-slab locking
- **Smart pointers & STL** — `make_pool_unique` / `make_pool_shared` / `PoolAllocator<T>` (via `PoolBlockToken`)
- **Defined pool lifetime** — orderly RAII, arena bulk reclaim, or `PoolHandle::create()` auto-retire
- **LifetimeObserver** with atomic liveness checks for async callbacks; **SpinLock** (TTAS → backoff → block)
- **Modern C++20/23** — templates, atomics, ranges, concepts

Design and API detail live under [`docs/`](docs/), not in this README.

## Quick Start

### Build and Test

The Makefile works with defaults (`BUILD_TYPE=addrsan`) or with `./configure` for a persisted `config.mk`.

```bash
# Optional: generate config.mk
./configure --enable-tests --enable-demo
# optional packing knobs (compile-time; defaults 8192 / 16):
# ./configure --with-slab-target-bytes=16384 --with-slab-target-multiplier=16

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

**Build types** (override without re-running configure). `addrsan` / `memsan` /
`threadsan` always pass the matching `-fsanitize=…` flags (driven by
`BUILD_TYPE`, not by whether `./configure` left `ENABLE_TSAN=yes`):

```bash
make BUILD_TYPE=debug       # unoptimized debug
make BUILD_TYPE=release     # optimized (-O3 -DNDEBUG) + coverage (CI default)
make BUILD_TYPE=addrsan     # AddressSanitizer + LSan + UBSan (default)
make BUILD_TYPE=memsan      # MemorySanitizer (Clang; needs host MSan runtime — not in CI)
make BUILD_TYPE=threadsan   # ThreadSanitizer + UBSan
make coverage               # run tests and print coverage summary (release)
```

`make coverage` uses `llvm-cov gcov` after a Clang build and `gcov` after GCC. Put the desired tool version on `PATH` (e.g. `update-alternatives`).

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
| **Pool** | `spallocator/pool.hpp` | Allocation domain: route by footprint/alignment, header protocol, `trim`, dispose as a unit |
| **Pool lifetime** | `spallocator/pool_lifetime.hpp` | `PoolHandle`, `PoolBlockToken`, `PoolControlBlock` (also via `pool.hpp`) |
| **Slab** | `spallocator/slab.hpp` | Fixed-size slot recycler for one size class (bitmaps, growth, reclaim) |
| **SlabProxy** | `spallocator/slab.hpp` | Large / overflow path via system `new[]` / `delete[]` |
| **Smart Pointers** | `spallocator/spallocator.hpp` | `make_pool_unique`, `make_pool_shared` for RAII over a pool domain |
| **PoolAllocator** | `spallocator/spallocator.hpp` | Standard C++ allocator for STL container integration |
| **PoolHandle** | `spallocator/pool_lifetime.hpp` | Heap domain handle; `create()` retires when idle |
| **LifetimeObserver** | `spallocator/lifetimeobserver.hpp` | Intrusive weak liveness; extra sync only when use and destroy can race across threads |
| **SpinLock** | `spallocator/spinlock.hpp` | Short-section lock with TTAS, backoff, and TSan annotations |
| **Helper** | `spallocator/helper.hpp` | User-defined literals, formatting, assertions |

### Size Classes

Per-alignment ladders (`Slab<ElemSize, Alignment>`). Align 4/8 use the 16-byte axis.

```
Axis 16 (eager): 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024
Axis 32 (lazy):  32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
Axis 64 (lazy):  64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
```

Beyond an axis max → `SlabProxy` (always 64-byte aligned `new[]`).

Intermediate sizes (48, 96, 192, 384, 768) reduce internal fragmentation. Slabs are selected by **total footprint** (requested size + allocation header), not the user size alone — see [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|------------|-------|
| Allocation | O(1) amortized | Two-level bitmap optimization |
| Deallocation | O(log n) | Address map lookup (n = slabs) |
| SpinLock lock/unlock | O(1) | No contention case |

**Space Overhead**: Default header is 8 bytes (`header_size = max(8, alignment)`), so ~6% for a 128-byte user request before bitset amortization. Details in [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md#memory-layout).

## Requirements

- **Minimum**: C++20 (for `atomic_flag::wait()`/`notify_one()`)
- **Recommended**: C++23 (for `std::format`, `std::source_location`)
- **Compilers**: GCC 11+, Clang 14+, MSVC 19.29+
- **Dependencies**: Google Test (for unit tests)

## Documentation

| Doc | Role |
|-----|------|
| **[docs/API.md](docs/API.md)** | Public API reference: contracts, thread-safety, short usage snippets (including helpers) |
| **[docs/CONCEPTS.md](docs/CONCEPTS.md)** | Conceptual / learning-oriented guide (pools, slabs, lifetime patterns, teaching path) |
| **[docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)** | Architecture, design trade-offs, and production readiness |
| **[SpinLock Analysis](spallocator/docs/SpinLock_Analysis.md)** | SpinLock correctness and performance deep dive (linked from IMPLEMENTATION) |

The repo-root [`IMPLEMENTATION.md`](IMPLEMENTATION.md) redirects here. Source headers also carry design comments.

## Testing

The suite in `test/tester.cpp` covers slab growth/trim, size-class routing, header validation, thread safety, smart pointers, SpinLock contention, `PoolAllocator` / STL integration, and `LifetimeObserver` (including concurrent `isAlive`).

```bash
make test
# equivalent:
make && ./obj/addrsan/tester
```

## Future Work

- Per-class metrics breakdown (PoolStats counters/HWMs shipped)
- Configurable SpinLock parameters

### Research / design backlog

Recorded for later (not scheduled). See also
[docs/IMPLEMENTATION.md — Research / design backlog](docs/IMPLEMENTATION.md#research--design-backlog).

- Consider dropping fixed size-class slots in favor of shaping buffers to
  `(element size + header) × N` (or similar multiples)
- Consider a C99-compliant wrapper API for callers that want the pool interface
  from C (opaque handle ≈ `PoolHandle`)
- Abstract the lock primitive so a build can choose `SpinLock` or another
  standard lock (e.g. `std::mutex`) at compile time

## References

- [Bonwick (1994) - The Slab Allocator](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps) - Original USENIX paper
- [Linux SLAB/SLUB/SLOB](https://www.kernel.org/doc/gorman/html/understand/understand011.html) - Kernel implementations
- [memcached](https://memcached.org/) - Popularized slab allocation for user-space

## License

BSD 3-Clause License - Copyright (c) 2025-2026, Michael VanLoon

See LICENSE section in source files for full text.

## Contributing

Production quality and clarity are the same bar. Contributions should:
- Prefer simple designs that stay correct under concurrency and failure
- Avoid obscurely complex micro-optimizations, preferring simplicity and clarity
- Always be mindful of performance impact where it is compatible with simplicity and clarity
- Explain *why* in comments and docs, not just *what*; keep claims aligned with the code
- Add or update tests for every behavior change
- Document design, intention, and tradeoffs

## AI Assistance Declaration

README and documentation generated with AI assistance (Claude Code, Grok, Cursor, GitHub Copilot).

Architecture, design, and core implementation are human-generated. Initial
implementation exclusively by Michael VanLoon. Standard library
adaptations (make_unique and make_shared support, along with standard allocator
support) also developed with AI assistance, but in all cases, all code has
been validated, reviewed, and forged by human hands and inspired by human
software development experience.
