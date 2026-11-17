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
    // - Request aligns 4/8 map to native axis 16 (slot over-align is fine)
    // - Axes 16 / 32 / 64 use Slab<ElemSize, Alignment> size ladders
    // - Beyond that axis's max footprint → SlabProxy (always align_val_t{64})
    // - Returned memory is exclusively owned until matching deallocate;
    //   do not free non-pool pointers or mutate the hidden header
    inline constexpr std::size_t kMinAlignment = 4;
    inline constexpr std::size_t kSlabNativeAlignment = 16;
    inline constexpr std::size_t kMaxAlignment = 64;
    inline constexpr std::size_t kSlabIndexNpos = std::numeric_limits<std::size_t>::max();
    // Written at user_ptr − 6; validated (then cleared) on deallocate.
    inline constexpr std::uint8_t kAllocMagic = 0xA7;

    inline constexpr std::size_t kLadder16[] = {
        16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};
    inline constexpr std::size_t kLadder32[] = {
        32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048};
    inline constexpr std::size_t kLadder64[] = {
        64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096};

    inline constexpr std::size_t kLadder16Count =
        sizeof(kLadder16) / sizeof(kLadder16[0]);
    inline constexpr std::size_t kLadder32Count =
        sizeof(kLadder32) / sizeof(kLadder32[0]);
    inline constexpr std::size_t kLadder64Count =
        sizeof(kLadder64) / sizeof(kLadder64[0]);

    // Map request alignment to a native slab axis (4/8 → 16).
    constexpr std::size_t nativeAxis(std::size_t alignment)
    {
        if (alignment <= kSlabNativeAlignment)
        {
            return kSlabNativeAlignment;
        }
        return alignment;
    }

    constexpr std::size_t axisMaxElem(std::size_t axis)
    {
        if (axis <= kSlabNativeAlignment)
        {
            return 1024;
        }
        if (axis == 32)
        {
            return 2048;
        }
        return 4096; // axis 64
    }

    constexpr std::size_t selectClassIndex(std::size_t alloc_size, std::size_t axis)
    {
        const std::size_t* ladder = nullptr;
        std::size_t count = 0;
        if (axis <= kSlabNativeAlignment)
        {
            ladder = kLadder16;
            count = kLadder16Count;
        }
        else if (axis == 32)
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


    struct Allocation
    {
        std::byte* ptr;
        std::size_t size;
    };


    class Pool;
    class PoolControlBlock;


    // Share of a PoolControlBlock that pins handle_refs (keeps empty heap pools
    // alive). From Pool::handle() or PoolHandle::create(). Declared before Pool
    // because Pool::handle() returns PoolHandle by value.
    class PoolHandle
    {
    public: // methods
        PoolHandle() noexcept = default;
        PoolHandle(const PoolHandle& other) noexcept;
        PoolHandle(PoolHandle&& other) noexcept;
        ~PoolHandle();

        PoolHandle& operator=(const PoolHandle& other) noexcept;
        PoolHandle& operator=(PoolHandle&& other) noexcept;

        // Heap domain: new Pool marked heap-owned; retired when handle_refs and
        // live_allocs are both zero.
        static PoolHandle create();

        // nullptr if empty handle or pool already marked dead.
        Pool* get() const noexcept;
        explicit operator bool() const noexcept;

        PoolControlBlock* control() const noexcept { return block_; }

    private: // methods
        friend class Pool;
        explicit PoolHandle(PoolControlBlock* block) noexcept;

        void retain() noexcept;
        void release() noexcept;

    private: // data members
        PoolControlBlock* block_{nullptr};
    };


    class Pool
    {
    public: // methods
        std::byte* allocate(std::size_t size, std::size_t alignment = 8);

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

        // Handle sharing this pool's control block (bumps handle_refs + block_refs).
        PoolHandle handle();

        PoolControlBlock* control() const noexcept { return control_; }

        Pool();
        ~Pool();

        // Default: axis 16. Pass alignment (or use selectClassIndex) for 32/64.
        constexpr std::size_t selectSlab(std::size_t size) const { return selectClassIndex(size, kSlabNativeAlignment); }
        constexpr std::size_t selectSlab(std::size_t size, std::size_t alignment) const { return selectClassIndex(size, nativeAxis(alignment)); }

    private: // methods
        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;
        Pool(Pool&&) = delete;
        Pool& operator=(Pool&&) = delete;

        AbstractSlab* resolveSlab(std::size_t alloc_size, std::size_t axis);
        AbstractSlab* getOrCreateLazy(std::size_t axis, std::size_t index);

        static std::unique_ptr<AbstractSlab> makeLazySlab(std::size_t axis,
                                                          std::size_t index);

    private: // data members
        PoolControlBlock* control_{nullptr};

        std::array<std::unique_ptr<AbstractSlab>, kLadder16Count> slabs16{};
        std::array<std::unique_ptr<AbstractSlab>, kLadder32Count> slabs32{};
        std::array<std::unique_ptr<AbstractSlab>, kLadder64Count> slabs64{};
        SlabProxy large_slab;

        SpinLock lock;
        SpinLock slab_init_lock;
    };


    // Shared side state for Pool lifetime vs handles / future deleters.
    // Heap-allocated; outlives Pool when block_refs remain (arena path).
    // After Pool: only held by pointer inside Pool, so a prior forward decl suffices.
    class PoolControlBlock
    {
    public: // methods
        static PoolControlBlock* create(Pool* pool);

        void add_block_ref() noexcept;
        void release_block_ref() noexcept;

        void add_handle_ref() noexcept;
        // Drops handle_refs; may delete heap-owned Pool if also idle.
        void release_handle_ref() noexcept;

        void note_alloc() noexcept;
        // Drops live_allocs; returns heap Pool to delete if handles also idle
        // (caller must delete — may be `this` of an in-progress free).
        Pool* note_free() noexcept;

        bool is_alive() const noexcept;
        bool is_heap_owned() const noexcept;
        void set_heap_owned() noexcept;
        Pool* pool() const noexcept;
        std::uint64_t live_allocation_count() const noexcept;
        std::uint64_t handle_ref_count() const noexcept;
        std::uint64_t block_ref_count() const noexcept;

        // ~Pool / successful retire: publish dead, drop Pool*.
        void mark_dead() noexcept;

    private: // methods
        PoolControlBlock() = default;
        ~PoolControlBlock() = default;

        PoolControlBlock(const PoolControlBlock&) = delete;
        PoolControlBlock& operator=(const PoolControlBlock&) = delete;
        PoolControlBlock(PoolControlBlock&&) = delete;
        PoolControlBlock& operator=(PoolControlBlock&&) = delete;

        // If heap-owned, alive, and both counts zero: claim death and return Pool*.
        Pool* try_retire() noexcept;

    private: // data members
        std::atomic<std::uint64_t> block_refs_{0};
        std::atomic<std::uint64_t> handle_refs_{0};
        std::atomic<std::uint64_t> live_allocs_{0};
        std::atomic<bool> alive_{false};
        bool heap_owned_{false};
        Pool* pool_{nullptr};
    };


    // ---------------------------------------------------------------------------
    // PoolControlBlock
    // ---------------------------------------------------------------------------

    inline PoolControlBlock* PoolControlBlock::create(Pool* pool)
    {
        auto* block = new PoolControlBlock();
        block->pool_ = pool;
        block->alive_.store(true, std::memory_order_release);
        // One block_ref held by the Pool object for the lifetime of Pool.
        block->block_refs_.store(1, std::memory_order_relaxed);
        return block;
    }


    inline void PoolControlBlock::add_block_ref() noexcept
    {
        block_refs_.fetch_add(1, std::memory_order_relaxed);
    }


    inline void PoolControlBlock::release_block_ref() noexcept
    {
        if (block_refs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            delete this;
        }
    }


    inline void PoolControlBlock::add_handle_ref() noexcept
    {
        handle_refs_.fetch_add(1, std::memory_order_relaxed);
    }


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


    inline void PoolControlBlock::note_alloc() noexcept
    {
        live_allocs_.fetch_add(1, std::memory_order_relaxed);
    }


    inline Pool* PoolControlBlock::note_free() noexcept
    {
        if (live_allocs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            return try_retire();
        }
        return nullptr;
    }


    inline bool PoolControlBlock::is_alive() const noexcept
    {
        return alive_.load(std::memory_order_acquire);
    }


    inline bool PoolControlBlock::is_heap_owned() const noexcept
    {
        return heap_owned_;
    }


    inline void PoolControlBlock::set_heap_owned() noexcept
    {
        heap_owned_ = true;
    }


    inline Pool* PoolControlBlock::pool() const noexcept
    {
        return pool_;
    }


    inline std::uint64_t PoolControlBlock::live_allocation_count() const noexcept
    {
        return live_allocs_.load(std::memory_order_acquire);
    }


    inline std::uint64_t PoolControlBlock::handle_ref_count() const noexcept
    {
        return handle_refs_.load(std::memory_order_acquire);
    }


    inline std::uint64_t PoolControlBlock::block_ref_count() const noexcept
    {
        return block_refs_.load(std::memory_order_acquire);
    }


    inline void PoolControlBlock::mark_dead() noexcept
    {
        alive_.store(false, std::memory_order_release);
        pool_ = nullptr;
    }


    inline Pool* PoolControlBlock::try_retire() noexcept
    {
        if (!heap_owned_)
        {
            return nullptr;
        }
        if (handle_refs_.load(std::memory_order_acquire) != 0)
        {
            return nullptr;
        }
        if (live_allocs_.load(std::memory_order_acquire) != 0)
        {
            return nullptr;
        }

        bool expected = true;
        if (!alive_.compare_exchange_strong(
                expected, false,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return nullptr;
        }

        // Lost a race with a new handle or alloc — undo and abort retire.
        if (handle_refs_.load(std::memory_order_acquire) != 0 ||
            live_allocs_.load(std::memory_order_acquire) != 0)
        {
            alive_.store(true, std::memory_order_release);
            return nullptr;
        }

        Pool* doomed = pool_;
        pool_ = nullptr;
        return doomed;
    }


    // ---------------------------------------------------------------------------
    // PoolHandle
    // ---------------------------------------------------------------------------

    inline PoolHandle PoolHandle::create()
    {
        Pool* pool = new Pool();
        pool->control()->set_heap_owned();
        return PoolHandle(pool->control());
    }


    inline PoolHandle::PoolHandle(PoolControlBlock* block) noexcept
        : block_(block)
    {
        retain();
    }


    inline PoolHandle::PoolHandle(const PoolHandle& other) noexcept
        : block_(other.block_)
    {
        retain();
    }


    inline PoolHandle::PoolHandle(PoolHandle&& other) noexcept
        : block_(other.block_)
    {
        other.block_ = nullptr;
    }


    inline PoolHandle::~PoolHandle()
    {
        release();
    }


    inline PoolHandle& PoolHandle::operator=(const PoolHandle& other) noexcept
    {
        if (this != &other)
        {
            release();
            block_ = other.block_;
            retain();
        }
        return *this;
    }


    inline PoolHandle& PoolHandle::operator=(PoolHandle&& other) noexcept
    {
        if (this != &other)
        {
            release();
            block_ = other.block_;
            other.block_ = nullptr;
        }
        return *this;
    }


    inline Pool* PoolHandle::get() const noexcept
    {
        if (!block_ || !block_->is_alive())
        {
            return nullptr;
        }
        return block_->pool();
    }


    inline PoolHandle::operator bool() const noexcept
    {
        return get() != nullptr;
    }


    inline void PoolHandle::retain() noexcept
    {
        if (block_)
        {
            block_->add_block_ref();
            block_->add_handle_ref();
        }
    }


    inline void PoolHandle::release() noexcept
    {
        if (block_)
        {
            block_->release_handle_ref();
            block_->release_block_ref();
            block_ = nullptr;
        }
    }


    // ---------------------------------------------------------------------------
    // Pool
    // ---------------------------------------------------------------------------

    inline std::unique_ptr<AbstractSlab> Pool::makeLazySlab(std::size_t axis,
                                                            std::size_t index)
    {
        if (axis == 32)
        {
            switch (index)
            {
                case 0:  return std::make_unique<Slab<32, 32>>();
                case 1:  return std::make_unique<Slab<64, 32>>();
                case 2:  return std::make_unique<Slab<96, 32>>();
                case 3:  return std::make_unique<Slab<128, 32>>();
                case 4:  return std::make_unique<Slab<192, 32>>();
                case 5:  return std::make_unique<Slab<256, 32>>();
                case 6:  return std::make_unique<Slab<384, 32>>();
                case 7:  return std::make_unique<Slab<512, 32>>();
                case 8:  return std::make_unique<Slab<768, 32>>();
                case 9:  return std::make_unique<Slab<1024, 32>>();
                case 10: return std::make_unique<Slab<1536, 32>>();
                case 11: return std::make_unique<Slab<2048, 32>>();
                default: break;
            }
        }
        else if (axis == 64)
        {
            switch (index)
            {
                case 0:  return std::make_unique<Slab<64, 64>>();
                case 1:  return std::make_unique<Slab<128, 64>>();
                case 2:  return std::make_unique<Slab<192, 64>>();
                case 3:  return std::make_unique<Slab<256, 64>>();
                case 4:  return std::make_unique<Slab<384, 64>>();
                case 5:  return std::make_unique<Slab<512, 64>>();
                case 6:  return std::make_unique<Slab<768, 64>>();
                case 7:  return std::make_unique<Slab<1024, 64>>();
                case 8:  return std::make_unique<Slab<1536, 64>>();
                case 9:  return std::make_unique<Slab<2048, 64>>();
                case 10: return std::make_unique<Slab<3072, 64>>();
                case 11: return std::make_unique<Slab<4096, 64>>();
                default: break;
            }
        }
        throw std::invalid_argument(
            std::format("Invalid lazy slab axis={} index={}", axis, index));
    }


    inline AbstractSlab* Pool::getOrCreateLazy(std::size_t axis, std::size_t index)
    {
        auto& registry = (axis == 32) ? slabs32 : slabs64;
        {
            std::scoped_lock<SpinLock> guard(lock);
            if (registry[index])
            {
                return registry[index].get();
            }
        }

        std::scoped_lock<SpinLock> init_guard(slab_init_lock);
        {
            std::scoped_lock<SpinLock> guard(lock);
            if (registry[index])
            {
                return registry[index].get();
            }
        }

        // Heavy construction while holding only slab_init_lock (not Pool::lock).
        auto created = makeLazySlab(axis, index);

        std::scoped_lock<SpinLock> guard(lock);
        if (!registry[index])
        {
            registry[index] = std::move(created);
        }
        return registry[index].get();
    }


    inline AbstractSlab* Pool::resolveSlab(std::size_t alloc_size, std::size_t axis)
    {
        const std::size_t index = selectClassIndex(alloc_size, axis);
        if (index == kSlabIndexNpos)
        {
            return &large_slab;
        }

        if (axis <= kSlabNativeAlignment)
        {
            std::scoped_lock<SpinLock> guard(lock);
            return slabs16[index].get();
        }

        return getOrCreateLazy(axis, index);
    }


    std::byte* Pool::allocate(std::size_t item_size, std::size_t alignment /* = 8 */)
    {
        Allocation alloc;
        alloc.size = item_size;

        if (item_size > 1_GB)
        {
            // excessively large allocations don't belong in the pool
            // slabs; use other allocation methods instead
            throw std::out_of_range("Allocation size exceeds maximum limit for pool allocator");
        }

        // Range and power-of-two checks throw in all builds (including release).
        // Non-power-of-two values in [4, 64] (e.g. 12) would break header/axis math.
        if (alignment < kMinAlignment || alignment > kMaxAlignment ||
            (alignment & (alignment - 1)) != 0)
        {
            throw std::invalid_argument("Unsupported alignment requested");
        }

        // Store footprint metadata in a header immediately before the user
        // pointer.
        // header_size = max(8, alignment). For align 4/8 this stays 8 (axis 16);
        // for 16/32/64 it equals the request align so free can recover the axis.
        // Layout (from user pointer backward): uint32 alloc_size at −4,
        // uint8 header_size at −5, uint8 magic at −6. Deallocate validates
        // magic/size then recovers the slot base and re-selects the slab via
        // (alloc_size, axis-from-header_size).
        std::size_t header_size = 8 < alignment ? alignment : 8;
        std::size_t alloc_size = item_size + header_size;
        alloc.size = alloc_size;

        const std::size_t axis = nativeAxis(alignment);
        AbstractSlab* slab = resolveSlab(alloc_size, axis);
        debug_println("Allocated {} bytes, axis={}, slab={}",
                      alloc_size, axis,
                      (slab == &large_slab) ? "large_slab" : "size_class");

        alloc.ptr = slab->allocateItem(alloc_size);

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
                return false;
            }

            uint32_t alloc_size_u32 = *reinterpret_cast<uint32_t*>(item - 4);
            std::size_t alloc_size = alloc_size_u32;
            if (alloc_size < header_size || alloc_size > 1_GB + kMaxAlignment)
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
                return false;
            }

            std::byte* original_ptr = item - header_size;

            // header_size 8 or 16 → axis 16; 32 → 32; 64 → 64.
            const std::size_t axis = (header_size <= kSlabNativeAlignment)
                ? kSlabNativeAlignment
                : static_cast<std::size_t>(header_size);

            // Invalidate header before returning the slot (detects Pool double-free).
            *reinterpret_cast<uint8_t*>(item - 6) = 0;

#if defined(SPALLOCATOR_HAS_ASAN)
            ASAN_POISON_MEMORY_REGION(original_ptr, header_size);
#endif

            AbstractSlab* slab = resolveSlab(alloc_size, axis);
            debug_println("Deallocating {} bytes at ptr={}, axis={}, slab={}",
                          alloc_size, static_cast<void*>(original_ptr), axis,
                          (slab == &large_slab) ? "large_slab" : "size_class");
            slab->deallocateItem(original_ptr);
            // May return `this` for heap-owned idle pools — delete only after
            // this method is done touching members.
            Pool* doomed = control_->note_free();
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
                reclaimed += slab->reclaimEmptySlabs(keep_min);
            }
        }

        // Lazy axes: hold init lock so unique_ptr slots are stable while we trim.
        {
            std::scoped_lock<SpinLock> init_guard(slab_init_lock);
            for (auto& slab : slabs32)
            {
                if (slab)
                {
                    reclaimed += slab->reclaimEmptySlabs(keep_min);
                }
            }
            for (auto& slab : slabs64)
            {
                if (slab)
                {
                    reclaimed += slab->reclaimEmptySlabs(keep_min);
                }
            }
        }

        debug_println("Pool::trim(keep_min={}): reclaimed {} slabs", keep_min, reclaimed);
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

        std::scoped_lock<SpinLock> init_guard(slab_init_lock);
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


    inline PoolHandle Pool::handle()
    {
        return PoolHandle(control_);
    }


    Pool::Pool()
        : control_(PoolControlBlock::create(this))
    {
        // Eager native-16 ladder only; 32/64 axes are created on first use.
        slabs16[0]  = std::make_unique<Slab<16, 16>>();
        slabs16[1]  = std::make_unique<Slab<32, 16>>();
        slabs16[2]  = std::make_unique<Slab<48, 16>>();
        slabs16[3]  = std::make_unique<Slab<64, 16>>();
        slabs16[4]  = std::make_unique<Slab<96, 16>>();
        slabs16[5]  = std::make_unique<Slab<128, 16>>();
        slabs16[6]  = std::make_unique<Slab<192, 16>>();
        slabs16[7]  = std::make_unique<Slab<256, 16>>();
        slabs16[8]  = std::make_unique<Slab<384, 16>>();
        slabs16[9]  = std::make_unique<Slab<512, 16>>();
        slabs16[10] = std::make_unique<Slab<768, 16>>();
        slabs16[11] = std::make_unique<Slab<1024, 16>>();
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
