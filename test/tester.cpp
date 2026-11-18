/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2025-2026, Michael VanLoon
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include "spallocator/helper.hpp"
#include "spallocator/slab.hpp"
#include "spallocator/pool.hpp"
#include "spallocator/spallocator.hpp"

using namespace std::literals;
using namespace spallocator;


TEST(BitsetSearchTest, FindNextClearBit)
{
    constexpr std::size_t N = 256;
    std::bitset<N> bits;

    EXPECT_EQ(find_next_clear_bit(bits, 0), 0u);
    EXPECT_EQ(find_next_clear_bit(bits, 63), 63u);
    EXPECT_EQ(find_next_clear_bit(bits, 64), 64u);
    EXPECT_EQ(find_next_clear_bit(bits, N), N);

    bits.set();
    EXPECT_EQ(find_next_clear_bit(bits, 0), N);

    bits.reset(0);
    bits.reset(63);
    bits.reset(64);
    bits.reset(200);
    EXPECT_EQ(find_next_clear_bit(bits, 0), 0u);
    EXPECT_EQ(find_next_clear_bit(bits, 1), 63u);
    EXPECT_EQ(find_next_clear_bit(bits, 63), 63u);
    EXPECT_EQ(find_next_clear_bit(bits, 64), 64u);
    EXPECT_EQ(find_next_clear_bit(bits, 65), 200u);
    EXPECT_EQ(find_next_clear_bit(bits, 201), N);
}


TEST(SlabTest, NextFitPrefersCursorOverEarlierFree)
{
    // Why: next-fit must not regress to first-fit. After advancing the cursor,
    // a free slot *behind* it must not win the next allocate — otherwise LIFO
    // churn re-scans the front of the freemap every time.
    //
    // Slot indices are derived from selectBufferSize<>() so the scenario stays
    // valid across kSlabTargetBytes / multiplier configure knobs (old tests
    // hard-coded 200/50 for a 256-slot 4 KiB buffer).
    Slab<16, 16> slab;
    constexpr std::size_t kSlots = selectBufferSize<16>() / 16;
    static_assert(kSlots >= 16);
    constexpr std::size_t kCursor = kSlots / 2;
    constexpr std::size_t kEarlier = kCursor / 4;
    std::vector<std::byte*> items;
    items.reserve(kSlots);

    for (std::size_t i = 0; i < kCursor; ++i)
    {
        items.push_back(slab.allocateItem(16));
    }
    std::byte* const base = items.front();
    // Hole behind the cursor (would win under first-fit).
    slab.deallocateItem(items[kEarlier]);
    items[kEarlier] = nullptr;

    std::byte* next = slab.allocateItem(16);
    EXPECT_EQ(next, base + kCursor * 16); // continues forward, ignores hole
    items.push_back(next);
    EXPECT_EQ(slab.getSlabCount(), 1u);

    for (auto* p : items)
    {
        if (p)
        {
            slab.deallocateItem(p);
        }
    }
}


TEST(SlabTest, NextFitWrapsWithinBuffer)
{
    // Why: when the only free slot is below the cursor, next-fit must wrap
    // *once within the same buffer* before growing. Growing here would waste
    // RSS and break the “reuse holes in the current slab” contract.
    //
    // Fill the whole buffer, park the cursor near the end, free a low slot,
    // then prove the next alloc returns that low slot without adding a buffer.
    Slab<16, 16> slab;
    constexpr std::size_t kSlots = selectBufferSize<16>() / 16;
    static_assert(kSlots >= 16);
    constexpr std::size_t kHigh = kSlots - 6;
    constexpr std::size_t kLow = 10;
    std::vector<std::byte*> items;
    items.reserve(kSlots);
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        items.push_back(slab.allocateItem(16));
    }
    EXPECT_EQ(slab.getSlabCount(), 1u);
    std::byte* const base = items.front();

    // Re-take kHigh so the cursor sits at kHigh + 1 (past the only soon-free hole).
    slab.deallocateItem(items[kHigh]);
    items[kHigh] = slab.allocateItem(16);
    EXPECT_EQ(items[kHigh], base + kHigh * 16);

    slab.deallocateItem(items[kLow]);
    items[kLow] = nullptr;

    std::byte* wrapped = slab.allocateItem(16);
    EXPECT_EQ(wrapped, base + kLow * 16);
    EXPECT_EQ(slab.getSlabCount(), 1u); // wrap, do not grow
    items[kLow] = wrapped;

    for (auto* p : items)
    {
        slab.deallocateItem(p);
    }
}


TEST(SlabTest, NextFitSurvivesReclaim)
{
    // Why: reclaimEmptySlabs swap-with-last invalidates buffer indices, so the
    // next-fit cursor must be reset. After growing to two buffers, freeing all,
    // and reclaiming one, alloc/free on the survivor must still work.
    Slab<16, 16> slab;
    constexpr std::size_t kSlots = selectBufferSize<16>() / 16;
    std::vector<std::byte*> items;
    items.reserve(kSlots + 1);
    for (std::size_t i = 0; i < kSlots + 1; ++i)
    {
        items.push_back(slab.allocateItem(16));
    }
    EXPECT_EQ(slab.getSlabCount(), 2u);

    for (auto* p : items)
    {
        slab.deallocateItem(p);
    }
    EXPECT_EQ(slab.reclaimEmptySlabs(), 1u);
    EXPECT_EQ(slab.getSlabCount(), 1u);

    auto* p = slab.allocateItem(16);
    ASSERT_NE(p, nullptr);
    slab.deallocateItem(p);
}


// Microbench evidence for P16 (next-fit + CTZ). Not a pass/fail gate on time —
// records ns/op via RecordProperty; prefer BUILD_TYPE=release for numbers.
TEST(SlabTest, AllocFreeChurnMicrobench)
{
    // Warm a full buffer then LIFO churn; slot count follows selectBufferSize
    // so the warm-up still saturates exactly one buffer under any packing knobs.
    Slab<16, 16> slab;
    constexpr std::size_t kSlots = selectBufferSize<16>() / 16;
#if defined(NDEBUG)
    constexpr int kIters = 500000;
#else
    constexpr int kIters = 20000; // keep sanitizer/debug suites snappy
#endif

    // Warm: fill then free so the freemap is dense and the cursor is live.
    std::vector<std::byte*> warm;
    warm.reserve(kSlots);
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        warm.push_back(slab.allocateItem(16));
    }
    for (auto* p : warm)
    {
        slab.deallocateItem(p);
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i)
    {
        std::byte* p = slab.allocateItem(16);
        slab.deallocateItem(p);
    }
    const auto t1 = std::chrono::steady_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const double ns_per_op = static_cast<double>(ns) / static_cast<double>(kIters);
    RecordProperty("alloc_free_pairs", kIters);
    // RecordProperty is int-sized; store ms total to avoid overflow.
    RecordProperty("total_ms", static_cast<int>(ns / 1'000'000));
    RecordProperty("ns_per_alloc_free", static_cast<int>(ns_per_op));
    // Sanity: churn must complete and stay on the warmed single buffer.
    EXPECT_EQ(slab.getSlabCount(), 1u);
    EXPECT_GT(ns, 0);
}


TEST(SlabTest, ReclaimEmptySlabsReturnsBuffers)
{
    // Why: empty buffers are retained by default (monotonic growth); reclaim
    // must drop extras while keeping ≥ keep_min. Capacity is selectBufferSize
    // based so “fill one buffer + 1” still forces a second buffer after packing.
    Slab<16, 16> slab;
    constexpr std::size_t kSlotsPerSlab = selectBufferSize<16>() / 16;
    EXPECT_EQ(slab.getSlabCount(), 1u);
    const std::size_t baseline = slab.getAllocatedMemory();

    std::vector<std::byte*> items;
    items.reserve(kSlotsPerSlab + 1);
    for (std::size_t i = 0; i < kSlotsPerSlab + 1; ++i)
    {
        items.push_back(slab.allocateItem(16));
    }
    EXPECT_EQ(slab.getSlabCount(), 2u);
    EXPECT_EQ(slab.getAllocatedMemory(), baseline * 2);

    for (auto* p : items)
    {
        slab.deallocateItem(p);
    }
    // Monotonic by default: empty slabs stay until reclaim.
    EXPECT_EQ(slab.getSlabCount(), 2u);
    EXPECT_EQ(slab.reclaimEmptySlabs(), 1u);
    EXPECT_EQ(slab.getSlabCount(), 1u);
    EXPECT_EQ(slab.getAllocatedMemory(), baseline);
    EXPECT_EQ(slab.reclaimEmptySlabs(), 0u);

    // Live allocations block reclaim of their slab.
    auto* live = slab.allocateItem(16);
    items.clear();
    for (std::size_t i = 0; i < kSlotsPerSlab; ++i)
    {
        items.push_back(slab.allocateItem(16));
    }
    EXPECT_EQ(slab.getSlabCount(), 2u);
    for (auto* p : items)
    {
        slab.deallocateItem(p);
    }
    EXPECT_EQ(slab.reclaimEmptySlabs(), 1u);
    EXPECT_EQ(slab.getSlabCount(), 1u);
    slab.deallocateItem(live);
}


// Control block + live_allocs + handle() (stack pool; no heap auto-retire).
TEST(PoolTest, ControlBlockTracksLiveAllocsAndHandle)
{
    Pool pool;
    EXPECT_EQ(pool.live_allocation_count(), 0u);
    ASSERT_NE(pool.control(), nullptr);
    EXPECT_TRUE(pool.control()->is_alive());
    EXPECT_EQ(pool.control()->handle_ref_count(), 0u);
    // Pool holds the initial block_ref.
    EXPECT_EQ(pool.control()->block_ref_count(), 1u);

    auto* a = pool.allocate(32);
    auto* b = pool.allocate(32);
    EXPECT_EQ(pool.live_allocation_count(), 2u);

    pool.deallocate(a);
    EXPECT_EQ(pool.live_allocation_count(), 1u);

    {
        PoolHandle h = pool.handle();
        EXPECT_TRUE(static_cast<bool>(h));
        EXPECT_EQ(h.get(), &pool);
        EXPECT_EQ(pool.control()->handle_ref_count(), 1u);
        EXPECT_EQ(pool.control()->block_ref_count(), 2u);

        PoolHandle h2 = h;
        EXPECT_EQ(pool.control()->handle_ref_count(), 2u);
        EXPECT_EQ(pool.control()->block_ref_count(), 3u);
    }
    EXPECT_EQ(pool.control()->handle_ref_count(), 0u);
    EXPECT_EQ(pool.control()->block_ref_count(), 1u);

    pool.deallocate(b);
    EXPECT_EQ(pool.live_allocation_count(), 0u);
}


TEST(PoolTest, ControlBlockSurvivesPoolWhenHandleRemains)
{
    PoolControlBlock* block = nullptr;
    {
        PoolHandle h;
        {
            Pool pool;
            block = pool.control();
            h = pool.handle();
            EXPECT_TRUE(block->is_alive());
            EXPECT_EQ(h.get(), &pool);
        } // ~Pool: mark_dead, drop Pool's block_ref; handle keeps block alive
        EXPECT_FALSE(block->is_alive());
        EXPECT_EQ(h.get(), nullptr);
        EXPECT_GE(block->block_ref_count(), 1u);
    } // ~PoolHandle drops last refs → ControlBlock deleted
}


// Heap PoolHandle::create + retire when handles and live allocs are both zero.
TEST(PoolTest, PoolHandleCreateRetiresWhenEmptyHandleDropped)
{
    PoolControlBlock* block = nullptr;
    {
        PoolHandle h = PoolHandle::create();
        ASSERT_TRUE(static_cast<bool>(h));
        block = h.control();
        ASSERT_NE(block, nullptr);
        EXPECT_TRUE(block->is_heap_owned());
        EXPECT_TRUE(block->is_alive());
        EXPECT_EQ(block->live_allocation_count(), 0u);
        EXPECT_EQ(block->handle_ref_count(), 1u);
        EXPECT_NE(h.get(), nullptr);
    } // last handle drop → retire heap Pool → control block gone with last block_ref
    // Cannot touch `block` here — it was deleted.
}


TEST(PoolTest, PoolHandleRetiresOnLastFreeAfterHandlesDropped)
{
    PoolHandle h = PoolHandle::create();
    Pool* pool = h.get();
    ASSERT_NE(pool, nullptr);
    PoolControlBlock* block = h.control();

    auto* p = pool->allocate(64);
    EXPECT_EQ(block->live_allocation_count(), 1u);

    // Drop all handles while an allocation remains — pool must stay alive.
    h = PoolHandle{};
    EXPECT_EQ(block->handle_ref_count(), 0u);
    EXPECT_TRUE(block->is_alive());
    EXPECT_EQ(block->pool(), pool);

    // Last free retires the heap pool (and may delete `this` inside try_deallocate).
    EXPECT_TRUE(pool->try_deallocate(p));
    // Pool and its control block are gone; do not touch pool/block.
}


TEST(PoolTest, PoolHandleKeepsEmptyPoolAliveUntilLastHandleDropped)
{
    PoolHandle h1 = PoolHandle::create();
    PoolControlBlock* block = h1.control();
    PoolHandle h2 = h1;
    EXPECT_EQ(block->handle_ref_count(), 2u);

    auto* p = h1.get()->allocate(16);
    h1.get()->deallocate(p);
    EXPECT_EQ(block->live_allocation_count(), 0u);
    EXPECT_TRUE(block->is_alive()); // handles still pin empty heap pool

    h1 = PoolHandle{};
    EXPECT_EQ(block->handle_ref_count(), 1u);
    EXPECT_TRUE(block->is_alive());

    h2 = PoolHandle{};
    // retired — block deleted with last handle
}


TEST(PoolTest, StackPoolHandleDoesNotHeapRetireOnLastFree)
{
    Pool pool;
    auto* p = pool.allocate(32);
    PoolHandle h = pool.handle();
    h = PoolHandle{}; // handle_refs → 0; stack pool must not be delete'd
    pool.deallocate(p);
    EXPECT_EQ(pool.live_allocation_count(), 0u);
    EXPECT_TRUE(pool.control()->is_alive());
    EXPECT_FALSE(pool.control()->is_heap_owned());
}


namespace
{
struct alignas(8) ArenaDtorProbe
{
    static int destructions;
    ~ArenaDtorProbe() { ++destructions; }
};
int ArenaDtorProbe::destructions = 0;
} // namespace


// Arena teardown: PoolDeleter sees !alive and skips ~T / free.
TEST(PoolTest, ArenaDestroySkipsTypedDestructorInDeleter)
{
    ArenaDtorProbe::destructions = 0;
    std::optional<unique_pool_ptr<ArenaDtorProbe>> ptr;
    {
        Pool pool;
        ptr.emplace(make_pool_unique<ArenaDtorProbe>(pool));
        ASSERT_NE(ptr->get(), nullptr);
        EXPECT_EQ(pool.live_allocation_count(), 1u);
    } // ~Pool: arena mark_dead + free slabs; ~T not run by pool
    EXPECT_EQ(ArenaDtorProbe::destructions, 0);
    ptr.reset(); // deleter sees !alive → no-op (still no ~T)
    EXPECT_EQ(ArenaDtorProbe::destructions, 0);
}


TEST(PoolTest, OrderlyUniquePtrRunsDestructor)
{
    ArenaDtorProbe::destructions = 0;
    Pool pool;
    {
        auto ptr = make_pool_unique<ArenaDtorProbe>(pool);
        ASSERT_NE(ptr.get(), nullptr);
    }
    EXPECT_EQ(ArenaDtorProbe::destructions, 1);
    EXPECT_EQ(pool.live_allocation_count(), 0u);
}


TEST(PoolTest, MakePoolUniqueFromHandle)
{
    auto h = PoolHandle::create();
    auto ptr = make_pool_unique<int>(h, 42);
    ASSERT_NE(ptr.get(), nullptr);
    EXPECT_EQ(*ptr, 42);
    ptr.reset();
    EXPECT_EQ(h.get()->live_allocation_count(), 0u);
}


TEST(PoolTest, StatsTracksAllocFreeAndLiveHwm)
{
    // Why: ops need live vs retained pressure and peak live (HWM) before hard
    // caps trip. HWM must not fall when frees drop the current live count.
    Pool pool;
    PoolStats empty = pool.stats();
    EXPECT_EQ(empty.live_allocations, 0u);
    EXPECT_EQ(empty.alloc_ok, 0u);
    EXPECT_EQ(empty.alloc_fail, 0u);
    EXPECT_EQ(empty.free_ok, 0u);
    EXPECT_EQ(empty.free_fail, 0u);
    EXPECT_EQ(empty.live_hwm, 0u);
    EXPECT_GE(empty.retained_slab_bytes, 0u);
    EXPECT_GE(empty.retained_hwm, empty.retained_slab_bytes);

    auto* a = pool.allocate(16);
    auto* b = pool.allocate(16);
    PoolStats mid = pool.stats();
    EXPECT_EQ(mid.live_allocations, 2u);
    EXPECT_EQ(mid.alloc_ok, 2u);
    EXPECT_EQ(mid.live_hwm, 2u);
    EXPECT_GE(mid.retained_hwm, mid.retained_slab_bytes);

    pool.deallocate(a);
    PoolStats after_one = pool.stats();
    EXPECT_EQ(after_one.live_allocations, 1u);
    EXPECT_EQ(after_one.free_ok, 1u);
    EXPECT_EQ(after_one.live_hwm, 2u); // peak retained after free

    pool.deallocate(b);
    PoolStats done = pool.stats();
    EXPECT_EQ(done.live_allocations, 0u);
    EXPECT_EQ(done.alloc_ok, 2u);
    EXPECT_EQ(done.free_ok, 2u);
    EXPECT_EQ(done.live_hwm, 2u);
}


TEST(PoolTest, StatsCountsAllocAndFreeFailures)
{
    // Why: alloc_fail / free_fail are the alert signals for bad clients and
    // approaching/hitting policy errors (align, size cap) without scraping logs.
    Pool pool;
    EXPECT_THROW(pool.allocate(16, 12), std::invalid_argument); // not power of two
    EXPECT_THROW(pool.allocate(kMaxAllocationBytes + 1), std::out_of_range);

    std::byte junk{};
    EXPECT_FALSE(pool.try_deallocate(&junk));

    PoolStats s = pool.stats();
    EXPECT_EQ(s.alloc_ok, 0u);
    EXPECT_GE(s.alloc_fail, 2u);
    EXPECT_EQ(s.free_ok, 0u);
    EXPECT_GE(s.free_fail, 1u);
}


// Arena + make_pool_shared: control block is heap-allocated; PoolDeleter token
// outlives slabs. Same no-op model as unique_pool_ptr when !alive.
TEST(PoolTest, ArenaDestroySharedPtrSkipsTypedDestructor)
{
    ArenaDtorProbe::destructions = 0;
    std::shared_ptr<ArenaDtorProbe> ptr;
    {
        Pool pool;
        ptr = make_pool_shared<ArenaDtorProbe>(pool);
        ASSERT_NE(ptr.get(), nullptr);
        EXPECT_EQ(pool.live_allocation_count(), 1u);
    } // ~Pool: mark_dead + free slabs; ~T not run by pool
    EXPECT_EQ(ArenaDtorProbe::destructions, 0);
    ptr.reset(); // deleter sees !alive → no-op (still no ~T)
    EXPECT_EQ(ArenaDtorProbe::destructions, 0);
}


TEST(PoolTest, ArenaDestroySharedPtrDeallocateNoops)
{
    std::shared_ptr<int> ptr;
    {
        Pool pool;
        ptr = make_pool_shared<int>(pool, 7);
        ASSERT_NE(ptr.get(), nullptr);
        EXPECT_EQ(*ptr, 7);
        EXPECT_EQ(pool.live_allocation_count(), 1u);
    }
    ptr.reset(); // ~int trivial; PoolDeleter sees !alive → no-op
}


TEST(PoolTest, OrderlySharedPtrRunsDestructor)
{
    ArenaDtorProbe::destructions = 0;
    Pool pool;
    {
        auto ptr = make_pool_shared<ArenaDtorProbe>(pool);
        ASSERT_NE(ptr.get(), nullptr);
    }
    EXPECT_EQ(ArenaDtorProbe::destructions, 1);
    EXPECT_EQ(pool.live_allocation_count(), 0u);
}


TEST(PoolTest, MakePoolSharedFromHandle)
{
    auto h = PoolHandle::create();
    auto ptr = make_pool_shared<int>(h, 42);
    ASSERT_NE(ptr.get(), nullptr);
    EXPECT_EQ(*ptr, 42);
    ptr.reset();
    EXPECT_EQ(h.get()->live_allocation_count(), 0u);
}


TEST(PoolTest, TrimReclaimsEmptySizeClassSlabs)
{
    // Why: Pool::trim is the ops path to return peak RSS after load drops.
    // Grow the 32 B size class past one buffer, free everything, then trim
    // back to the eager baseline. Slot count uses selectBufferSize<32>().
    Pool pool;
    const std::size_t before = pool.getAllocatedMemory();

    // allocate(16) → footprint 24 → 32 B class on axis 16.
    constexpr std::size_t kSlotsPerSlab = selectBufferSize<32>() / 32;
    std::vector<std::byte*> items;
    items.reserve(kSlotsPerSlab + 1);
    for (std::size_t i = 0; i < kSlotsPerSlab + 1; ++i)
    {
        items.push_back(pool.allocate(16));
    }
    EXPECT_GT(pool.getAllocatedMemory(), before);

    for (auto* p : items)
    {
        pool.deallocate(p);
    }
    EXPECT_GT(pool.getAllocatedMemory(), before);
    EXPECT_GE(pool.trim(), 1u);
    EXPECT_EQ(pool.getAllocatedMemory(), before);
    EXPECT_EQ(pool.trim(), 0u);
}


TEST(SlabTest, AvailabilityMapScalesWithLiveSlabs)
{
    // Why: DynBitset must stay tiny vs the old fixed bitset<max_slabs>
    // (~128 KiB when buffers were 4 KiB). Filling one packed buffer + 1 forces
    // a second live slab so we also check the map grows with live count only.
    constexpr std::size_t kLegacyFixedBytes = 128_KB;

    Slab<16, 16> slab;
    EXPECT_EQ(slab.getSlabCount(), 1u);
    EXPECT_GT(slab.getAvailabilityMapBytes(), 0u);
    EXPECT_LT(slab.getAvailabilityMapBytes(), kLegacyFixedBytes);
    EXPECT_LE(slab.getAvailabilityMapBytes(), 64u);

    constexpr std::size_t kSlotsPerSlab = selectBufferSize<16>() / 16;
    std::vector<std::byte*> items;
    items.reserve(kSlotsPerSlab + 1);
    for (std::size_t i = 0; i < kSlotsPerSlab; ++i)
    {
        items.push_back(slab.allocateItem(16));
        ASSERT_NE(items.back(), nullptr);
    }
    EXPECT_EQ(slab.getSlabCount(), 1u);
    items.push_back(slab.allocateItem(16));
    EXPECT_EQ(slab.getSlabCount(), 2u);
    EXPECT_LE(slab.getAvailabilityMapBytes(), 64u);

    for (auto* p : items)
    {
        slab.deallocateItem(p);
    }
}


TEST(SlabTest, SelectBufferSizeExactPackAndFloors)
{
    // Why: buffer packing must leave zero unused tail bytes (old fixed 4 KiB
    // buffers wasted up to 6.25% on 384/768) and honor both knobs:
    //   slots = max(kSlabTargetMultiplier, kSlabTargetBytes / ElemSize)
    //   buffer = slots * ElemSize
    // Classes below were the worst historical tails under 4 KiB buffers.
    EXPECT_EQ(selectBufferSize<48>() % 48u, 0u);
    EXPECT_EQ(selectBufferSize<96>() % 96u, 0u);
    EXPECT_EQ(selectBufferSize<192>() % 192u, 0u);
    EXPECT_EQ(selectBufferSize<384>() % 384u, 0u);
    EXPECT_EQ(selectBufferSize<768>() % 768u, 0u);

    // Soft target wins when target/ElemSize > multiplier (may be slightly
    // under kSlabTargetBytes for odd ElemSize — that is intentional exact pack).
    EXPECT_EQ(selectBufferSize<16>(),
              (kSlabTargetBytes / 16) * 16);
    EXPECT_EQ(selectBufferSize<48>(),
              (kSlabTargetBytes / 48) * 48);
    EXPECT_EQ(selectBufferSize<384>(),
              (kSlabTargetBytes / 384) * 384);

    // Hard floor: large classes must not starve at 4 slots (old ElemSize*4);
    // they get exactly kSlabTargetMultiplier slots.
    EXPECT_EQ(selectBufferSize<1_KB>(), kSlabTargetMultiplier * 1_KB);
    EXPECT_EQ(selectBufferSize<4096>(), kSlabTargetMultiplier * 4096u);

    // Slab<> must publish the same size the freemap/bitset was compiled with.
    Slab<48, 16> s48;
    Slab<384, 16> s384;
    EXPECT_EQ(s48.getAllocSize(), selectBufferSize<48>());
    EXPECT_EQ(s384.getAllocSize(), selectBufferSize<384>());
    EXPECT_EQ(s48.getAllocSize() / 48u, kSlabTargetBytes / 48u);
    EXPECT_GE(s384.getAllocSize() / 384u, kSlabTargetMultiplier);
}


TEST(SlabTest, CreateSlabs)
{
    // Smoke: getAllocSize/getAllocatedMemory match selectBufferSize for a
    // cross-section of classes, including multiplier-floored large sizes and
    // an odd ElemSize that is not a KiB multiple (KB-multiple assert removed).
    {
        Slab<64> slab;
        EXPECT_EQ(slab.getElemSize(), 64u);
        EXPECT_EQ(slab.getAllocSize(), selectBufferSize<64>());
        EXPECT_EQ(slab.getAllocatedMemory(), selectBufferSize<64>());
        EXPECT_EQ(slab.getAllocSize() % 64u, 0u);
    }

    {
        Slab<128> slab;
        EXPECT_EQ(slab.getElemSize(), 128u);
        EXPECT_EQ(slab.getAllocSize(), selectBufferSize<128>());
        EXPECT_EQ(slab.getAllocatedMemory(), selectBufferSize<128>());
    }

    {
        Slab<1_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 1_KB);
        EXPECT_EQ(slab.getAllocSize(), selectBufferSize<1_KB>());
        EXPECT_EQ(slab.getAllocatedMemory(), selectBufferSize<1_KB>());
        EXPECT_EQ(slab.getAllocSize(), kSlabTargetMultiplier * 1_KB);
    }

    {
        Slab<2_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 2_KB);
        EXPECT_EQ(slab.getAllocSize(), selectBufferSize<2_KB>());
        EXPECT_EQ(slab.getAllocatedMemory(), selectBufferSize<2_KB>());
        EXPECT_EQ(slab.getAllocSize(), kSlabTargetMultiplier * 2_KB);
    }

    {
        Slab<16_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 16_KB);
        EXPECT_EQ(slab.getAllocSize(), selectBufferSize<16_KB>());
        EXPECT_EQ(slab.getAllocatedMemory(), selectBufferSize<16_KB>());
        EXPECT_EQ(slab.getAllocSize(), kSlabTargetMultiplier * 16_KB);
    }

    {
        Slab<12336> slab;
        EXPECT_EQ(slab.getElemSize(), 12336u);
        EXPECT_EQ(slab.getAllocSize(), selectBufferSize<12336>());
        EXPECT_EQ(slab.getAllocatedMemory(), selectBufferSize<12336>());
        EXPECT_EQ(slab.getAllocSize() % 12336u, 0u);
    }

}


TEST(SlabTest, AllocateItems)
{
    // Why: growth must happen only after a buffer is full, and retained empty
    // buffers stay mapped until trim/reclaim (monotonic). Drive fills from
    // selectBufferSize so expectations track packing knobs.
    Slab<128> slab;

    constexpr std::size_t kBuf = selectBufferSize<128>();
    constexpr std::size_t kSlots = kBuf / 128;
    EXPECT_EQ(slab.getAllocatedMemory(), kBuf);
    EXPECT_EQ(slab.getSlabCount(), 1u);

    std::vector<std::byte*> items;
    items.reserve(2 * kSlots + 1);
    for (std::size_t i = 0; i < kSlots; ++i)
    {
        auto* p = slab.allocateItem(120);
        EXPECT_NE(p, nullptr);
        items.push_back(p);
    }
    EXPECT_EQ(slab.getSlabCount(), 1u);

    // One past capacity → second buffer.
    items.push_back(slab.allocateItem(120));
    EXPECT_EQ(slab.getSlabCount(), 2u);
    EXPECT_EQ(slab.getAllocatedMemory(), 2 * kBuf);

    for (std::size_t i = 1; i < kSlots; ++i)
    {
        items.push_back(slab.allocateItem(120));
    }
    EXPECT_EQ(slab.getSlabCount(), 2u);

    items.push_back(slab.allocateItem(120));
    EXPECT_EQ(slab.getSlabCount(), 3u);
    EXPECT_EQ(slab.getAllocatedMemory(), 3 * kBuf);

    for (auto* p : items)
    {
        slab.deallocateItem(p);
    }

    // Reuse retained buffers (monotonic growth until trim).
    for (std::size_t i = 0; i < 2 * kSlots + 1; ++i)
    {
        EXPECT_NE(slab.allocateItem(120), nullptr);
    }
    EXPECT_EQ(slab.getAllocatedMemory(), 3 * kBuf);
}


TEST(SlabTest, DeallocateInvalidItem)
{
    Slab<256> slab;

    auto item1 = slab.allocateItem(200);
    EXPECT_NE(item1, nullptr);

    // invalid pointer (not from this slab)
    std::byte* invalid_item = reinterpret_cast<std::byte*>(std::malloc(256));
    EXPECT_THROW(slab.deallocateItem(invalid_item), std::invalid_argument);
    std::free(invalid_item);

    // Interior pointer into a live slot must not clear that (or any) bit.
    EXPECT_THROW(slab.deallocateItem(item1 + 1), std::invalid_argument);
    EXPECT_THROW(slab.deallocateItem(item1 + 128), std::invalid_argument);

    // Valid free still works after rejected interior frees.
    slab.deallocateItem(item1);

    // double free
    EXPECT_THROW(slab.deallocateItem(item1), std::invalid_argument);
}


// Size limits must throw in all build types (not only DEBUG via runtime_assert).
TEST(SlabTest, AllocateOversizedThrows)
{
    Slab<128> slab;
    EXPECT_THROW(slab.allocateItem(129), std::invalid_argument);
    EXPECT_NO_THROW(slab.allocateItem(128));
}


TEST(SlabProxyTest, SizeLimitsThrow)
{
    SlabProxy proxy;
    // Must be strictly greater than 1_KB; 1_KB itself is not a large allocation.
    EXPECT_THROW(proxy.allocateItem(1_KB), std::invalid_argument);
    EXPECT_NO_THROW({
        auto* p = proxy.allocateItem(1_KB + 1);
        proxy.deallocateItem(p);
    });
    EXPECT_THROW(proxy.allocateItem(kMaxAllocationBytes + 1), std::out_of_range);
}


TEST(SlabTest, Alignment)
{
    Slab<64> slab;

    std::vector<std::byte*> items;
    for (int i = 0; i < 10; ++i)
    {
        auto item = slab.allocateItem(60);
        EXPECT_NE(item, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(item) % 16, 0u); // 16-byte alignment
        items.push_back(item);
    }

    for (auto it : items)
    {
        slab.deallocateItem(it);
    }
}


TEST(PoolTest, Selector)
{
    Pool pool;

    // Default selectSlab(size) ⇒ axis 16
    EXPECT_EQ(pool.selectSlab(16), 0u);
    EXPECT_EQ(pool.selectSlab(32), 1u);
    EXPECT_EQ(pool.selectSlab(48), 2u);
    EXPECT_EQ(pool.selectSlab(64), 3u);
    EXPECT_EQ(pool.selectSlab(96), 4u);
    EXPECT_EQ(pool.selectSlab(128), 5u);
    EXPECT_EQ(pool.selectSlab(192), 6u);
    EXPECT_EQ(pool.selectSlab(256), 7u);
    EXPECT_EQ(pool.selectSlab(384), 8u);
    EXPECT_EQ(pool.selectSlab(512), 9u);
    EXPECT_EQ(pool.selectSlab(768), 10u);
    EXPECT_EQ(pool.selectSlab(1024), 11u);
    EXPECT_EQ(pool.selectSlab(1500), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(2000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(3000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(4000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(5000), std::numeric_limits<std::size_t>::max());

    EXPECT_EQ(pool.selectSlab(1), 0u);
    EXPECT_EQ(pool.selectSlab(15), 0u);
    EXPECT_EQ(pool.selectSlab(17), 1u);
    EXPECT_EQ(pool.selectSlab(31), 1u);
    EXPECT_EQ(pool.selectSlab(33), 2u);
    EXPECT_EQ(pool.selectSlab(47), 2u);
    EXPECT_EQ(pool.selectSlab(49), 3u);
    EXPECT_EQ(pool.selectSlab(63), 3u);
    EXPECT_EQ(pool.selectSlab(65), 4u);
    EXPECT_EQ(pool.selectSlab(95), 4u);
    EXPECT_EQ(pool.selectSlab(97), 5u);
    EXPECT_EQ(pool.selectSlab(127), 5u);
    EXPECT_EQ(pool.selectSlab(129), 6u);
    EXPECT_EQ(pool.selectSlab(191), 6u);
    EXPECT_EQ(pool.selectSlab(193), 7u);
    EXPECT_EQ(pool.selectSlab(255), 7u);
    EXPECT_EQ(pool.selectSlab(257), 8u);
    EXPECT_EQ(pool.selectSlab(383), 8u);
    EXPECT_EQ(pool.selectSlab(385), 9u);
    EXPECT_EQ(pool.selectSlab(511), 9u);
    EXPECT_EQ(pool.selectSlab(513), 10u);
    EXPECT_EQ(pool.selectSlab(767), 10u);
    EXPECT_EQ(pool.selectSlab(769), 11u);
    EXPECT_EQ(pool.selectSlab(1023), 11u);
    EXPECT_EQ(pool.selectSlab(1025), std::numeric_limits<std::size_t>::max());

    // Align 4/8 map to axis 16
    EXPECT_EQ(nativeAxis(4), 16u);
    EXPECT_EQ(nativeAxis(8), 16u);
    EXPECT_EQ(nativeAxis(16), 16u);
    EXPECT_EQ(nativeAxis(32), 32u);
    EXPECT_EQ(nativeAxis(64), 64u);
    EXPECT_EQ(pool.selectSlab(64, 8), pool.selectSlab(64));
    EXPECT_EQ(pool.selectSlab(64, 4), pool.selectSlab(64));

    // Explicit axis 32 ladder (incl. denser top)
    EXPECT_EQ(pool.selectSlab(32, 32), 0u);
    EXPECT_EQ(pool.selectSlab(96, 32), 2u);
    EXPECT_EQ(pool.selectSlab(1024, 32), 9u);
    EXPECT_EQ(pool.selectSlab(1536, 32), 10u);
    EXPECT_EQ(pool.selectSlab(2048, 32), 11u);
    EXPECT_EQ(pool.selectSlab(2049, 32), std::numeric_limits<std::size_t>::max());

    // Explicit axis 64 ladder
    EXPECT_EQ(pool.selectSlab(64, 64), 0u);
    EXPECT_EQ(pool.selectSlab(1024, 64), 7u);
    EXPECT_EQ(pool.selectSlab(1536, 64), 8u);
    EXPECT_EQ(pool.selectSlab(2048, 64), 9u);
    EXPECT_EQ(pool.selectSlab(3072, 64), 10u);
    EXPECT_EQ(pool.selectSlab(4096, 64), 11u);
    EXPECT_EQ(pool.selectSlab(4097, 64), std::numeric_limits<std::size_t>::max());
}


TEST(PoolTest, DeallocateValidatesHeader)
{
    Pool pool;

    auto* p = pool.allocate(64);
    ASSERT_NE(p, nullptr);

    // Corrupt magic at user − 6
    auto* live = pool.allocate(32);
    ASSERT_NE(live, nullptr);
    *(live - 6) = std::byte{0};
    EXPECT_THROW(pool.deallocate(live), std::invalid_argument);
    EXPECT_FALSE(pool.try_deallocate(live));

    // Corrupt header_size at user − 5 (keep magic intact on a fresh block)
    auto* live2 = pool.allocate(32);
    ASSERT_NE(live2, nullptr);
    *(live2 - 5) = std::byte{7};
    EXPECT_THROW(pool.deallocate(live2), std::invalid_argument);
    EXPECT_FALSE(pool.try_deallocate(live2));

    // Non-pool pointer
    std::byte stack_buf[64]{};
    EXPECT_THROW(pool.deallocate(stack_buf + 16), std::invalid_argument);
    EXPECT_FALSE(pool.try_deallocate(stack_buf + 16));

    // Double free at Pool layer (magic cleared on first free)
    pool.deallocate(p);
    EXPECT_THROW(pool.deallocate(p), std::invalid_argument);
    EXPECT_FALSE(pool.try_deallocate(p));
}


// Must pass under NDEBUG: interior user pointers are rejected via exceptions /
// try_deallocate false, not debug-only asserts.
TEST(PoolTest, DeallocateRejectsInteriorPointer)
{
    Pool pool;
    auto* p = pool.allocate(64);
    ASSERT_NE(p, nullptr);

    EXPECT_THROW(pool.deallocate(p + 1), std::invalid_argument);
    EXPECT_FALSE(pool.try_deallocate(p + 1));
    EXPECT_THROW(pool.deallocate(p + 32), std::invalid_argument);
    EXPECT_FALSE(pool.try_deallocate(p + 32));

    // Exact user pointer still frees cleanly after rejected interiors.
    EXPECT_NO_THROW(pool.deallocate(p));
}


TEST(PoolAllocatorTest, DeallocateIsNoexcept)
{
    Pool pool;
    PoolAllocator<int> alloc(pool);

    static_assert(noexcept(alloc.deallocate(nullptr, 0)));
    static_assert(std::is_nothrow_invocable_v<PoolDeleter<int>, int*>);

    int* p = alloc.allocate(1);
    ASSERT_NE(p, nullptr);
    alloc.deallocate(p, 1);

    // try_deallocate is the non-throwing policy under noexcept clients
    EXPECT_TRUE(pool.try_deallocate(nullptr));
    std::byte junk[32]{};
    EXPECT_FALSE(pool.try_deallocate(junk + 16));
}


TEST(PoolTest, AllocateItems)
{
    Pool pool;

    std::map<std::size_t, std::byte*> items;

    // allocate various sizes
    for (std::size_t size : {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                             160, 192, 224, 256, 320, 384, 448, 512, 640, 768,
                             896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
                             3584, 4096, 4512, 1500, 2000, 3000,
                             4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items[size] = item;
    }

    // free all items
    for (auto it : items)
    {
        pool.deallocate(it.second);
    }

    items.clear();

    // allocate again to ensure reuse of freed items
    for (std::size_t size : {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                             160, 192, 224, 256, 320, 384, 448, 512, 640, 768,
                             896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
                             3584, 4096, 4512, 1500, 2000, 3000,
                             4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items[size] = item;
    }

    // free all items again
    for (auto it : items)
    {
        pool.deallocate(it.second);
    }
}


TEST(PoolTest, uniqueBytePoolPtr)
{
    Pool pool;

    // Create the deleter lambda
    auto deleter = [&pool](std::byte* p) {
        pool.deallocate(p);
    };

    using unique_byte_pool_ptr = std::unique_ptr<std::byte, decltype(deleter)>;

    {
        unique_byte_pool_ptr item1(pool.allocate(128), deleter);
        EXPECT_NE(item1.get(), nullptr);

        unique_byte_pool_ptr item2(pool.allocate(2048), deleter);
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item3 = pool.allocate(512);
    EXPECT_NE(item3, nullptr);
    pool.deallocate(item3);

    using shared_byte_pool_ptr = std::shared_ptr<std::byte>;

    {
        shared_byte_pool_ptr item1(pool.allocate(256),
            [&pool](std::byte* p) { pool.deallocate(p); });
        EXPECT_NE(item1.get(), nullptr);

        shared_byte_pool_ptr item2(pool.allocate(4096),
            [&pool](std::byte* p) { pool.deallocate(p); });
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item4 = pool.allocate(1024);
    EXPECT_NE(item4, nullptr);
    pool.deallocate(item4);
}


TEST(PoolTest, sharedBytePoolPtr)
{
    Pool pool;

    // Create the deleter lambda
    auto deleter = [&pool](std::byte* p) {
        pool.deallocate(p);
    };

    using shared_byte_pool_ptr = std::shared_ptr<std::byte>;

    {
        shared_byte_pool_ptr item1(pool.allocate(128), deleter);
        EXPECT_NE(item1.get(), nullptr);

        shared_byte_pool_ptr item2(pool.allocate(2048), deleter);
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item3 = pool.allocate(512);
    EXPECT_NE(item3, nullptr);
    pool.deallocate(item3);

    using shared_byte_pool_ptr = std::shared_ptr<std::byte>;

    {
        shared_byte_pool_ptr item1(pool.allocate(256), deleter);
        EXPECT_NE(item1.get(), nullptr);

        shared_byte_pool_ptr item2(pool.allocate(4096), deleter);
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item4 = pool.allocate(1024);
    EXPECT_NE(item4, nullptr);
    pool.deallocate(item4);
}


TEST(PoolTest, uniquePoolPtr)
{
    Pool pool;

    {
        auto item1 = make_pool_unique<int>(pool, 128);
        EXPECT_NE(item1.get(), nullptr);
        EXPECT_EQ(*item1, 128);

        auto item2 = make_pool_unique<int>(pool);
        EXPECT_NE(item2.get(), nullptr);
        EXPECT_EQ(*item2, 0);
        *item2 = 42;
        EXPECT_EQ(*item2, 42);

        auto item3 = make_pool_unique<int[]>(pool, 10);
        EXPECT_NE(item3.get(), nullptr);

        for (int i = 0; i < 10; ++i)
        {
            item3[i] = i * 10;
        }

        for (int i = 0; i < 10; ++i)
        {
            EXPECT_EQ(item3[i], i * 10);
        }
    } // items go out of scope and are deallocated
}


// Throws on the Nth successful live object when constructed == throw_at.
// alignas(8): Pool::allocate requires alignment in [4, 64]; empty types are
// often alignof 1 and would hit the alignment check before the ctor path.
struct alignas(8) ThrowOnNthConstruct
{
    inline static int live = 0;
    inline static int throw_at = -1;

    ThrowOnNthConstruct()
    {
        if (live == throw_at)
        {
            throw std::runtime_error("ThrowOnNthConstruct");
        }
        ++live;
    }

    ~ThrowOnNthConstruct()
    {
        --live;
    }
};


TEST(PoolTest, uniquePoolPtrArrayStrongExceptionSafety)
{
    Pool pool;
    ThrowOnNthConstruct::live = 0;
    ThrowOnNthConstruct::throw_at = 3; // fail while constructing the 4th element (0-based)

    EXPECT_THROW(
        (void)make_pool_unique<ThrowOnNthConstruct[]>(pool, 5),
        std::runtime_error);

    // All partially constructed elements destroyed; pool block returned.
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);

    ThrowOnNthConstruct::throw_at = -1;
    auto ok = make_pool_unique<ThrowOnNthConstruct[]>(pool, 3);
    EXPECT_EQ(ThrowOnNthConstruct::live, 3);
    ok.reset();
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);
}


TEST(PoolTest, uniquePoolPtrSingleStrongExceptionSafety)
{
    Pool pool;
    ThrowOnNthConstruct::live = 0;
    ThrowOnNthConstruct::throw_at = 0; // first construction throws

    EXPECT_THROW(
        (void)make_pool_unique<ThrowOnNthConstruct>(pool),
        std::runtime_error);
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);

    ThrowOnNthConstruct::throw_at = -1;
    auto ok = make_pool_unique<ThrowOnNthConstruct>(pool);
    EXPECT_EQ(ThrowOnNthConstruct::live, 1);
    ok.reset();
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);
}


// Must pass under NDEBUG (exceptions, not runtime_assert).
TEST(PoolTest, sharedPoolPtrArrayStrongExceptionSafety)
{
    Pool pool;
    ThrowOnNthConstruct::live = 0;
    ThrowOnNthConstruct::throw_at = 3;

    EXPECT_THROW(
        (void)make_pool_shared<ThrowOnNthConstruct[]>(pool, 5),
        std::runtime_error);
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);

    ThrowOnNthConstruct::throw_at = -1;
    auto ok = make_pool_shared<ThrowOnNthConstruct[]>(pool, 3);
    EXPECT_EQ(ThrowOnNthConstruct::live, 3);
    ok.reset();
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);
}


TEST(PoolTest, sharedPoolPtrSingleStrongExceptionSafety)
{
    Pool pool;
    ThrowOnNthConstruct::live = 0;
    ThrowOnNthConstruct::throw_at = 0;

    EXPECT_THROW(
        (void)make_pool_shared<ThrowOnNthConstruct>(pool),
        std::runtime_error);
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);

    ThrowOnNthConstruct::throw_at = -1;
    auto ok = make_pool_shared<ThrowOnNthConstruct>(pool);
    EXPECT_EQ(ThrowOnNthConstruct::live, 1);
    ok.reset();
    EXPECT_EQ(ThrowOnNthConstruct::live, 0);
}


TEST(PoolTest, sharedPoolPtr)
{
    Pool pool;

    {
        auto item1 = make_pool_shared<int>(pool, 128);
        EXPECT_NE(item1.get(), nullptr);
        EXPECT_EQ(*item1, 128);

        auto item2 = make_pool_shared<int>(pool);
        EXPECT_NE(item2.get(), nullptr);
        EXPECT_EQ(*item2, 0);
        *item2 = 42;
        EXPECT_EQ(*item2, 42);

        auto item3 = make_pool_shared<int[]>(pool, 10);
        EXPECT_NE(item3.get(), nullptr);

        for (int i = 0; i < 10; ++i)
        {
            item3[i] = i * 10;
        }

        for (int i = 0; i < 10; ++i)
        {
            EXPECT_EQ(item3[i], i * 10);
        }

        item1 = item2;
        EXPECT_EQ(*item1, 42);
        EXPECT_EQ(*item2, 42);
        *item1 = 84;

        auto item4 = item3;
        for (int i = 0; i < 10; ++i)
        {
            EXPECT_EQ(item4[i], i * 10);
        }

        auto item5 = item1;
        EXPECT_EQ(*item5, 84);
        EXPECT_EQ(*item1, 84);
        EXPECT_EQ(*item2, 84);

        *item5 = 99;
        EXPECT_EQ(*item1, 99);
        EXPECT_EQ(*item2, 99);
        EXPECT_EQ(*item5, 99);
    } // items go out of scope and are deallocated
}


TEST(PoolTest, Alignment)
{
    Pool pool;

    for (size_t align : { 4, 8, 16, 32, 64 })
    {
        std::vector<std::byte*> items;
        // Fewer rounds for 32/64: lazy size-class slabs on first use.
        int rounds = (align > 16) ? 8 : 64;
        for (int i = 0; i <= rounds; ++i)
        {
            for (size_t size = 1; size <= 128; ++size)
            {
                auto item = pool.allocate(size, align);
                EXPECT_NE(item, nullptr);
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(item) % align, 0u);
                items.push_back(item);
            }
        }

        for (auto it : items)
        {
            pool.deallocate(it);
        }
    }

    EXPECT_THROW(pool.allocate(16, 128), std::invalid_argument);
    // Non-power-of-two must fail in all builds (not only debug asserts).
    EXPECT_THROW(pool.allocate(16, 12), std::invalid_argument);
    EXPECT_THROW(pool.allocate(16, 24), std::invalid_argument);
}


TEST(PoolTest, HighAlignmentFactories)
{
    Pool pool;

    struct alignas(32) AvxLike
    {
        std::byte data[32];
    };
    struct alignas(64) CacheLine
    {
        std::byte data[64];
    };

    {
        auto p = make_pool_unique<AvxLike>(pool);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p.get()) % 32, 0u);
    }
    {
        auto p = make_pool_unique<CacheLine>(pool);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p.get()) % 64, 0u);
    }
    {
        auto p = make_pool_shared<CacheLine>(pool);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p.get()) % 64, 0u);
    }
    {
        auto arr = make_pool_unique<CacheLine[]>(pool, 3);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(arr.get()) % 64, 0u);
    }
}


// High-align size classes reuse slots; over-axis-max uses SlabProxy.
TEST(PoolTest, HighAlignmentSlabReuseAndProxy)
{
    Pool pool;

    std::vector<std::byte*> first_round;
    for (int i = 0; i < 32; ++i)
    {
        auto* p = pool.allocate(64, 64);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64, 0u);
        first_round.push_back(p);
    }
    for (auto* p : first_round)
    {
        pool.deallocate(p);
    }

    // Re-allocate into the same axis-64 size class (lazy slab already live).
    for (int i = 0; i < 32; ++i)
    {
        auto* p = pool.allocate(64, 64);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64, 0u);
        pool.deallocate(p);
    }

    // Footprint > 4096 on axis 64 → Proxy (still 64-byte aligned storage).
    auto* big = pool.allocate(4100, 64);
    EXPECT_NE(big, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(big) % 64, 0u);
    pool.deallocate(big);

    auto* mid = pool.allocate(2000, 32);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(mid) % 32, 0u);
    pool.deallocate(mid);
}


TEST(PoolTest, MultiThreadTest)
{
    std::size_t const num_cores = std::thread::hardware_concurrency();
    println("Detected {} hardware threads", num_cores);
    ASSERT_GT(num_cores, 0u);
    const std::size_t num_threads = num_cores * 8 / 10;

    Pool pool;

    constexpr int allocations_per_thread = 10000;

    // Seed on the main thread only — std::random_device is not required to be
    // thread-safe, so workers must not share a single device.
    std::random_device rd;
    std::vector<std::uint32_t> seeds(num_threads);
    for (std::uint32_t& seed : seeds)
    {
        seed = rd();
    }

    auto worker = [&pool](std::uint32_t seed) {
        std::mt19937 gen(seed);
        std::uniform_int_distribution<int> dist(0, 1000);
        std::vector<std::byte*> items;
        for (int i = 0; i < allocations_per_thread; ++i)
        {
            size_t size = 16 + dist(gen);
            auto item = pool.allocate(size);
            EXPECT_NE(item, nullptr);
            items.push_back(item);
        }

        for (auto it : items)
        {
            pool.deallocate(it);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(worker, seeds[i]);
    }

    for (auto& t : threads)
    {
        t.join();
    }
}



void pre_test()
{
    println("Running pre-test setup...");
}


int main(int argc, char** argv)
{
    pre_test();

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
