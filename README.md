# Slab Pool Allocator

A C++ memory **pool** backed by **slab** size-class storage — for production services that need fast, thread-safe allocation, with a clear design that also teaches well.

## Overview

A memory **pool** (disposable / isolatable allocation domain) backed by **slabs** (fixed-size slot recyclers). Pool and slab are separate ideas — the pool owns lifetime and routing; slabs implement size-class storage.

**Terminology:** Pool → Slab (size-class engine) → **Block** (mapped chunk) → Slot. See [docs/terminology-glossary.md](docs/terminology-glossary.md) and [docs/CONCEPTS.md](docs/CONCEPTS.md#terminology).

**Readiness:** The pool, smart-pointer helpers, and `PoolAllocator` are built for concurrent service use. Pool lifetime vs clients is defined: orderly RAII (pointers first), optional arena bulk reclaim (`unique_pool_ptr` no-ops when the pool is already dead), and `PoolHandle::create()` for heap domains that retire when idle. Short critical sections use `mlvlib::SpinLock` (from the [mlvlib](https://github.com/MLV-WIP/mlvlib) submodule). Known follow-ons (per-class metrics, freelist) are listed under Future Work.

**Directory map:**
```
spallocator/          # header-only allocator library
mlvlib/               # git submodule — general utils (SpinLock, LifetimeObserver, helpers)
docs/                 # API.md, CONCEPTS.md, IMPLEMENTATION.md, terminology-glossary.md
test/                 # Google Test suite (tester.cpp)
demos/                # example programs
bench/                # on-demand allocator comparison (`make bench`)
README.md             # this overview + build/run
```

## Key Features

- **Pool as allocation domain** — construct, share `Pool&` / `PoolHandle`, destroy to dispose; shard pools to isolate or reduce contention
- **Slab size classes** — alignment-class ladders 16 (16B–4KB), 32 (32B–8KB), 64 (64B–16KB); large fallback via `SlabProxy`
- **Thread-safe routing** — pool + per-slab locking
- **Smart pointers & STL** — `make_pool_unique` / `make_pool_shared` / `PoolAllocator<T>` (via `PoolLifetimeToken`)
- **Optional PMR bridge** — `PoolMemoryResource` in `pool_pmr.hpp` for `std::pmr` containers (prefer `PoolAllocator` when types can bake it in)
- **Defined pool lifetime** — orderly RAII, arena bulk reclaim, or `PoolHandle::create()` auto-retire
- **mlvlib dependency** — `SpinLock`, helpers, and optional `LifetimeObserver` via the [mlvlib](https://github.com/MLV-WIP/mlvlib) submodule
- **Modern C++20+** — templates, atomics, ranges, concepts; C++23/26 when the toolchain allows

Design and API detail live under [`docs/`](docs/), not in this README.

## Quick Start

### Build and Test

The Makefile works with defaults (`BUILD_TYPE=addrsan`) or with `./configure` for a persisted `config.mk`.

```bash
# Required once after clone (and after CI checkout without submodules:recursive)
git submodule update --init

# Optional: generate config.mk
./configure --enable-tests --enable-demo
# optional: pin dialect (default = newest of c++26/c++23/c++20 the compiler accepts)
# ./configure --with-cxxstd=c++20
# optional compile-time size knobs (defaults 8192 / 16 / 1024 MiB / 4096 MiB):
# ./configure --with-slab-target-bytes=16384 --with-slab-target-multiplier=16
# ./configure --with-max-allocation-mib=256 --with-max-class-mib=1024

# Build all enabled targets (tests + demos)
make

# Run the unit test suite
make test

# On-demand allocator comparison (not part of make test; prefer release)
make bench BUILD_TYPE=release
# make bench BUILD_TYPE=release BENCH_ARGS='--threads=4'
# make bench BUILD_TYPE=release BENCH_ARGS='--align=64'   # Pool vs ::new spot-check
# make bench BUILD_TYPE=release BENCH_ARGS='--scenario=lifo --backend=pool'
# Focused perf record/report (BUILD_TYPE=profile: -O3 + frame pointers + -g)
# make profile BENCH_ARGS='--scenario=lifo --backend=slab16 --iters=2000000'
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
make BUILD_TYPE=profile     # like release + -g -fno-omit-frame-pointer (for perf)
make BUILD_TYPE=addrsan     # AddressSanitizer + LSan + UBSan (default)
make BUILD_TYPE=memsan      # MemorySanitizer (Clang; needs host MSan runtime — not in CI)
make BUILD_TYPE=threadsan   # ThreadSanitizer + UBSan
make coverage               # run tests and print coverage summary (release)
```

`make coverage` uses `llvm-cov gcov` after a Clang build and `gcov` after GCC. Put the desired tool version on `PATH` (e.g. `update-alternatives`).

Google Test is required for `make test`. If GTest is not found, tests are disabled automatically.

### Benchmarks

On-demand harness in `bench/` (not built by default `make`, not a CI gate):

| Program | Source | What it compares |
|---------|--------|------------------|
| `bench_allocators` | `bench/bench_allocators.cpp` | `::operator new`, `Pool`, `pool_pmr`, `std::pmr` pool resources, bare `Slab<16,16>` |

Reports ns/op and **ratio vs `::new`**. Prefer `BUILD_TYPE=release` (`make bench` forces coverage off). Filter with `--scenario=` / `--backend=` for focused runs; `--pool-stats` for optional `PoolStats` dumps. For `perf` stacks use `make profile` (`BUILD_TYPE=profile`: `-O3` + frame pointers + `-g`). See [docs/IMPLEMENTATION.md — Performance](docs/IMPLEMENTATION.md#performance).

### Demos

Sources live in `demos/`. After `make`:

| Program | Source | What it shows |
|---------|--------|----------------|
| `demo_spalloc_shared_ptr` | `demos/demo_spalloc_shared_ptr.cpp` | `make_pool_shared`, sharing, `PoolAllocator` with `std::vector` |
| `demo_pool_pmr` | `demos/demo_pool_pmr.cpp` | `PoolMemoryResource` + `pmr::vector`, vs `PoolAllocator`, tracing decorator |

`LifetimeObserver` demos and tests live in [mlvlib](https://github.com/MLV-WIP/mlvlib).

```bash
# Build everything, then run both demos (Makefile `demo` target)
make demo

# Or run a binary directly (default build type = addrsan)
./obj/addrsan/demo_spalloc_shared_ptr
./obj/addrsan/demo_pool_pmr
# Same via obj/ symlink
./obj/demo_spalloc_shared_ptr
./obj/demo_pool_pmr
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
| **Pool lifetime** | `spallocator/pool_lifetime.hpp` | `PoolHandle`, `PoolLifetimeToken`, `PoolControlBlock` (also via `pool.hpp`) |
| **Slab** | `spallocator/impl/slab.hpp` | Fixed-size slot recycler for one size class (bitmaps, growth, reclaim) |
| **SlabProxy** | `spallocator/impl/slab_proxy.hpp` | Large / overflow path via system `new[]` / `delete[]` |
| **Smart Pointers** | `spallocator/spallocator.hpp` | `make_pool_unique`, `make_pool_shared` for RAII over a pool domain |
| **PoolAllocator** | `spallocator/spallocator.hpp` | Standard C++ allocator for STL container integration |
| **PoolMemoryResource** | `spallocator/pool_pmr.hpp` | Optional `std::pmr::memory_resource` adapter (additive header) |
| **PoolHandle** | `spallocator/pool_lifetime.hpp` | Heap domain handle; `create()` retires when idle |
| **Helper** | `spallocator/impl/helper.hpp` | Pool size caps; re-exports general helpers from mlvlib (impl/) |
| **SpinLock** | `mlvlib/spinlock.hpp` (submodule) | Short-section lock used by Pool/Slab (`mlvlib::SpinLock`) |
| **LifetimeObserver** | `mlvlib/lifetimeobserver.hpp` (submodule) | Standalone liveness helper (optional; not required by Pool) |

### Size Classes

Per-alignment ladders (`Slab<ElemSize, Alignment>`). Align 4/8 use the 16-byte alignment class.

```
Alignment class 16: 16 … 4096 (16 rungs; engines eager, Blocks may map on demand)
Alignment class 32: 32 … 8192
Alignment class 64: 64 … 16384
```

Beyond an alignment-class max → `SlabProxy` (always 64-byte aligned `new[]`).

Intermediate sizes (48, 96, 192, 384, 768) reduce internal fragmentation. Slabs are selected by **total footprint** (requested size + allocation header), not the user size alone — see [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|------------|-------|
| Allocation | O(1) amortized | Two-level bitmap optimization |
| Deallocation | O(log n) | Address map lookup (n = Blocks in that Slab) |
| SpinLock lock/unlock | O(1) | No contention case |

**Space Overhead**: Header width is `header_size = max(8, alignment)`. With the
default alignment (`kDefaultAlignment` / `__STDCPP_DEFAULT_NEW_ALIGNMENT__`,
typically 16) that is a **16-byte** header — ~12.5% for a 128-byte user request
before bitset amortization. Explicit `allocate(N, 8)` still uses an 8-byte
header. Details in [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md#memory-layout).

## Requirements

- **Minimum**: C++20 with a working `<format>`, `std::ranges`, and `atomic_flag::wait` / `notify_one`
- **Dialect**: `./configure` selects the newest of C++26 / C++23 / C++20 the compiler accepts (`--with-cxxstd=` to pin)
- **`std::print`**: used when `__cpp_lib_print` is set; otherwise a `std::format`-based shim in `helper.hpp`
- **Compilers**: GCC 11+, Clang 18+ recommended for full dialect probing (older may work at C++20)
- **Dependencies**: Google Test (for unit tests)

## Documentation

| Doc | Role |
|-----|------|
| **[docs/API.md](docs/API.md)** | Public API reference: contracts, thread-safety, short usage snippets (including helpers) |
| **[docs/CONCEPTS.md](docs/CONCEPTS.md)** | Conceptual / learning-oriented guide (pools, slabs, lifetime patterns, teaching path) |
| **[docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)** | Architecture, design trade-offs, and production readiness |
| **[docs/terminology-glossary.md](docs/terminology-glossary.md)** | Locked Pool / Slab / Block / Slot vocabulary (Phase 1+) |
| **[SpinLock Analysis](mlvlib/docs/deepdive/SpinLock_Analysis.md)** | SpinLock correctness and performance deep dive (linked from IMPLEMENTATION) |

The repo-root [`IMPLEMENTATION.md`](IMPLEMENTATION.md) redirects here. Source headers also carry design comments.

## Testing

The suite in `test/tester.cpp` covers slab growth/trim, size-class routing, header validation (including interior frees), thread safety, smart-pointer exception safety, `PoolAllocator` / STL integration, and `PoolMemoryResource` / `std::pmr`. SpinLock and LifetimeObserver tests live in mlvlib.

```bash
make test
# equivalent:
make && ./obj/addrsan/tester
```

CI (`.github/workflows/c-cpp.yml`) runs the full suite for **release** (`-O3 -DNDEBUG`, plus coverage), **addrsan**, and **threadsan**, on both clang and gcc. Locally:

```bash
./configure --build-type=release --enable-tests && make test
```

## Future Work

- Per-class metrics breakdown (PoolStats counters/HWMs shipped)
- Configurable SpinLock parameters

### Research / design backlog

Recorded for later (not scheduled). See also
[docs/IMPLEMENTATION.md — Research / design backlog](docs/IMPLEMENTATION.md#research--design-backlog).

- Consider dropping fixed size-class slots in favor of shaping buffers to
  `(element size + header) × N` (or similar multiples)
- Dedicated bump / size-based arena facility — **deferred** (use arena `~Pool`
  for bulk reclaim today; see IMPLEMENTATION backlog for revival shape)
- Consider a C99-compliant wrapper API for callers that want the pool interface
  from C (opaque handle ≈ `PoolHandle`)
- Abstract the lock primitive so a build can choose `SpinLock` or another
  standard lock (e.g. `std::mutex`) at compile time

## References

- [Bonwick (1994) - The Slab Allocator](https://www.usenix.org/legacy/publications/library/proceedings/bos94/full_papers/bonwick.ps) - Original USENIX paper
- [Linux SLAB/SLUB/SLOB](https://www.kernel.org/doc/gorman/html/understand/understand011.html) - Kernel implementations
- [memcached](https://memcached.org/) - Popularized slab allocation for user-space
- Patrice Roy - *C++ Memory Management* (Packt, 2025) - arenas, deferred reclamation, allocators/PMR; strategy mapping in [docs/CONCEPTS.md](docs/CONCEPTS.md#choosing-a-memory-strategy)

## License

BSD 3-Clause License — Copyright (c) 2025-2026, Michael VanLoon.

See [`LICENSE`](LICENSE) for the full text (also referenced via SPDX in headers).

## Contributing

Production quality and clarity are the same bar. Contributions should:
- Prefer simple designs that stay correct under concurrency and failure
- Avoid obscurely complex micro-optimizations, preferring simplicity and clarity
- Always be mindful of performance impact where it is compatible with simplicity and clarity
- Explain *why* in comments and docs, not just *what*; keep claims aligned with the code
- Add or update tests for every behavior change
- Document design, intention, and tradeoffs

### Git / releases

- Prefer **rebase merge** into `main` so each landed commit stays a Conventional Commit (`feat:`, `fix:`, `perf:`, `docs:`, `chore:`, …). CI runs commitlint on PRs.
- Do **not** push from `main`/`master`, and do not push updates to `refs/heads/main`. Trunk advances via pull request only.
- Canonical SemVer is cut by **semantic-release** on `main` only (GitHub Release + `v*` tag + `spallocator/version.hpp`). Dev/PR identity: `git describe --tags --match 'v*' --always`.
- Node / semantic-release / commitlint live in the shared **[MLV-WIP/release-tooling](https://github.com/MLV-WIP/release-tooling)** repo; this project only has thin workflow wrappers and project-specific `scripts/set-version.py`.

Enable local hooks:

```bash
git config core.hooksPath .githooks
# Optional: clone release-tooling next to this repo for offline commitlint config
# git clone git@github.com:MLV-WIP/release-tooling.git ../release-tooling
```

`.githooks/pre-push` aborts if `HEAD` is trunk or if the push destination is trunk (including when a feature branch’s upstream is wrongly `origin/main`).  
`.githooks/commit-msg` runs commitlint using the shared release-tooling config.

## AI Assistance Declaration

README and documentation generated with AI assistance (Claude Code, Grok, Cursor, GitHub Copilot).

Architecture, design, and core implementation are human-generated. Initial
implementation exclusively by Michael VanLoon. Standard library
adaptations (make_unique and make_shared support, along with standard allocator
support) also developed with AI assistance, but in all cases, all code has
been validated, reviewed, and forged by human hands and inspired by human
software development experience.
