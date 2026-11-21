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

#ifndef POOL_HPP_
#define POOL_HPP_

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

#include "slab.hpp"
#include "pool_lifetime.hpp"

#if defined(__SANITIZE_ADDRESS__)
#  define SPALLOCATOR_HAS_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define SPALLOCATOR_HAS_ASAN 1
#  endif
#endif
#if defined(SPALLOCATOR_HAS_ASAN)
#  include <sanitizer/asan_interface.h>
#endif


namespace spallocator
{

    // Alignment contract for Pool::allocate:
    // - Powers of two in [kMinAlignment, kMaxAlignment]
    // - Request aligns 4/8 map to native alignment class 16 (slot over-align is fine)
    // - Alignment classes 16 / 32 / 64 use Slab<ElemSize, Alignment> size ladders
    // - Beyond that alignment class's max footprint → SlabProxy (always align_val_t{64})
    // - Returned memory is exclusively owned until matching deallocate;
    //   do not free non-pool pointers or mutate the hidden header
    inline constexpr std::size_t kMinAlignment = 4;
    inline constexpr std::size_t kSlabNativeAlignment = 16;
    inline constexpr std::size_t kMaxAlignment = 64;

    // Default request alignment for Pool::allocate — matches ::operator new
    // (C++17 __STDCPP_DEFAULT_NEW_ALIGNMENT__). Prefer that macro over
    // alignof(std::max_align_t): both are often 16, but the macro is the
    // allocation-function guarantee rather than a scalar-type property.
    inline constexpr std::size_t kDefaultAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    static_assert(kDefaultAlignment >= kMinAlignment &&
                      kDefaultAlignment <= kMaxAlignment,
                  "kDefaultAlignment must be within Pool alignment range");
    static_assert((kDefaultAlignment & (kDefaultAlignment - 1)) == 0,
                  "kDefaultAlignment must be a power of two");
    inline constexpr std::size_t kSlabIndexNpos = std::numeric_limits<std::size_t>::max();
    // Written at user_ptr − 6; validated (then cleared) on deallocate.
    inline constexpr std::uint8_t kAllocMagic = 0xA7;

    inline constexpr std::size_t kLadder16[] = {
        16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096};
    inline constexpr std::size_t kLadder32[] = {
        32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192};
    inline constexpr std::size_t kLadder64[] = {
        64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384};

    inline constexpr std::size_t kLadder16Count =
        sizeof(kLadder16) / sizeof(kLadder16[0]);
    inline constexpr std::size_t kLadder32Count =
        sizeof(kLadder32) / sizeof(kLadder32[0]);
    inline constexpr std::size_t kLadder64Count =
        sizeof(kLadder64) / sizeof(kLadder64[0]);

    // Pool cold-start: which size-class engines map a first data block in
    // Pool(). Engines for every alignment class are always constructed (immutable
    // for the pool lifetime → lock-free resolve). Override via -D / ./configure.
#ifndef SPALLOCATOR_PREALLOCATE_MAX_ELEM_SIZE
#  define SPALLOCATOR_PREALLOCATE_MAX_ELEM_SIZE 256u
#endif
#ifndef SPALLOCATOR_PREALLOCATE_MAX_ALIGNMENT
#  define SPALLOCATOR_PREALLOCATE_MAX_ALIGNMENT 16u
#endif

    inline constexpr std::size_t kPreallocateMaxElemSize =
        SPALLOCATOR_PREALLOCATE_MAX_ELEM_SIZE;
    inline constexpr std::size_t kPreallocateMaxAlignment =
        SPALLOCATOR_PREALLOCATE_MAX_ALIGNMENT;

    static_assert(
        kPreallocateMaxAlignment == 16 || kPreallocateMaxAlignment == 32 ||
            kPreallocateMaxAlignment == 64,
        "kPreallocateMaxAlignment must be 16, 32, or 64");

    // Preallocate a first data block when both thresholds pass (ElemSize is
    // the ladder rung; Alignment is the alignment class). Default: alignment class 16 and
    // ElemSize <= 256. Raise max alignment to also warm 32/64 classes.
    constexpr bool shouldPreallocateFirstBlock(std::size_t elem_size,
                                               std::size_t alignment) noexcept
    {
        return elem_size <= kPreallocateMaxElemSize &&
               alignment <= kPreallocateMaxAlignment;
    }

    template<std::size_t ElemSize, std::size_t Alignment>
    inline std::unique_ptr<ISlab> makePoolSlab()
    {
        constexpr bool pre =
            shouldPreallocateFirstBlock(ElemSize, Alignment);
        return std::make_unique<Slab<ElemSize, Alignment>>(pre);
    }

    // Map request alignment to a native alignment class (4/8 → 16).
    constexpr std::size_t nativeAlignmentClass(std::size_t alignment)
    {
        if (alignment <= kSlabNativeAlignment)
        {
            return kSlabNativeAlignment;
        }
        return alignment;
    }

    constexpr std::size_t alignmentClassMaxElem(std::size_t alignment_class)
    {
        // Keep in sync with kLadder* tails (last rung = max size-class ElemSize).
        if (alignment_class <= kSlabNativeAlignment)
        {
            return kLadder16[kLadder16Count - 1];
        }
        if (alignment_class == 32)
        {
            return kLadder32[kLadder32Count - 1];
        }
        return kLadder64[kLadder64Count - 1];
    }

    constexpr std::size_t selectClassIndex(std::size_t alloc_size, std::size_t alignment_class)
    {
        const std::size_t* ladder = nullptr;
        std::size_t count = 0;
        if (alignment_class <= kSlabNativeAlignment)
        {
            ladder = kLadder16;
            count = kLadder16Count;
        }
        else if (alignment_class == 32)
        {
            ladder = kLadder32;
            count = kLadder32Count;
        }
        else
        {
            ladder = kLadder64;
            count = kLadder64Count;
        }

        for (std::size_t i = 0; i < count; ++i)
        {
            if (alloc_size <= ladder[i])
            {
                return i;
            }
        }
        return kSlabIndexNpos;
    }


    // Hot-path success/fail tallies (alloc_ok / free_ok / *_fail). Off by
    // default — each bump is a locked atomic RMW on every op. Enable with
    // -DSPALLOCATOR_POOL_COUNTERS=1 or ./configure --enable-pool-counters.
    // live_allocs_ (lifetime) and live_hwm_ (cheap relaxed max) stay on.
#ifndef SPALLOCATOR_POOL_COUNTERS
#  define SPALLOCATOR_POOL_COUNTERS 0
#endif
    inline constexpr bool kPoolCountersEnabled = (SPALLOCATOR_POOL_COUNTERS != 0);

    // Best-effort ops snapshot. Counters use relaxed atomics; fields may
    // not form a single transactional freeze under concurrency.
    struct PoolStats
    {
        std::uint64_t live_allocations{0};     // outstanding successful allocs
        std::uint64_t retained_mapped_bytes{0};  // mapped Block bytes held (not live "in use")
        std::uint64_t alloc_ok{0};             // 0 unless kPoolCountersEnabled
        std::uint64_t alloc_fail{0};           // size/align/cap/slab throws
        std::uint64_t free_ok{0};              // successful non-null frees
        std::uint64_t free_fail{0};            // try_deallocate returned false
        std::uint64_t live_hwm{0};             // peak live_allocations (relaxed max)
        std::uint64_t retained_hwm{0};         // peak retained_mapped_bytes observed
    };


    class Pool
    {
    public: // methods
        std::byte* allocate(std::size_t size,
                            std::size_t alignment = kDefaultAlignment);

        // Throws std::invalid_argument (or from the slab layer) on bad pointers.
        void deallocate(std::byte* item);

        // Non-throwing free for noexcept clients (PoolAllocator, PoolDeleter).
        // Returns false if the pointer is not a live pool allocation.
        bool try_deallocate(std::byte* item) noexcept;

        // Release fully empty size-class slab buffers back to the OS.
        // Default growth is monotonic (free reuses slots but does not shrink);
        // call trim() at quiet points when peak RSS should be returned.
        // keep_min is per size class (clamped to ≥ 1). Returns slabs deleted.
        std::size_t trim(std::size_t keep_min = 1);

        // Sum of size-class slab buffer bytes currently retained (not SlabProxy).
        std::size_t getAllocatedMemory();

        // Outstanding user allocations (successful allocate minus successful free).
        std::uint64_t live_allocation_count() const noexcept;

        // Best-effort counters + high-water marks; see PoolStats.
        PoolStats stats();

        // Handle sharing this pool's control block (bumps handle_refs + block_refs).
        PoolHandle handle();

        // Deleter/allocator token (block_refs only).
        PoolLifetimeToken lifetime_token();

        PoolControlBlock* control() const noexcept { return control_; }

        Pool();
        ~Pool();

        // Class selection: use free function selectClassIndex(footprint, alignment_class).

    private: // methods
        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;
        Pool(Pool&&) = delete;
        Pool& operator=(Pool&&) = delete;

        ISlab* resolveSlab(std::size_t alloc_size, std::size_t alignment_class);

        void note_alloc_ok() noexcept;
        void note_alloc_fail() noexcept;
        void note_free_ok() noexcept;
        void note_free_fail() noexcept;
        void observe_live_hwm(std::uint64_t live) noexcept;
        void observe_retained_hwm(std::uint64_t retained_bytes) noexcept;

        // Scratch for allocate's header path (not part of the public API).
        struct Allocation
        {
            std::byte* ptr;
            std::size_t size;
        };

    private: // data members
        PoolControlBlock* control_{nullptr};

        // Eager, immutable registries: every ladder rung is installed in
        // Pool() and never replaced/nulled. Resolve is lock-free on all alignment classes;
        // per-slab locks still protect freemaps / data-block growth.
        // Whether a class maps a first data block at birth is controlled by
        // kPreallocateMaxElemSize / kPreallocateMaxAlignment.
        std::array<std::unique_ptr<ISlab>, kLadder16Count> slabs16{};
        std::array<std::unique_ptr<ISlab>, kLadder32Count> slabs32{};
        std::array<std::unique_ptr<ISlab>, kLadder64Count> slabs64{};
        SlabProxy large_slab;

#if SPALLOCATOR_POOL_COUNTERS
        std::atomic<std::uint64_t> alloc_ok_{0};
        std::atomic<std::uint64_t> alloc_fail_{0};
        std::atomic<std::uint64_t> free_ok_{0};
        std::atomic<std::uint64_t> free_fail_{0};
#endif
        std::atomic<std::uint64_t> live_hwm_{0};
        std::atomic<std::uint64_t> retained_hwm_{0};
    };


    // ---------------------------------------------------------------------------
    // Lifetime methods that need a complete Pool
    // ---------------------------------------------------------------------------

    inline void PoolControlBlock::release_handle_ref() noexcept
    {
        if (handle_refs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            if (Pool* doomed = try_retire())
            {
                delete doomed;
            }
        }
    }


    inline PoolHandle PoolHandle::create()
    {
        Pool* pool = new Pool();
        pool->control()->set_heap_owned();
        return PoolHandle(pool->control());
    }


    // ---------------------------------------------------------------------------
    // Pool
    // ---------------------------------------------------------------------------

    inline ISlab* Pool::resolveSlab(std::size_t alloc_size, std::size_t alignment_class)
    {
        const std::size_t index = selectClassIndex(alloc_size, alignment_class);
        if (index == kSlabIndexNpos)
        {
            return &large_slab;
        }

        // All alignment-class engines are eager and immutable for the pool lifetime.
        // trim/reclaim only drops empty Blocks inside each engine, never
        // the unique_ptr entry — so resolve stays lock-free on every class.
        if (alignment_class <= kSlabNativeAlignment)
        {
            return slabs16[index].get();
        }
        if (alignment_class == 32)
        {
            return slabs32[index].get();
        }
        return slabs64[index].get();
    }


    std::byte* Pool::allocate(std::size_t item_size,
                              std::size_t alignment /* = kDefaultAlignment */)
    {
        Allocation alloc;
        alloc.size = item_size;

        if (item_size > kMaxAllocationBytes)
        {
            // excessively large allocations don't belong in the pool
            // slabs; use other allocation methods instead
            note_alloc_fail();
            throw std::out_of_range("Allocation size exceeds maximum limit for pool allocator");
        }

        // Range and power-of-two checks throw in all builds (including release).
        // Non-power-of-two values in [4, 64] (e.g. 12) would break header/alignment-class math.
        if (alignment < kMinAlignment || alignment > kMaxAlignment ||
            (alignment & (alignment - 1)) != 0)
        {
            note_alloc_fail();
            throw std::invalid_argument("Unsupported alignment requested");
        }

        // Store footprint metadata in a header immediately before the user
        // pointer.
        // header_size = max(8, alignment). For align 4/8 this stays 8 (alignment class 16);
        // for 16/32/64 it equals the request align so free can recover the class.
        // Layout (from user pointer backward): uint32 alloc_size at −4,
        // uint8 header_size at −5, uint8 magic at −6. Deallocate validates
        // magic/size then recovers the slot base and re-selects the slab via
        // (alloc_size, alignment-class-from-header_size).
        std::size_t header_size = 8 < alignment ? alignment : 8;
        std::size_t alloc_size = item_size + header_size;
        alloc.size = alloc_size;

        const std::size_t alignment_class = nativeAlignmentClass(alignment);
        ISlab* slab = resolveSlab(alloc_size, alignment_class);
        debug_println("Allocated {} bytes, alignment_class={}, slab={}",
                      alloc_size, alignment_class,
                      (slab == &large_slab) ? "large_slab" : "size_class");

        try
        {
            alloc.ptr = slab->allocateItem(alloc_size);
        }
        catch (...)
        {
            note_alloc_fail();
            throw;
        }

#if defined(SPALLOCATOR_HAS_ASAN)
        // Slot may have been header-poisoned on a prior free.
        ASAN_UNPOISON_MEMORY_REGION(alloc.ptr, header_size);
#endif

        uint32_t* size_ptr = reinterpret_cast<uint32_t*>(alloc.ptr + header_size - 4);
        *size_ptr = uint32_t(alloc_size & 0xffffffff);

        uint8_t* header_size_ptr = reinterpret_cast<uint8_t*>(alloc.ptr + header_size - 5);
        *header_size_ptr = uint8_t(header_size & 0xff);

        uint8_t* magic_ptr = reinterpret_cast<uint8_t*>(alloc.ptr + header_size - 6);
        *magic_ptr = kAllocMagic;

        control_->note_alloc();
        note_alloc_ok();
        return alloc.ptr + header_size;
    }


    void Pool::deallocate(std::byte* item)
    {
        if (!try_deallocate(item))
        {
            throw std::invalid_argument(
                "Pool::deallocate failed (invalid pointer, corrupt header, or double free)");
        }
    }


    bool Pool::try_deallocate(std::byte* item) noexcept
    {
        if (item == nullptr)
        {
            return true;
        }

        try
        {
            // Metadata always lives in the 8 bytes immediately before the user
            // pointer. A prior free poisons the header; temporarily unpoison so
            // double-free / corrupt-header checks can read it without ASan abort.
            [[maybe_unused]] constexpr std::size_t kHeaderPeek = 8;
#if defined(SPALLOCATOR_HAS_ASAN)
            const bool header_was_poisoned =
                __asan_address_is_poisoned(item - 5) != 0;
            ASAN_UNPOISON_MEMORY_REGION(item - kHeaderPeek, kHeaderPeek);
#endif

            // Validate header before trusting it to choose a slab (P9).
            uint8_t header_size = *reinterpret_cast<uint8_t*>(item - 5);
            if (header_size != 8 && header_size != 16 &&
                header_size != 32 && header_size != 64)
            {
                debug_println("Invalid allocation header_size {} at ptr={}",
                              header_size, static_cast<void*>(item));
#if defined(SPALLOCATOR_HAS_ASAN)
                if (header_was_poisoned)
                {
                    ASAN_POISON_MEMORY_REGION(item - kHeaderPeek, kHeaderPeek);
                }
#endif
                note_free_fail();
                return false;
            }

            uint8_t magic = *reinterpret_cast<uint8_t*>(item - 6);
            if (magic != kAllocMagic)
            {
                debug_println(
                    "Invalid allocation magic {:#x} at ptr={} "
                    "(not a live pool pointer, or double free)",
                    magic, static_cast<void*>(item));
#if defined(SPALLOCATOR_HAS_ASAN)
                if (header_was_poisoned)
                {
                    ASAN_POISON_MEMORY_REGION(item - header_size, header_size);
                }
#endif
                note_free_fail();
                return false;
            }

            uint32_t alloc_size_u32 = *reinterpret_cast<uint32_t*>(item - 4);
            std::size_t alloc_size = alloc_size_u32;
            if (alloc_size < header_size ||
                alloc_size > kMaxAllocationBytes + kMaxAlignment)
            {
                debug_println(
                    "Invalid allocation size {} in header at ptr={} (header_size={})",
                    alloc_size, static_cast<void*>(item), header_size);
#if defined(SPALLOCATOR_HAS_ASAN)
                if (header_was_poisoned)
                {
                    ASAN_POISON_MEMORY_REGION(item - header_size, header_size);
                }
#endif
                note_free_fail();
                return false;
            }

            std::byte* original_ptr = item - header_size;

            // header_size 8 or 16 → alignment class 16; 32 → 32; 64 → 64.
            const std::size_t alignment_class = (header_size <= kSlabNativeAlignment)
                ? kSlabNativeAlignment
                : static_cast<std::size_t>(header_size);

            // Invalidate header before returning the slot (detects Pool double-free).
            *reinterpret_cast<uint8_t*>(item - 6) = 0;

#if defined(SPALLOCATOR_HAS_ASAN)
            ASAN_POISON_MEMORY_REGION(original_ptr, header_size);
#endif

            ISlab* slab = resolveSlab(alloc_size, alignment_class);
            debug_println("Deallocating {} bytes at ptr={}, alignment_class={}, slab={}",
                          alloc_size, static_cast<void*>(original_ptr), alignment_class,
                          (slab == &large_slab) ? "large_slab" : "size_class");
            slab->deallocateItem(original_ptr);
            // May return `this` for heap-owned idle pools — delete only after
            // this method is done touching members.
            Pool* doomed = control_->note_free();
            note_free_ok();
            if (doomed)
            {
                delete doomed;
            }
            return true;
        }
        catch (...)
        {
            // Slab layer may throw; noexcept clients must not see that.
            debug_println(
                "Pool::try_deallocate: slab deallocate failed for ptr={}",
                static_cast<void*>(item));
            note_free_fail();
            return false;
        }
    }


    std::size_t Pool::trim(std::size_t keep_min)
    {
        std::size_t reclaimed = 0;

        for (auto& slab : slabs16)
        {
            if (slab)
            {
                reclaimed += slab->reclaimEmptyBlocks(keep_min);
            }
        }
        for (auto& slab : slabs32)
        {
            if (slab)
            {
                reclaimed += slab->reclaimEmptyBlocks(keep_min);
            }
        }
        for (auto& slab : slabs64)
        {
            if (slab)
            {
                reclaimed += slab->reclaimEmptyBlocks(keep_min);
            }
        }

        debug_println("Pool::trim(keep_min={}): reclaimed {} slabs", keep_min, reclaimed);
        observe_retained_hwm(getAllocatedMemory());
        return reclaimed;
    }


    std::size_t Pool::getAllocatedMemory()
    {
        std::size_t total = 0;
        for (auto& slab : slabs16)
        {
            if (slab)
            {
                total += slab->getAllocatedMemory();
            }
        }
        for (auto& slab : slabs32)
        {
            if (slab)
            {
                total += slab->getAllocatedMemory();
            }
        }
        for (auto& slab : slabs64)
        {
            if (slab)
            {
                total += slab->getAllocatedMemory();
            }
        }
        return total;
    }


    inline std::uint64_t Pool::live_allocation_count() const noexcept
    {
        return control_ ? control_->live_allocation_count() : 0;
    }


    inline void Pool::observe_retained_hwm(std::uint64_t retained_bytes) noexcept
    {
        std::uint64_t cur = retained_hwm_.load(std::memory_order_relaxed);
        while (retained_bytes > cur &&
               !retained_hwm_.compare_exchange_weak(
                   cur, retained_bytes,
                   std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }
    }


    inline void Pool::observe_live_hwm(std::uint64_t live) noexcept
    {
        // Best-effort peak: relaxed load/store (no CAS). Concurrent allocs may
        // rarely under-report HWM; PoolStats already documents that looseness.
        const std::uint64_t cur = live_hwm_.load(std::memory_order_relaxed);
        if (live > cur)
        {
            live_hwm_.store(live, std::memory_order_relaxed);
        }
    }


    inline void Pool::note_alloc_ok() noexcept
    {
#if SPALLOCATOR_POOL_COUNTERS
        alloc_ok_.fetch_add(1, std::memory_order_relaxed);
#endif
        observe_live_hwm(live_allocation_count());
    }


    inline void Pool::note_alloc_fail() noexcept
    {
#if SPALLOCATOR_POOL_COUNTERS
        alloc_fail_.fetch_add(1, std::memory_order_relaxed);
#endif
    }


    inline void Pool::note_free_ok() noexcept
    {
#if SPALLOCATOR_POOL_COUNTERS
        free_ok_.fetch_add(1, std::memory_order_relaxed);
#endif
    }


    inline void Pool::note_free_fail() noexcept
    {
#if SPALLOCATOR_POOL_COUNTERS
        free_fail_.fetch_add(1, std::memory_order_relaxed);
#endif
    }


    inline PoolStats Pool::stats()
    {
        PoolStats s;
        s.live_allocations = live_allocation_count();
        s.retained_mapped_bytes = getAllocatedMemory();
        observe_live_hwm(s.live_allocations);
        observe_retained_hwm(s.retained_mapped_bytes);
#if SPALLOCATOR_POOL_COUNTERS
        s.alloc_ok = alloc_ok_.load(std::memory_order_relaxed);
        s.alloc_fail = alloc_fail_.load(std::memory_order_relaxed);
        s.free_ok = free_ok_.load(std::memory_order_relaxed);
        s.free_fail = free_fail_.load(std::memory_order_relaxed);
#endif
        s.live_hwm = live_hwm_.load(std::memory_order_relaxed);
        s.retained_hwm = retained_hwm_.load(std::memory_order_relaxed);
        return s;
    }


    inline PoolHandle Pool::handle()
    {
        return PoolHandle(control_);
    }


    inline PoolLifetimeToken Pool::lifetime_token()
    {
        return PoolLifetimeToken(control_);
    }


    Pool::Pool()
        : control_(PoolControlBlock::create(this))
    {
        // Every alignment-class engine is filled here (immutable thereafter). First
        // data-block mapping follows shouldPreallocateFirstBlock (defaults:
        // alignment class ≤16 and ElemSize ≤256).
        static_assert(kLadder16Count == 16 && kLadder32Count == 16 && kLadder64Count == 16,
            "Pool ctor installs one engine per ladder rung; update makePoolSlab list when counts change");

        slabs16[0]  = makePoolSlab<16, 16>();
        slabs16[1]  = makePoolSlab<32, 16>();
        slabs16[2]  = makePoolSlab<48, 16>();
        slabs16[3]  = makePoolSlab<64, 16>();
        slabs16[4]  = makePoolSlab<96, 16>();
        slabs16[5]  = makePoolSlab<128, 16>();
        slabs16[6]  = makePoolSlab<192, 16>();
        slabs16[7]  = makePoolSlab<256, 16>();
        slabs16[8]  = makePoolSlab<384, 16>();
        slabs16[9]  = makePoolSlab<512, 16>();
        slabs16[10] = makePoolSlab<768, 16>();
        slabs16[11] = makePoolSlab<1024, 16>();
        slabs16[12] = makePoolSlab<1536, 16>();
        slabs16[13] = makePoolSlab<2048, 16>();
        slabs16[14] = makePoolSlab<3072, 16>();
        slabs16[15] = makePoolSlab<4096, 16>();

        slabs32[0]  = makePoolSlab<32, 32>();
        slabs32[1]  = makePoolSlab<64, 32>();
        slabs32[2]  = makePoolSlab<96, 32>();
        slabs32[3]  = makePoolSlab<128, 32>();
        slabs32[4]  = makePoolSlab<192, 32>();
        slabs32[5]  = makePoolSlab<256, 32>();
        slabs32[6]  = makePoolSlab<384, 32>();
        slabs32[7]  = makePoolSlab<512, 32>();
        slabs32[8]  = makePoolSlab<768, 32>();
        slabs32[9]  = makePoolSlab<1024, 32>();
        slabs32[10] = makePoolSlab<1536, 32>();
        slabs32[11] = makePoolSlab<2048, 32>();
        slabs32[12] = makePoolSlab<3072, 32>();
        slabs32[13] = makePoolSlab<4096, 32>();
        slabs32[14] = makePoolSlab<6144, 32>();
        slabs32[15] = makePoolSlab<8192, 32>();

        slabs64[0]  = makePoolSlab<64, 64>();
        slabs64[1]  = makePoolSlab<128, 64>();
        slabs64[2]  = makePoolSlab<192, 64>();
        slabs64[3]  = makePoolSlab<256, 64>();
        slabs64[4]  = makePoolSlab<384, 64>();
        slabs64[5]  = makePoolSlab<512, 64>();
        slabs64[6]  = makePoolSlab<768, 64>();
        slabs64[7]  = makePoolSlab<1024, 64>();
        slabs64[8]  = makePoolSlab<1536, 64>();
        slabs64[9]  = makePoolSlab<2048, 64>();
        slabs64[10] = makePoolSlab<3072, 64>();
        slabs64[11] = makePoolSlab<4096, 64>();
        slabs64[12] = makePoolSlab<6144, 64>();
        slabs64[13] = makePoolSlab<8192, 64>();
        slabs64[14] = makePoolSlab<12288, 64>();
        slabs64[15] = makePoolSlab<16384, 64>();
    }


    Pool::~Pool()
    {
        // Publish dead before member unique_ptrs tear down slabs so any later
        // control-block observer (handles / deleters) sees !alive.
        if (control_)
        {
            control_->mark_dead();
            control_->release_block_ref();
            control_ = nullptr;
        }
    }

}; // namespace spallocator


#endif // POOL_HPP_
