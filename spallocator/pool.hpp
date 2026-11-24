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
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <stdexcept>

#include "impl/block_registry.hpp"
#include "impl/islab.hpp"
#include "impl/slab.hpp"
#include "impl/slab_proxy.hpp"
#include "impl/slot_tag.hpp"
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
    // - Size-class: user ptr == slot base; 8 B end tag. Proxy: front header.
    // - Returned memory is exclusively owned until matching deallocate;
    //   do not free non-pool pointers or mutate the end tag / front header
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
    // Proxy front-header LIVE magic (user_ptr − 6). Size-class uses kTagMagicLive.
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

        // Block map diagnostics (size-class free routing table).
        std::size_t block_owner_count() const;
        bool block_owner_contains(std::byte* block_base) const;

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

        void install_block_registry();

        // Shared map lookup: upper_bound predecessor + range + exact slot base.
        // Copies BlockEntry then unlocks. nullopt ⇒ not a size-class Block slot.
        std::optional<BlockEntry> find_block_owner(std::byte* p) const noexcept;

        // Scratch for allocate (not part of the public API).
        struct Allocation
        {
            std::byte* ptr;
            std::size_t size;
        };

        // Nested observer so Slabs do not need a public Pool* callback surface.
        class Registry final : public BlockRegistry
        {
        public:
            explicit Registry(Pool& pool) : pool_(pool) {}
            void on_block_born(std::byte* base, const BlockEntry& entry) override;
            void on_block_died(std::byte* base) override;

        private:
            Pool& pool_;
        };

    private: // data members
        PoolControlBlock* control_{nullptr};

        // Block ownership map for size-class free routing. Declared *before*
        // size-class Slabs so ~Slab can unregister while map/registry live.
        // Shared lookup on free; exclusive mutate on grow/trim.
        mutable std::shared_mutex block_map_lock_;
        std::map<std::byte*, BlockEntry> block_owner_;
        Registry block_registry_{*this};

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
            note_alloc_fail();
            throw std::out_of_range("Allocation size exceeds maximum limit for pool allocator");
        }

        // Range and power-of-two checks throw in all builds (including release).
        if (alignment < kMinAlignment || alignment > kMaxAlignment ||
            (alignment & (alignment - 1)) != 0)
        {
            note_alloc_fail();
            throw std::invalid_argument("Unsupported alignment requested");
        }

        const std::size_t alignment_class = nativeAlignmentClass(alignment);

        // Size-class: fixed 8 B end tag. Proxy: front header
        // header_size = max(16, alignment) ∈ {16,32,64}.
        const bool use_proxy =
            selectClassIndex(item_size + kSizeClassTrailerTax, alignment_class) ==
            kSlabIndexNpos;

        std::size_t header_size = 0;
        std::size_t alloc_size = 0;
        if (use_proxy)
        {
            header_size = alignment < 16 ? 16 : alignment;
            alloc_size = item_size + header_size;
        }
        else
        {
            alloc_size = item_size + kSizeClassTrailerTax;
        }
        alloc.size = alloc_size;

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

        if (slab == &large_slab)
        {
            // Proxy: front header immediately before user pointer.
            std::byte* base = alloc.ptr;
            std::byte* user = base + header_size;
#if defined(SPALLOCATOR_HAS_ASAN)
            ASAN_UNPOISON_MEMORY_REGION(base, header_size);
#endif
            std::memset(base, 0, header_size);

            const auto alloc_size_u32 = static_cast<std::uint32_t>(alloc_size);
            std::memcpy(user - 4, &alloc_size_u32, 4);

            *reinterpret_cast<std::uint8_t*>(user - 5) =
                static_cast<std::uint8_t>(header_size);
            *reinterpret_cast<std::uint8_t*>(user - 6) = kAllocMagic;

            const std::uint32_t crc = proxy_header_crc(
                user, alloc_size_u32, static_cast<std::uint8_t>(header_size), kAllocMagic);
            std::memcpy(user - 16, &crc, 4);

            control_->note_alloc();
            note_alloc_ok();
            return user;
        }

        // Size-class: user pointer == slot base; tag at ElemSize - 8.
        std::byte* slot = alloc.ptr;
        std::optional<BlockEntry> entry = find_block_owner(slot);
        if (!entry || entry->slab != slab)
        {
            // Best-effort rollback so the Slot is not leaked; ignore secondary
            // failures — the logic_error below is the primary signal.
            try
            {
                slab->deallocateItem(slot);
            }
            catch (...)
            {
            }
            note_alloc_fail();
            throw std::logic_error("Pool::allocate: Block map missing newly allocated slot");
        }

#if defined(SPALLOCATOR_HAS_ASAN)
        // Prior free poisoned the tag word; unpoison the whole Slot for reuse.
        ASAN_UNPOISON_MEMORY_REGION(slot, entry->elem_size);
#endif
        write_slot_tag(slot, entry->elem_size, entry->generation);

        control_->note_alloc();
        note_alloc_ok();
        return slot;
    }


    void Pool::deallocate(std::byte* item)
    {
        if (!try_deallocate(item))
        {
            throw std::invalid_argument(
                "Pool::deallocate failed (invalid pointer, corrupt tag/header, or double free)");
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
            // Size-class free: map shared lookup → unlock → tag/CRC → slab.
            // Else strict proxy front-header probe. Never hold the map lock
            // across slab work. See docs/design/slot-end-tag.md.
            if (std::optional<BlockEntry> entry = find_block_owner(item))
            {
#if defined(SPALLOCATOR_HAS_ASAN)
                ASAN_UNPOISON_MEMORY_REGION(
                    item + entry->elem_size - kSlotTagBytes, kSlotTagBytes);
#endif
                if (!verify_slot_tag(item, entry->elem_size, entry->generation))
                {
                    debug_println(
                        "Invalid size-class tag at ptr={} (corrupt, freed, or gen mismatch)",
                        static_cast<void*>(item));
#if defined(SPALLOCATOR_HAS_ASAN)
                    ASAN_POISON_MEMORY_REGION(
                        item + entry->elem_size - kSlotTagBytes, kSlotTagBytes);
#endif
                    note_free_fail();
                    return false;
                }

                clear_slot_tag(item, entry->elem_size);
#if defined(SPALLOCATOR_HAS_ASAN)
                ASAN_POISON_MEMORY_REGION(
                    item + entry->elem_size - kSlotTagBytes, kSlotTagBytes);
#endif

                debug_println("Deallocating size-class slot at ptr={}, elem_size={}",
                              static_cast<void*>(item), entry->elem_size);
                entry->slab->deallocateItem(item);
                Pool* doomed = control_->note_free();
                note_free_ok();
                if (doomed)
                {
                    delete doomed;
                }
                return true;
            }

            // Proxy path: front header at user − header_size (min 16).
            [[maybe_unused]] constexpr std::size_t kHeaderPeek = 16;
#if defined(SPALLOCATOR_HAS_ASAN)
            const bool header_was_poisoned =
                __asan_address_is_poisoned(item - 5) != 0;
            ASAN_UNPOISON_MEMORY_REGION(item - kHeaderPeek, kHeaderPeek);
#endif

            const std::uint8_t header_size = *reinterpret_cast<std::uint8_t*>(item - 5);
            if (header_size != 16 && header_size != 32 && header_size != 64)
            {
                debug_println("Invalid proxy header_size {} at ptr={}",
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

            const std::uint8_t magic = *reinterpret_cast<std::uint8_t*>(item - 6);
            if (magic != kAllocMagic)
            {
                debug_println(
                    "Invalid proxy magic {:#x} at ptr={} "
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

            std::uint32_t alloc_size_u32 = 0;
            std::memcpy(&alloc_size_u32, item - 4, 4);
            const std::size_t alloc_size = alloc_size_u32;
            if (alloc_size < header_size ||
                alloc_size > kMaxAllocationBytes + kMaxAlignment)
            {
                debug_println(
                    "Invalid proxy alloc_size {} at ptr={} (header_size={})",
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

            std::uint32_t stored_crc = 0;
            std::memcpy(&stored_crc, item - 16, 4);
            const std::uint32_t expect_crc =
                proxy_header_crc(item, alloc_size_u32, header_size, kAllocMagic);
            if (stored_crc != expect_crc)
            {
                debug_println("Invalid proxy CRC at ptr={}", static_cast<void*>(item));
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
            *reinterpret_cast<std::uint8_t*>(item - 6) = 0;
#if defined(SPALLOCATOR_HAS_ASAN)
            ASAN_POISON_MEMORY_REGION(original_ptr, header_size);
#endif

            debug_println("Deallocating proxy {} bytes at ptr={}",
                          alloc_size, static_cast<void*>(original_ptr));
            large_slab.deallocateItem(original_ptr);
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

        // After all engines exist (including warm-start Blocks), install the
        // Pool Block registry so grow/trim keep block_owner_ consistent.
        install_block_registry();
    }


    inline void Pool::install_block_registry()
    {
        for (auto& s: slabs16)
        {
            s->set_block_registry(&block_registry_);
        }
        for (auto& s: slabs32)
        {
            s->set_block_registry(&block_registry_);
        }
        for (auto& s: slabs64)
        {
            s->set_block_registry(&block_registry_);
        }
    }


    inline void Pool::Registry::on_block_born(std::byte* base, const BlockEntry& entry)
    {
        std::unique_lock lock(pool_.block_map_lock_);
        pool_.block_owner_.insert_or_assign(base, entry);
    }


    inline void Pool::Registry::on_block_died(std::byte* base)
    {
        std::unique_lock lock(pool_.block_map_lock_);
        pool_.block_owner_.erase(base);
    }


    inline std::size_t Pool::block_owner_count() const
    {
        std::shared_lock lock(block_map_lock_);
        return block_owner_.size();
    }


    inline bool Pool::block_owner_contains(std::byte* block_base) const
    {
        std::shared_lock lock(block_map_lock_);
        return block_owner_.find(block_base) != block_owner_.end();
    }


    inline std::optional<BlockEntry> Pool::find_block_owner(std::byte* p) const noexcept
    {
        std::shared_lock lock(block_map_lock_);

        auto it = block_owner_.upper_bound(p);
        if (it == block_owner_.begin())
        {
            return std::nullopt;
        }
        --it;

        std::byte* base = it->first;
        const BlockEntry& entry = it->second;
        if (p < base || entry.elem_size == 0 || entry.block_bytes == 0)
        {
            return std::nullopt;
        }

        const auto offset = static_cast<std::size_t>(p - base);
        if (offset >= entry.block_bytes || (offset % entry.elem_size) != 0)
        {
            return std::nullopt;
        }
        return entry;
    }


    Pool::~Pool()
    {
        // Publish dead before member unique_ptrs tear down slabs so any later
        // control-block observer (handles / deleters) sees !alive.
        // ~Slab unregisters Blocks from block_owner_ before delete[].
        if (control_)
        {
            control_->mark_dead();
            control_->release_block_ref();
            control_ = nullptr;
        }
    }

}; // namespace spallocator


#endif // POOL_HPP_
