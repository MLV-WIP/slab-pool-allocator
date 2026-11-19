/*
 * On-demand allocator comparison harness (not part of make test).
 *
 * Prefer: make bench BUILD_TYPE=release
 *
 * Compares ::operator new, spallocator::Pool, std::pmr pool resources, and
 * (where applicable) bare Slab. Reports ns/op and ratio vs ::new.
 *
 * Usage:
 *   bench_allocators [--align=8|64] [--iters=N] [--threads=N] [--help]
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
#include "spallocator/slab.hpp"

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

inline std::size_t pick_user_size(std::minstd_rand& rng)
{
    std::uniform_int_distribution<std::size_t> dist(kUserSizeMin, kUserSizeMax);
    return dist(rng);
}

void print_pool_stats(std::string_view label, spallocator::Pool& pool)
{
    spallocator::PoolStats s = pool.stats();
    std::cout << "  [PoolStats] " << label << "\n"
              << "    live_allocations=" << s.live_allocations
              << " live_hwm=" << s.live_hwm << "\n"
              << "    retained_slab_bytes=" << s.retained_slab_bytes
              << " retained_hwm=" << s.retained_hwm << "\n"
              << "    alloc_ok=" << s.alloc_ok
              << " alloc_fail=" << s.alloc_fail
              << " free_ok=" << s.free_ok
              << " free_fail=" << s.free_fail << "\n";
}

void print_slab16_stats(std::string_view label, spallocator::Slab<16, 16>& slab)
{
    std::cout << "  [Slab<16,16>] " << label << "\n"
              << "    slab_count=" << slab.getSlabCount()
              << " retained_bytes=" << slab.getAllocatedMemory()
              << " availability_map_bytes=" << slab.getAvailabilityMapBytes()
              << "\n";
}

struct Options
{
    std::size_t alignment = 8;
    int iters = 300000;
    int threads = 1;
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
    if (opt.alignment != 8 && opt.alignment != 64)
    {
        std::cerr << "--align must be 8 (default suite) or 64 (Pool vs ::new spot-check)\n";
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
        "  --align=8|64     Payload alignment (default 8). 64 runs Pool vs ::new spot-check only.\n"
        "  --iters=N        Timed alloc/free pairs per scenario (default release 300000)\n"
        "  --threads=N      Worker threads for multi-thread multi-size churn (default 1)\n"
        "  --help           Show this help\n"
        "\n"
        "Multi-size scenarios draw user sizes uniformly from [16, 1024].\n"
        "Per-thread std::minstd_rand (seed key + worker index).\n"
        "Recommended: make bench BUILD_TYPE=release\n";
}

void print_banner(Options const& opt)
{
    std::cout << "spallocator allocator bench\n";
    std::cout << "  alignment: " << opt.alignment << "\n";
    std::cout << "  iters:     " << opt.iters << "\n";
    std::cout << "  threads:   " << opt.threads << "\n";
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
void warm_random_sizes(Backend& b, std::size_t align, std::minstd_rand& rng, int warm_count)
{
    std::vector<std::pair<void*, std::size_t>> live;
    live.reserve(static_cast<std::size_t>(warm_count));
    for (int i = 0; i < warm_count; ++i)
    {
        std::size_t s = pick_user_size(rng);
        live.emplace_back(b.allocate(s, align), s);
    }
    for (auto& [p, s] : live)
    {
        b.deallocate(p, s, align);
    }
}

template <class Backend>
Timing timed_multi_size_churn(Backend& b, std::size_t align, int iters,
                              unsigned worker_index = 0)
{
    std::minstd_rand rng = make_bench_rng(worker_index);
    warm_random_sizes(b, align, rng, 256);
    const auto t0 = clock_type::now();
    for (int i = 0; i < iters; ++i)
    {
        std::size_t s = pick_user_size(rng);
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
Timing timed_threaded_multi_size_churn(Backend& shared, std::size_t align,
                                       int total_iters, int threads)
{
    {
        std::minstd_rand warm_rng = make_bench_rng(0);
        warm_random_sizes(shared, align, warm_rng, 256);
    }
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    // Each worker holds its own batch of live pointers (not shared across threads).
    constexpr std::size_t kBatch = 16;
    const int per = total_iters / threads / static_cast<int>(kBatch);

    const auto t0 = clock_type::now();
    for (int t = 0; t < threads; ++t)
    {
        const unsigned worker_index = static_cast<unsigned>(t);
        workers.emplace_back([&shared, &ready, align, per, threads, worker_index] {
            // Per-thread RNG: same seed key every process run; distinct stream
            // per worker_index (not std::thread::id - that is not stable).
            std::minstd_rand rng = make_bench_rng(worker_index);
            std::vector<std::pair<void*, std::size_t>> batch(kBatch);

            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < threads)
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < per; ++i)
            {
                for (std::size_t j = 0; j < kBatch; ++j)
                {
                    std::size_t s = pick_user_size(rng);
                    batch[j].first = shared.allocate(s, align);
                    batch[j].second = s;
                    sink_ptr(batch[j].first);
                }
                // Reverse free; use signed index so j >= 0 is well-defined
                // (size_t would wrap forever after j hits 0).
                for (int j = static_cast<int>(kBatch) - 1; j >= 0; --j)
                {
                    auto& slot = batch[static_cast<std::size_t>(j)];
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
    return make_timing(t1 - t0, per * threads * static_cast<int>(kBatch));
}

struct Row
{
    std::string backend;
    Timing timing;
};

void print_table(std::string_view title, std::vector<Row> const& rows)
{
    std::cout << title << "\n";
    if (rows.empty())
    {
        std::cout << "  (no rows)\n\n";
        return;
    }
    const double baseline = rows.front().timing.ns_per_op;
    std::cout << std::left << std::setw(28) << "  backend"
              << std::right << std::setw(14) << "ns/op"
              << std::setw(12) << "vs ::new"
              << "\n";
    for (auto const& r : rows)
    {
        const double ratio = (baseline > 0.0) ? (r.timing.ns_per_op / baseline) : 0.0;
        std::cout << std::left << std::setw(28) << ("  " + r.backend)
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(14) << r.timing.ns_per_op
                  << std::setprecision(3) << std::setw(12) << ratio
                  << "\n";
    }
    std::cout << "\n";
}

template <class Backend>
Row row_from(Backend& /*b*/, Timing t)
{
    return Row{Backend::name, t};
}

void run_align8_suite(Options const& opt)
{
    constexpr std::size_t kSize = 16;
    const int bulk_count = std::min(opt.iters, 100000);

    // --- Warmed LIFO churn (16B) ---
    {
        std::vector<Row> rows;
        NewDeleteBackend new_b;
        PoolBackend pool_b;
        PmrUnsyncBackend pmr_b;
        Slab16Backend slab_b;

        rows.push_back(row_from(new_b, timed_lifo_churn(new_b, kSize, opt.alignment, opt.iters)));
        rows.push_back(row_from(pool_b, timed_lifo_churn(pool_b, kSize, opt.alignment, opt.iters)));
        rows.push_back(row_from(pmr_b, timed_lifo_churn(pmr_b, kSize, opt.alignment, opt.iters)));
        rows.push_back(row_from(slab_b, timed_lifo_churn(slab_b, kSize, opt.alignment, opt.iters)));
        print_table("Warmed LIFO churn (size=16, alloc+free pair)", rows);
        print_pool_stats("after LIFO churn", pool_b.pool);
        print_slab16_stats("after LIFO churn", slab_b.slab);
        std::cout << "\n";
    }

    // --- Multi-size mix (uniform random user sizes in [16, 1024]) ---
    {
        std::vector<Row> rows;
        NewDeleteBackend new_b;
        PoolBackend pool_b;
        PmrUnsyncBackend pmr_b;

        rows.push_back(row_from(new_b, timed_multi_size_churn(new_b, opt.alignment, opt.iters)));
        rows.push_back(row_from(pool_b, timed_multi_size_churn(pool_b, opt.alignment, opt.iters)));
        rows.push_back(row_from(pmr_b, timed_multi_size_churn(pmr_b, opt.alignment, opt.iters)));
        print_table(
            "Multi-size mix (uniform random user size in [16, 1024], alloc+free pair)",
            rows);
        print_pool_stats("after multi-size mix", pool_b.pool);
        std::cout << "\n";
    }

    // --- Alloc-all then free-all ---
    {
        std::cout << "Alloc-then-free-all (size=16, count=" << bulk_count << ")\n";
        std::cout << std::left << std::setw(28) << "  backend"
                  << std::right << std::setw(14) << "alloc ns/op"
                  << std::setw(14) << "free ns/op"
                  << std::setw(12) << "alloc vs"
                  << std::setw(12) << "free vs"
                  << "\n";

        NewDeleteBackend baseline_b;
        auto [base_a, base_f] =
            timed_alloc_then_free_all(baseline_b, kSize, opt.alignment, bulk_count);

        auto print_bulk = [&](char const* name, Timing a, Timing f) {
            std::cout << std::left << std::setw(28) << (std::string("  ") + name)
                      << std::right << std::fixed << std::setprecision(1)
                      << std::setw(14) << a.ns_per_op
                      << std::setw(14) << f.ns_per_op
                      << std::setprecision(3)
                      << std::setw(12) << (a.ns_per_op / base_a.ns_per_op)
                      << std::setw(12) << (f.ns_per_op / base_f.ns_per_op)
                      << "\n";
        };

        print_bulk(NewDeleteBackend::name, base_a, base_f);
        PoolBackend pool_b;
        {
            auto [a, f] = timed_alloc_then_free_all(pool_b, kSize, opt.alignment, bulk_count);
            print_bulk(PoolBackend::name, a, f);
        }
        {
            PmrUnsyncBackend b;
            auto [a, f] = timed_alloc_then_free_all(b, kSize, opt.alignment, bulk_count);
            print_bulk(PmrUnsyncBackend::name, a, f);
        }
        print_pool_stats("after alloc-then-free-all", pool_b.pool);
        std::cout << "\n";
    }

    // --- Multi-thread multi-size (shared backends that are thread-safe) ---
    if (opt.threads > 1)
    {
        std::vector<Row> rows;
        NewDeleteBackend new_b;
        PoolBackend pool_b;
        PmrSyncBackend pmr_b;

        rows.push_back(row_from(
            new_b, timed_threaded_multi_size_churn(
                       new_b, opt.alignment, opt.iters, opt.threads)));
        rows.push_back(row_from(
            pool_b, timed_threaded_multi_size_churn(
                        pool_b, opt.alignment, opt.iters, opt.threads)));
        rows.push_back(row_from(
            pmr_b, timed_threaded_multi_size_churn(
                       pmr_b, opt.alignment, opt.iters, opt.threads)));
        print_table(
            "Multi-thread multi-size (uniform random [16, 1024], shared allocator)",
            rows);
        print_pool_stats("after multi-thread multi-size", pool_b.pool);
        std::cout << "\n";
    }
    else
    {
        std::cout << "Multi-thread multi-size skipped (pass --threads=N with N>1).\n\n";
    }
}

void run_align64_spot(Options const& opt)
{
    constexpr std::size_t kSize = 16;
    std::cout << "Align-64 spot-check (Pool axis/header tax vs ::new; size=16)\n";
    std::cout << "Other backends omitted on purpose.\n\n";

    std::vector<Row> rows;
    NewDeleteBackend new_b;
    PoolBackend pool_b;
    rows.push_back(row_from(new_b, timed_lifo_churn(new_b, kSize, opt.alignment, opt.iters)));
    rows.push_back(row_from(pool_b, timed_lifo_churn(pool_b, kSize, opt.alignment, opt.iters)));
    print_table("Warmed LIFO churn", rows);
    print_pool_stats("after align-64 LIFO churn", pool_b.pool);
    std::cout << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    Options opt = parse_args(argc, argv);
    if (opt.help)
    {
        print_help();
        return opt.alignment == 8 || opt.alignment == 64 ? 0 : 2;
    }

    print_banner(opt);
    std::cout << "Ratios are ns/op divided by ::operator new on this run.\n"
                 "Absolute ns are host/toolchain-specific; re-check after upgrades.\n\n";

    if (opt.alignment == 64)
    {
        run_align64_spot(opt);
    }
    else
    {
        run_align8_suite(opt);
    }
    return 0;
}
