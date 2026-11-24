/*
 * On-demand allocator comparison harness (not part of make test).
 *
 * Prefer: make bench BUILD_TYPE=release
 * Profile: make profile BENCH_ARGS='--scenario=lifo --backend=slab16'
 *
 * Compares ::operator new, spallocator::Pool, std::pmr pool resources, and
 * (where applicable) bare Slab. Reports ns/op and ratio vs ::new.
 *
 * Usage:
 *   bench_allocators [--align=8|16|32|64] [--iters=N] [--threads=N]
 *                   [--scenario=...] [--backend=...] [--help]
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "spallocator/pool.hpp"
#include "spallocator/pool_pmr.hpp"
#include "spallocator/impl/slab.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

// Prevent the compiler from eliding timed allocations. Thread-local so
// multi-thread runs do not contend on a shared sink.
inline void sink_ptr(void* p) noexcept
{
    thread_local volatile std::uintptr_t sink = 0;
    sink = sink ^ reinterpret_cast<std::uintptr_t>(p);
}

inline constexpr std::size_t kUserSizeMin = 16;
inline constexpr std::size_t kUserSizeMax = 1024;

// Default per-thread live batch for --scenario=threaded (peak live ≈ threads×batch).
// 1024 favors multi-buffer growth across several size classes under --size-mix=class.
inline constexpr int kDefaultThreadedBatch = 1024;

enum class SizeMix
{
    Uniform, // uniform user bytes in [kUserSizeMin, kUserSizeMax]
    Class,   // uniform over reachable alignment-class-16 size classes (exact-fit user size)
};

// Per-worker std::minstd_rand (fast LCG from <random>). Deterministic seed per
// worker index so runs are repeatable and threads do not share RNG state.
inline constexpr std::uint32_t kBenchRngSeedKey = 0x51AB0001u;

inline std::minstd_rand make_bench_rng(unsigned worker_index = 0)
{
    // Mix seed key with worker index so nearby workers get distinct streams.
    std::uint32_t z = kBenchRngSeedKey + 0x9E3779B9u * (worker_index + 1u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    z ^= (z >> 16);
    if (z == 0)
    {
        z = 0xA5A5A5A5u; // minstd_rand rejects seed 0
    }
    return std::minstd_rand{z};
}

inline std::size_t pool_header_size(std::size_t alignment)
{
    return alignment < 8 ? 8 : alignment;
}

// Exact-fit user sizes that land on each reachable native alignment-class ladder when
// footprint = user + header. With default align 16 (header 16) and user ≥ 16,
// class 16 is unreachable; classes 32..1024 are.
inline std::size_t pick_user_size(std::minstd_rand& rng, SizeMix mix,
                                 std::size_t alignment)
{
    if (mix == SizeMix::Uniform)
    {
        std::uniform_int_distribution<std::size_t> dist(kUserSizeMin, kUserSizeMax);
        return dist(rng);
    }

    const std::size_t header = pool_header_size(alignment);
    // Mirror spallocator::kLadder16; keep local to avoid pulling ladder internals
    // into the bench beyond what Pool already documents.
    static constexpr std::size_t kLadder[] = {
        16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};

    std::size_t choices[sizeof(kLadder) / sizeof(kLadder[0])];
    std::size_t n = 0;
    for (std::size_t cls : kLadder)
    {
        if (cls <= header)
        {
            continue;
        }
        const std::size_t user = cls - header;
        if (user >= kUserSizeMin && user <= kUserSizeMax)
        {
            choices[n++] = user;
        }
    }
    if (n == 0)
    {
        return kUserSizeMin;
    }
    std::uniform_int_distribution<std::size_t> dist(0, n - 1);
    return choices[dist(rng)];
}

char const* size_mix_label(SizeMix m)
{
    return m == SizeMix::Class ? "class" : "uniform";
}

void print_pool_stats(std::string_view label, spallocator::Pool& pool)
{
    spallocator::PoolStats s = pool.stats();
    std::cout << "  [PoolStats] " << label << "\n"
              << "    live_allocations=" << s.live_allocations
              << " live_hwm=" << s.live_hwm << "\n"
              << "    retained_mapped_bytes=" << s.retained_mapped_bytes
              << " retained_hwm=" << s.retained_hwm << "\n"
              << "    alloc_ok=" << s.alloc_ok
              << " alloc_fail=" << s.alloc_fail
              << " free_ok=" << s.free_ok
              << " free_fail=" << s.free_fail;
    if (!spallocator::kPoolCountersEnabled)
    {
        std::cout << " (counters off; -DSPALLOCATOR_POOL_COUNTERS=1 to enable)";
    }
    std::cout << "\n";
}

void print_slab16_stats(std::string_view label, spallocator::Slab<16, 16>& slab)
{
    std::cout << "  [Slab<16,16>] " << label << "\n"
              << "    slab_count=" << slab.getBlockCount()
              << " retained_bytes=" << slab.getAllocatedMemory()
              << " availability_map_bytes=" << slab.getAvailabilityMapBytes()
              << "\n";
}

enum class Scenario : unsigned
{
    None = 0,
    Lifo = 1u << 0,
    Multi = 1u << 1,
    Bulk = 1u << 2,
    Threaded = 1u << 3,
    All = Lifo | Multi | Bulk | Threaded,
};

enum class BackendId : unsigned
{
    None = 0,
    New = 1u << 0,
    Pool = 1u << 1,
    PmrUnsync = 1u << 2,
    PmrSync = 1u << 3,
    Slab16 = 1u << 4,
    PoolPmr = 1u << 5,
    All = New | Pool | PmrUnsync | PmrSync | Slab16 | PoolPmr,
};

inline Scenario operator|(Scenario a, Scenario b)
{
    return static_cast<Scenario>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
inline Scenario& operator|=(Scenario& a, Scenario b)
{
    a = a | b;
    return a;
}
inline bool has(Scenario mask, Scenario bit)
{
    return (static_cast<unsigned>(mask) & static_cast<unsigned>(bit)) != 0;
}

inline BackendId operator|(BackendId a, BackendId b)
{
    return static_cast<BackendId>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
inline BackendId& operator|=(BackendId& a, BackendId b)
{
    a = a | b;
    return a;
}
inline bool has(BackendId mask, BackendId bit)
{
    return (static_cast<unsigned>(mask) & static_cast<unsigned>(bit)) != 0;
}

bool parse_scenario_list(std::string_view csv, Scenario& out, std::string& err)
{
    out = Scenario::None;
    std::size_t start = 0;
    while (start <= csv.size())
    {
        std::size_t comma = csv.find(',', start);
        std::string_view tok =
            csv.substr(start, comma == std::string_view::npos ? csv.size() - start : comma - start);
        if (tok.empty())
        {
            err = "empty --scenario token";
            return false;
        }
        if (tok == "all")
        {
            out = Scenario::All;
        }
        else if (tok == "lifo")
        {
            out |= Scenario::Lifo;
        }
        else if (tok == "multi")
        {
            out |= Scenario::Multi;
        }
        else if (tok == "bulk")
        {
            out |= Scenario::Bulk;
        }
        else if (tok == "threaded")
        {
            out |= Scenario::Threaded;
        }
        else
        {
            err = "unknown --scenario value: " + std::string(tok);
            return false;
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    if (out == Scenario::None)
    {
        err = "--scenario selected nothing";
        return false;
    }
    return true;
}

bool parse_backend_list(std::string_view csv, BackendId& out, std::string& err)
{
    out = BackendId::None;
    std::size_t start = 0;
    while (start <= csv.size())
    {
        std::size_t comma = csv.find(',', start);
        std::string_view tok =
            csv.substr(start, comma == std::string_view::npos ? csv.size() - start : comma - start);
        if (tok.empty())
        {
            err = "empty --backend token";
            return false;
        }
        if (tok == "all")
        {
            out = BackendId::All;
        }
        else if (tok == "new")
        {
            out |= BackendId::New;
        }
        else if (tok == "pool")
        {
            out |= BackendId::Pool;
        }
        else if (tok == "pmr_unsync")
        {
            out |= BackendId::PmrUnsync;
        }
        else if (tok == "pmr_sync")
        {
            out |= BackendId::PmrSync;
        }
        else if (tok == "slab16")
        {
            out |= BackendId::Slab16;
        }
        else if (tok == "pool_pmr")
        {
            out |= BackendId::PoolPmr;
        }
        else
        {
            err = "unknown --backend value: " + std::string(tok);
            return false;
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    if (out == BackendId::None)
    {
        err = "--backend selected nothing";
        return false;
    }
    return true;
}

char const* scenario_label(Scenario s)
{
    if (s == Scenario::All)
    {
        return "all";
    }
    // Compact banner for common single/multi selections.
    static thread_local std::string buf;
    buf.clear();
    auto append = [&](char const* name) {
        if (!buf.empty())
        {
            buf += ',';
        }
        buf += name;
    };
    if (has(s, Scenario::Lifo))
    {
        append("lifo");
    }
    if (has(s, Scenario::Multi))
    {
        append("multi");
    }
    if (has(s, Scenario::Bulk))
    {
        append("bulk");
    }
    if (has(s, Scenario::Threaded))
    {
        append("threaded");
    }
    return buf.c_str();
}

char const* backend_label(BackendId b)
{
    if (b == BackendId::All)
    {
        return "all";
    }
    static thread_local std::string buf;
    buf.clear();
    auto append = [&](char const* name) {
        if (!buf.empty())
        {
            buf += ',';
        }
        buf += name;
    };
    if (has(b, BackendId::New))
    {
        append("new");
    }
    if (has(b, BackendId::Pool))
    {
        append("pool");
    }
    if (has(b, BackendId::PmrUnsync))
    {
        append("pmr_unsync");
    }
    if (has(b, BackendId::PmrSync))
    {
        append("pmr_sync");
    }
    if (has(b, BackendId::Slab16))
    {
        append("slab16");
    }
    if (has(b, BackendId::PoolPmr))
    {
        append("pool_pmr");
    }
    return buf.c_str();
}

struct Options
{
    std::size_t alignment = spallocator::kDefaultAlignment;
    int iters = 300000;
    int threads = 1;
    int batch = kDefaultThreadedBatch;
    SizeMix size_mix = SizeMix::Class;
    Scenario scenarios = Scenario::All;
    BackendId backends = BackendId::All;
    bool pool_stats = false; // opt-in [PoolStats] / slab retained dumps
    bool help = false;
};

Options parse_args(int argc, char** argv)
{
    Options opt;
#if !defined(NDEBUG)
    opt.iters = 50000; // keep sanitizer/debug runs snappy
#endif
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        if (a == "--help" || a == "-h")
        {
            opt.help = true;
        }
        else if (a.rfind("--align=", 0) == 0)
        {
            opt.alignment = static_cast<std::size_t>(std::strtoull(a.data() + 8, nullptr, 10));
        }
        else if (a.rfind("--iters=", 0) == 0)
        {
            opt.iters = static_cast<int>(std::strtol(a.data() + 8, nullptr, 10));
        }
        else if (a.rfind("--threads=", 0) == 0)
        {
            opt.threads = static_cast<int>(std::strtol(a.data() + 10, nullptr, 10));
        }
        else if (a.rfind("--batch=", 0) == 0)
        {
            opt.batch = static_cast<int>(std::strtol(a.data() + 8, nullptr, 10));
        }
        else if (a.rfind("--size-mix=", 0) == 0)
        {
            std::string_view v = a.substr(11);
            if (v == "uniform")
            {
                opt.size_mix = SizeMix::Uniform;
            }
            else if (v == "class")
            {
                opt.size_mix = SizeMix::Class;
            }
            else
            {
                std::cerr << "--size-mix must be uniform or class\n";
                opt.help = true;
            }
        }
        else if (a.rfind("--scenario=", 0) == 0)
        {
            std::string err;
            if (!parse_scenario_list(a.substr(11), opt.scenarios, err))
            {
                std::cerr << err << "\n";
                opt.help = true;
            }
        }
        else if (a.rfind("--backend=", 0) == 0)
        {
            std::string err;
            if (!parse_backend_list(a.substr(10), opt.backends, err))
            {
                std::cerr << err << "\n";
                opt.help = true;
            }
        }
        else if (a == "--pool-stats")
        {
            opt.pool_stats = true;
        }
        else
        {
            std::cerr << "unknown argument: " << a << "\n";
            opt.help = true;
        }
    }
    if (opt.iters < 1)
    {
        opt.iters = 1;
    }
    if (opt.threads < 1)
    {
        opt.threads = 1;
    }
    if (opt.batch < 1)
    {
        opt.batch = 1;
    }
    if (opt.alignment != 8 && opt.alignment != 16 && opt.alignment != 32 &&
        opt.alignment != 64)
    {
        std::cerr << "--align must be 8|16 (full suite; default "
                  << spallocator::kDefaultAlignment
                  << ") or 32|64 (Pool vs ::new spot-check)\n";
        opt.help = true;
    }
    return opt;
}

void print_help()
{
    std::cout <<
        "bench_allocators - compare allocation backends (on-demand; not a CI gate)\n"
        "\n"
        "Options:\n"
        "  --align=8|16|32|64  Payload alignment (default kDefaultAlignment, typically 16).\n"
        "                      8|16 run the full suite; 32|64 are Pool vs ::new spot-checks.\n"
        "  --iters=N           Timed alloc/free pairs per scenario (default release 300000)\n"
        "  --threads=N         Worker threads for multi-thread multi-size churn (default 1)\n"
        "  --batch=N           Per-thread live batch for threaded scenario (default 1024)\n"
        "  --size-mix=MIX      uniform | class (default class). Class = equal weight per\n"
        "                      reachable alignment-class-16 size class (exact-fit user size).\n"
        "  --scenario=LIST     lifo,multi,bulk,threaded,all (comma-separated; default all)\n"
        "  --backend=LIST      new,pool,pmr_unsync,pmr_sync,slab16,pool_pmr,all\n"
        "                      (comma-separated; default all)\n"
        "  --pool-stats        Print PoolStats / slab retained dumps after scenarios\n"
        "  --help              Show this help\n"
        "\n"
        "Filters skip inapplicable combinations with a note (e.g. slab16 outside\n"
        "lifo/bulk; pmr_sync outside threaded; threaded needs --threads=N with N>1).\n"
        "Bulk alloc-then-free-all grows many slab buffers (multi-buffer free uses the map).\n"
        "Threaded: shared allocator + start barrier (contention); peak live ≈ threads×batch\n"
        "so multi-buffer growth needs a large batch (default 1024) and --size-mix=class (default).\n"
        "When ::new is omitted, tables print ns/op only (no vs ::new ratio).\n"
        "\n"
        "Per-thread std::minstd_rand (seed key + worker index).\n"
        "Recommended: make bench BUILD_TYPE=release\n"
        "Profiling:   make profile BENCH_ARGS='--scenario=lifo --backend=slab16 --iters=2000000'\n"
        "Threaded:    make bench BUILD_TYPE=release BENCH_ARGS='--scenario=threaded --threads=4'\n";
}

void print_banner(Options const& opt)
{
    std::cout << "spallocator allocator bench\n";
    std::cout << "  alignment: " << opt.alignment << "\n";
    std::cout << "  iters:     " << opt.iters << "\n";
    std::cout << "  threads:   " << opt.threads << "\n";
    std::cout << "  batch:     " << opt.batch << " (threaded peak live ≈ threads×batch)\n";
    std::cout << "  size-mix:  " << size_mix_label(opt.size_mix) << "\n";
    std::cout << "  scenario:  " << scenario_label(opt.scenarios) << "\n";
    std::cout << "  backend:   " << backend_label(opt.backends) << "\n";
#if defined(__clang__)
    std::cout << "  compiler:  clang " << __clang_major__ << "." << __clang_minor__
              << "." << __clang_patchlevel__ << "\n";
#elif defined(__GNUC__)
    std::cout << "  compiler:  gcc " << __GNUC__ << "." << __GNUC_MINOR__
              << "." << __GNUC_PATCHLEVEL__ << "\n";
#else
    std::cout << "  compiler:  unknown\n";
#endif
#if defined(NDEBUG)
    std::cout << "  build:     NDEBUG (prefer this for ratios)\n";
#else
    std::cout << "  build:     debug/sanitizer (ratios are not comparable to release)\n";
#endif
    std::cout << "\n";
}

struct Timing
{
    double ns_per_op = 0.0;
    std::int64_t total_ns = 0;
};

Timing make_timing(clock_type::duration d, int ops)
{
    Timing t;
    t.total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    t.ns_per_op = ops > 0 ? static_cast<double>(t.total_ns) / static_cast<double>(ops) : 0.0;
    return t;
}

// --- Backends ----------------------------------------------------------------

struct NewDeleteBackend
{
    static constexpr const char* name = "::operator new";

    void* allocate(std::size_t n, std::size_t align)
    {
        return ::operator new(n, std::align_val_t{align});
    }

    void deallocate(void* p, std::size_t /*n*/, std::size_t align) noexcept
    {
        ::operator delete(p, std::align_val_t{align});
    }
};

struct PoolBackend
{
    static constexpr const char* name = "spallocator::Pool";

    spallocator::Pool pool;

    void* allocate(std::size_t n, std::size_t align)
    {
        return pool.allocate(n, align);
    }

    void deallocate(void* p, std::size_t /*n*/, std::size_t /*align*/)
    {
        pool.deallocate(static_cast<std::byte*>(p));
    }
};

struct PmrUnsyncBackend
{
    static constexpr const char* name = "pmr::unsynchronized_pool";

    std::pmr::unsynchronized_pool_resource resource;

    void* allocate(std::size_t n, std::size_t align)
    {
        return resource.allocate(n, align);
    }

    void deallocate(void* p, std::size_t n, std::size_t align) noexcept
    {
        resource.deallocate(p, n, align);
    }
};

struct PmrSyncBackend
{
    static constexpr const char* name = "pmr::synchronized_pool";

    std::pmr::synchronized_pool_resource resource;

    void* allocate(std::size_t n, std::size_t align)
    {
        return resource.allocate(n, align);
    }

    void deallocate(void* p, std::size_t n, std::size_t align) noexcept
    {
        resource.deallocate(p, n, align);
    }
};

// Pool behind std::pmr::memory_resource (virtual dispatch tax vs raw Pool).
struct PoolPmrBackend
{
    static constexpr const char* name = "spallocator::pool_pmr";

    spallocator::Pool pool;
    spallocator::PoolMemoryResource resource{pool};

    void* allocate(std::size_t n, std::size_t align)
    {
        return resource.allocate(n, align);
    }

    void deallocate(void* p, std::size_t n, std::size_t align) noexcept
    {
        resource.deallocate(p, n, align);
    }
};

// Bare size-class engine (16B slots). Only meaningful for size==16, align<=16.
struct Slab16Backend
{
    static constexpr const char* name = "Slab<16,16>";

    spallocator::Slab<16, 16> slab;

    void* allocate(std::size_t n, std::size_t /*align*/)
    {
        if (n > 16)
        {
            throw std::invalid_argument("Slab16Backend only supports n<=16");
        }
        return slab.allocateItem(16);
    }

    void deallocate(void* p, std::size_t /*n*/, std::size_t /*align*/)
    {
        slab.deallocateItem(static_cast<std::byte*>(p));
    }
};

// --- Workloads ---------------------------------------------------------------

template <class Backend>
void warm_lifo(Backend& b, std::size_t size, std::size_t align, int warm_count)
{
    std::vector<void*> live;
    live.reserve(static_cast<std::size_t>(warm_count));
    for (int i = 0; i < warm_count; ++i)
    {
        live.push_back(b.allocate(size, align));
    }
    for (void* p : live)
    {
        b.deallocate(p, size, align);
    }
}

template <class Backend>
Timing timed_lifo_churn(Backend& b, std::size_t size, std::size_t align, int iters)
{
    // One live slot recycled LIFO-style (steady-state recycle).
    warm_lifo(b, size, align, 256);
    const auto t0 = clock_type::now();
    for (int i = 0; i < iters; ++i)
    {
        void* p = b.allocate(size, align);
        sink_ptr(p);
        b.deallocate(p, size, align);
    }
    const auto t1 = clock_type::now();
    return make_timing(t1 - t0, iters);
}

template <class Backend>
void warm_random_sizes(Backend& b, std::size_t align, std::minstd_rand& rng,
                       SizeMix mix, int warm_count)
{
    std::vector<std::pair<void*, std::size_t>> live;
    live.reserve(static_cast<std::size_t>(warm_count));
    for (int i = 0; i < warm_count; ++i)
    {
        std::size_t s = pick_user_size(rng, mix, align);
        live.emplace_back(b.allocate(s, align), s);
    }
    for (auto& [p, s] : live)
    {
        b.deallocate(p, s, align);
    }
}

template <class Backend>
Timing timed_multi_size_churn(Backend& b, std::size_t align, SizeMix mix, int iters,
                              unsigned worker_index = 0)
{
    std::minstd_rand rng = make_bench_rng(worker_index);
    warm_random_sizes(b, align, rng, mix, 256);
    const auto t0 = clock_type::now();
    for (int i = 0; i < iters; ++i)
    {
        std::size_t s = pick_user_size(rng, mix, align);
        void* p = b.allocate(s, align);
        sink_ptr(p);
        b.deallocate(p, s, align);
    }
    const auto t1 = clock_type::now();
    return make_timing(t1 - t0, iters);
}

template <class Backend>
std::pair<Timing, Timing> timed_alloc_then_free_all(Backend& b, std::size_t size,
                                                    std::size_t align, int count)
{
    warm_lifo(b, size, align, 64);
    std::vector<void*> ptrs(static_cast<std::size_t>(count));

    const auto a0 = clock_type::now();
    for (int i = 0; i < count; ++i)
    {
        ptrs[static_cast<std::size_t>(i)] = b.allocate(size, align);
        sink_ptr(ptrs[static_cast<std::size_t>(i)]);
    }
    const auto a1 = clock_type::now();

    const auto f0 = clock_type::now();
    for (int i = count - 1; i >= 0; --i)
    {
        b.deallocate(ptrs[static_cast<std::size_t>(i)], size, align);
    }
    const auto f1 = clock_type::now();

    return {make_timing(a1 - a0, count), make_timing(f1 - f0, count)};
}

template <class Backend>
Timing timed_threaded_multi_size_churn(Backend& shared, std::size_t align, SizeMix mix,
                                       int total_iters, int threads, int batch)
{
    {
        std::minstd_rand warm_rng = make_bench_rng(0);
        warm_random_sizes(shared, align, warm_rng, mix, 256);
    }
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    // Each worker holds its own batch of live pointers (not shared across threads).
    // Large batch → peak live ≈ threads×batch for multi-buffer growth under mix=class.
    const std::size_t kBatch = static_cast<std::size_t>(batch);
    const int per = total_iters / threads / batch;
    if (per < 1)
    {
        std::cerr << "threaded: iters too small for threads×batch; "
                     "raise --iters or lower --threads/--batch\n";
        return {};
    }

    const auto t0 = clock_type::now();
    for (int t = 0; t < threads; ++t)
    {
        const unsigned worker_index = static_cast<unsigned>(t);
        workers.emplace_back([&shared, &ready, align, mix, per, threads, kBatch,
                              worker_index] {
            // Per-thread RNG: same seed key every process run; distinct stream
            // per worker_index (not std::thread::id - that is not stable).
            std::minstd_rand rng = make_bench_rng(worker_index);
            std::vector<std::pair<void*, std::size_t>> batch_ptrs(kBatch);

            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < threads)
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < per; ++i)
            {
                for (std::size_t j = 0; j < kBatch; ++j)
                {
                    std::size_t s = pick_user_size(rng, mix, align);
                    batch_ptrs[j].first = shared.allocate(s, align);
                    batch_ptrs[j].second = s;
                    sink_ptr(batch_ptrs[j].first);
                }
                // Reverse free; use signed index so j >= 0 is well-defined
                // (size_t would wrap forever after j hits 0).
                for (int j = static_cast<int>(kBatch) - 1; j >= 0; --j)
                {
                    auto& slot = batch_ptrs[static_cast<std::size_t>(j)];
                    shared.deallocate(slot.first, slot.second, align);
                    slot.first = nullptr;
                }
            }
        });
    }
    for (auto& th : workers)
    {
        th.join();
    }
    const auto t1 = clock_type::now();
    // ops = alloc+free pairs (each outer iter does kBatch of them).
    return make_timing(t1 - t0, per * threads * batch);
}

struct Row
{
    std::string backend;
    Timing timing;
    bool is_new_baseline = false;
};

void print_table(std::string_view title, std::vector<Row> const& rows)
{
    std::cout << title << "\n";
    if (rows.empty())
    {
        std::cout << "  (no rows)\n\n";
        return;
    }
    double baseline = 0.0;
    bool have_baseline = false;
    for (auto const& r : rows)
    {
        if (r.is_new_baseline)
        {
            baseline = r.timing.ns_per_op;
            have_baseline = true;
            break;
        }
    }
    std::cout << std::left << std::setw(28) << "  backend"
              << std::right << std::setw(14) << "ns/op"
              << std::setw(12) << "vs ::new"
              << "\n";
    for (auto const& r : rows)
    {
        std::cout << std::left << std::setw(28) << ("  " + r.backend)
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(14) << r.timing.ns_per_op;
        if (have_baseline && baseline > 0.0)
        {
            std::cout << std::setprecision(3) << std::setw(12)
                      << (r.timing.ns_per_op / baseline);
        }
        else
        {
            std::cout << std::setw(12) << "n/a";
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

template <class Backend>
Row row_from(Backend& /*b*/, Timing t, bool is_new_baseline = false)
{
    return Row{Backend::name, t, is_new_baseline};
}

void note_skip(std::string_view why)
{
    std::cout << "  skip: " << why << "\n";
}

void run_default_align_suite(Options const& opt)
{
    constexpr std::size_t kSize = 16;
    // Cap bulk live-set size (pointer vector + retained slabs). Default packing
    // is 512 slots/buffer for Slab<16,16>, so counts >> 512 exercise multi-buffer.
    const int bulk_count = std::min(opt.iters, 500000);
    const BackendId be = opt.backends;

    // --- Warmed LIFO churn (16B) ---
    if (has(opt.scenarios, Scenario::Lifo))
    {
        std::vector<Row> rows;
        PoolBackend pool_b;
        PoolPmrBackend pool_pmr_b;
        Slab16Backend slab_b;
        bool ran_pool = false;
        bool ran_pool_pmr = false;
        bool ran_slab = false;

        if (has(be, BackendId::New))
        {
            NewDeleteBackend new_b;
            rows.push_back(row_from(
                new_b, timed_lifo_churn(new_b, kSize, opt.alignment, opt.iters), true));
        }
        if (has(be, BackendId::Pool))
        {
            rows.push_back(row_from(
                pool_b, timed_lifo_churn(pool_b, kSize, opt.alignment, opt.iters)));
            ran_pool = true;
        }
        if (has(be, BackendId::PoolPmr))
        {
            rows.push_back(row_from(
                pool_pmr_b,
                timed_lifo_churn(pool_pmr_b, kSize, opt.alignment, opt.iters)));
            ran_pool_pmr = true;
        }
        if (has(be, BackendId::PmrUnsync))
        {
            PmrUnsyncBackend pmr_b;
            rows.push_back(row_from(
                pmr_b, timed_lifo_churn(pmr_b, kSize, opt.alignment, opt.iters)));
        }
        if (has(be, BackendId::PmrSync))
        {
            note_skip("pmr_sync not used in single-thread LIFO (use --scenario=threaded)");
        }
        if (has(be, BackendId::Slab16))
        {
            rows.push_back(row_from(
                slab_b, timed_lifo_churn(slab_b, kSize, opt.alignment, opt.iters)));
            ran_slab = true;
        }
        print_table("Warmed LIFO churn (size=16, alloc+free pair)", rows);
        if (opt.pool_stats)
        {
            if (ran_pool)
            {
                print_pool_stats("after LIFO churn", pool_b.pool);
            }
            if (ran_pool_pmr)
            {
                print_pool_stats("pool_pmr after LIFO churn", pool_pmr_b.pool);
            }
            if (ran_slab)
            {
                print_slab16_stats("after LIFO churn", slab_b.slab);
            }
        }
        std::cout << "\n";
    }

    // --- Multi-size mix (uniform random user sizes in [16, 1024]) ---
    if (has(opt.scenarios, Scenario::Multi))
    {
        std::vector<Row> rows;
        PoolBackend pool_b;
        PoolPmrBackend pool_pmr_b;
        bool ran_pool = false;
        bool ran_pool_pmr = false;

        if (has(be, BackendId::New))
        {
            NewDeleteBackend new_b;
                rows.push_back(row_from(
                new_b,
                timed_multi_size_churn(new_b, opt.alignment, opt.size_mix, opt.iters),
                true));
        }
        if (has(be, BackendId::Pool))
        {
            rows.push_back(row_from(
                pool_b,
                timed_multi_size_churn(pool_b, opt.alignment, opt.size_mix, opt.iters)));
            ran_pool = true;
        }
        if (has(be, BackendId::PoolPmr))
        {
            rows.push_back(row_from(
                pool_pmr_b,
                timed_multi_size_churn(
                    pool_pmr_b, opt.alignment, opt.size_mix, opt.iters)));
            ran_pool_pmr = true;
        }
        if (has(be, BackendId::PmrUnsync))
        {
            PmrUnsyncBackend pmr_b;
            rows.push_back(row_from(
                pmr_b,
                timed_multi_size_churn(pmr_b, opt.alignment, opt.size_mix, opt.iters)));
        }
        if (has(be, BackendId::PmrSync))
        {
            note_skip("pmr_sync not used in single-thread multi (use --scenario=threaded)");
        }
        if (has(be, BackendId::Slab16))
        {
            note_skip("slab16 only supports size<=16 (use --scenario=lifo or bulk)");
        }
        print_table(
            std::string("Multi-size mix (size-mix=") + size_mix_label(opt.size_mix) +
                ", alloc+free pair)",
            rows);
        if (opt.pool_stats)
        {
            if (ran_pool)
            {
                print_pool_stats("after multi-size mix", pool_b.pool);
            }
            if (ran_pool_pmr)
            {
                print_pool_stats("pool_pmr after multi-size mix", pool_pmr_b.pool);
            }
        }
        std::cout << "\n";
    }

    // --- Alloc-all then free-all (grows many slab buffers; not single-slot churn) ---
    if (has(opt.scenarios, Scenario::Bulk))
    {
        if (has(be, BackendId::PmrSync))
        {
            note_skip("pmr_sync omitted from alloc-then-free-all (use --scenario=threaded)");
        }

        const bool want_new = has(be, BackendId::New);
        const bool want_pool = has(be, BackendId::Pool);
        const bool want_pool_pmr = has(be, BackendId::PoolPmr);
        const bool want_pmr = has(be, BackendId::PmrUnsync);
        const bool want_slab = has(be, BackendId::Slab16);
        if (!want_new && !want_pool && !want_pool_pmr && !want_pmr && !want_slab)
        {
            std::cout << "Alloc-then-free-all skipped (no applicable backends selected).\n\n";
        }
        else
        {
            // Slab<16,16> holds 512 slots/buffer at default packing; count>>512
            // forces multi-buffer growth (and map-backed free when m>1).
            std::cout << "Alloc-then-free-all (size=16, count=" << bulk_count
                      << "; multi-buffer growth)\n";
            std::cout << std::left << std::setw(28) << "  backend"
                      << std::right << std::setw(14) << "alloc ns/op"
                      << std::setw(14) << "free ns/op"
                      << std::setw(12) << "alloc vs"
                      << std::setw(12) << "free vs"
                      << "\n";

            Timing base_a{};
            Timing base_f{};
            bool have_baseline = false;
            if (want_new)
            {
                NewDeleteBackend baseline_b;
                auto pair =
                    timed_alloc_then_free_all(baseline_b, kSize, opt.alignment, bulk_count);
                base_a = pair.first;
                base_f = pair.second;
                have_baseline = true;
            }

            auto print_bulk = [&](char const* name, Timing a, Timing f) {
                std::cout << std::left << std::setw(28) << (std::string("  ") + name)
                          << std::right << std::fixed << std::setprecision(1)
                          << std::setw(14) << a.ns_per_op
                          << std::setw(14) << f.ns_per_op;
                if (have_baseline && base_a.ns_per_op > 0.0 && base_f.ns_per_op > 0.0)
                {
                    std::cout << std::setprecision(3)
                              << std::setw(12) << (a.ns_per_op / base_a.ns_per_op)
                              << std::setw(12) << (f.ns_per_op / base_f.ns_per_op);
                }
                else
                {
                    std::cout << std::setw(12) << "n/a" << std::setw(12) << "n/a";
                }
                std::cout << "\n";
            };

            if (want_new)
            {
                print_bulk(NewDeleteBackend::name, base_a, base_f);
            }
            PoolBackend pool_b;
            if (want_pool)
            {
                auto [a, f] =
                    timed_alloc_then_free_all(pool_b, kSize, opt.alignment, bulk_count);
                print_bulk(PoolBackend::name, a, f);
                if (opt.pool_stats)
                {
                    print_pool_stats("after alloc-then-free-all", pool_b.pool);
                }
            }
            if (want_pool_pmr)
            {
                PoolPmrBackend pool_pmr_b;
                auto [a, f] = timed_alloc_then_free_all(
                    pool_pmr_b, kSize, opt.alignment, bulk_count);
                print_bulk(PoolPmrBackend::name, a, f);
                if (opt.pool_stats)
                {
                    print_pool_stats(
                        "pool_pmr after alloc-then-free-all", pool_pmr_b.pool);
                }
            }
            if (want_pmr)
            {
                PmrUnsyncBackend b;
                auto [a, f] =
                    timed_alloc_then_free_all(b, kSize, opt.alignment, bulk_count);
                print_bulk(PmrUnsyncBackend::name, a, f);
            }
            if (want_slab)
            {
                Slab16Backend slab_b;
                auto [a, f] =
                    timed_alloc_then_free_all(slab_b, kSize, opt.alignment, bulk_count);
                print_bulk(Slab16Backend::name, a, f);
                if (opt.pool_stats)
                {
                    print_slab16_stats("after alloc-then-free-all", slab_b.slab);
                }
            }
            std::cout << "\n";
        }
    }

    // --- Multi-thread multi-size (shared backends that are thread-safe) ---
    if (has(opt.scenarios, Scenario::Threaded))
    {
        if (opt.threads <= 1)
        {
            std::cout << "Multi-thread multi-size skipped (pass --threads=N with N>1).\n\n";
        }
        else
        {
            if (has(be, BackendId::PmrUnsync))
            {
                note_skip("pmr_unsync not thread-safe (use pmr_sync for threaded)");
            }
            if (has(be, BackendId::Slab16))
            {
                note_skip("slab16 omitted from threaded multi-size (size<=16 only; use lifo/bulk)");
            }

            std::vector<Row> rows;
            PoolBackend pool_b;
            PoolPmrBackend pool_pmr_b;
            bool ran_pool = false;
            bool ran_pool_pmr = false;

            const int peak_live = opt.threads * opt.batch;
            std::cout << "Multi-thread multi-size (shared allocator; size-mix="
                      << size_mix_label(opt.size_mix) << "; batch=" << opt.batch
                      << "; peak live ≈ " << peak_live << ")\n";

            if (has(be, BackendId::New))
            {
                NewDeleteBackend new_b;
                rows.push_back(row_from(
                    new_b,
                    timed_threaded_multi_size_churn(
                        new_b, opt.alignment, opt.size_mix, opt.iters, opt.threads,
                        opt.batch),
                    true));
            }
            if (has(be, BackendId::Pool))
            {
                rows.push_back(row_from(
                    pool_b,
                    timed_threaded_multi_size_churn(
                        pool_b, opt.alignment, opt.size_mix, opt.iters, opt.threads,
                        opt.batch)));
                ran_pool = true;
            }
            if (has(be, BackendId::PoolPmr))
            {
                rows.push_back(row_from(
                    pool_pmr_b,
                    timed_threaded_multi_size_churn(
                        pool_pmr_b, opt.alignment, opt.size_mix, opt.iters,
                        opt.threads, opt.batch)));
                ran_pool_pmr = true;
            }
            if (has(be, BackendId::PmrSync))
            {
                PmrSyncBackend pmr_b;
                rows.push_back(row_from(
                    pmr_b,
                    timed_threaded_multi_size_churn(
                        pmr_b, opt.alignment, opt.size_mix, opt.iters, opt.threads,
                        opt.batch)));
            }
            print_table("Multi-thread timing", rows);
            if (opt.pool_stats)
            {
                if (ran_pool)
                {
                    print_pool_stats("after multi-thread multi-size", pool_b.pool);
                    std::cout << "    (peak live ≈ " << peak_live
                              << "; retained_mapped_bytes reflects multi-buffer growth)\n";
                }
                if (ran_pool_pmr)
                {
                    print_pool_stats(
                        "pool_pmr after multi-thread multi-size", pool_pmr_b.pool);
                }
            }
            std::cout << "\n";
        }
    }
}

void run_high_align_spot(Options const& opt)
{
    constexpr std::size_t kSize = 16;
    std::cout << "Align-" << opt.alignment
              << " spot-check (Pool alignment-class/header tax vs ::new; size=16)\n";
    std::cout << "Only ::new and Pool participate; other --backend values are skipped.\n\n";

    // High-align spots are LIFO-only tax checks (alignment class 32 or 64).
    if (!has(opt.scenarios, Scenario::Lifo))
    {
        std::cout << "Align-" << opt.alignment
                  << " spot-check skipped (--scenario without lifo).\n\n";
        return;
    }

    const BackendId be = opt.backends;
    std::vector<Row> rows;
    PoolBackend pool_b;
    bool ran_pool = false;

    if (has(be, BackendId::New))
    {
        NewDeleteBackend new_b;
        rows.push_back(row_from(
            new_b, timed_lifo_churn(new_b, kSize, opt.alignment, opt.iters), true));
    }
    if (has(be, BackendId::Pool))
    {
        rows.push_back(row_from(
            pool_b, timed_lifo_churn(pool_b, kSize, opt.alignment, opt.iters)));
        ran_pool = true;
    }
    if (has(be, BackendId::PmrUnsync) || has(be, BackendId::PmrSync) ||
        has(be, BackendId::Slab16))
    {
        note_skip("align=" + std::to_string(opt.alignment) +
                  " spot-check is Pool vs ::new only");
    }
    print_table("Warmed LIFO churn", rows);
    if (opt.pool_stats && ran_pool)
    {
        print_pool_stats(
            ("after align-" + std::to_string(opt.alignment) + " LIFO churn").c_str(),
            pool_b.pool);
    }
    std::cout << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    Options opt = parse_args(argc, argv);
    if (opt.help)
    {
        print_help();
        return (opt.alignment == 8 || opt.alignment == 32 || opt.alignment == 64) ? 0 : 2;
    }

    print_banner(opt);
    if (has(opt.backends, BackendId::New))
    {
        std::cout << "Ratios are ns/op divided by ::operator new on this run.\n";
    }
    else
    {
        std::cout << "Backend filter omits ::new; vs ::new column is n/a.\n";
    }
    std::cout << "Absolute ns are host/toolchain-specific; re-check after upgrades.\n\n";

    if (opt.alignment == 32 || opt.alignment == 64)
    {
        run_high_align_spot(opt);
    }
    else
    {
        run_default_align_suite(opt);
    }
    return 0;
}
