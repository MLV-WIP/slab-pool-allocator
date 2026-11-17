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

#ifndef POOL_LIFETIME_HPP_
#define POOL_LIFETIME_HPP_

#include <atomic>
#include <cstdint>

// Pool lifetime meta-management: control block, block tokens, and handles.
// Separated from pool.hpp so the allocator facade stays focused on routing.
//
// Methods that require a complete Pool (PoolHandle::create, and deleting a
// heap-owned Pool on last handle drop) are defined in pool.hpp after class Pool.


namespace spallocator
{

    class Pool;


    // Shared side state for Pool lifetime vs handles / deleters.
    // Heap-allocated; outlives Pool when block_refs remain (arena path).
    class PoolControlBlock
    {
    public: // methods
        static PoolControlBlock* create(Pool* pool);

        void add_block_ref() noexcept;
        void release_block_ref() noexcept;

        void add_handle_ref() noexcept;
        // Drops handle_refs; may delete heap-owned Pool if also idle.
        // Defined in pool.hpp (needs complete Pool).
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


    // Deleter/allocator pin: block_refs only (does not keep empty heap pools
    // alive). Outlives Pool on the arena path so !alive can be observed.
    class PoolBlockToken
    {
    public: // methods
        PoolBlockToken() noexcept = default;
        PoolBlockToken(const PoolBlockToken& other) noexcept;
        PoolBlockToken(PoolBlockToken&& other) noexcept;
        ~PoolBlockToken();

        PoolBlockToken& operator=(const PoolBlockToken& other) noexcept;
        PoolBlockToken& operator=(PoolBlockToken&& other) noexcept;

        // nullptr if empty token or pool already marked dead.
        Pool* get() const noexcept;
        bool alive() const noexcept;
        explicit operator bool() const noexcept;

        PoolControlBlock* control() const noexcept { return block_; }

    private: // methods
        friend class Pool;
        friend class PoolHandle;
        explicit PoolBlockToken(PoolControlBlock* block) noexcept;

        void retain() noexcept;
        void release() noexcept;

    private: // data members
        PoolControlBlock* block_{nullptr};
    };


    // Share of a PoolControlBlock that pins handle_refs (keeps empty heap pools
    // alive). From Pool::handle() or PoolHandle::create().
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
        // live_allocs are both zero. Defined in pool.hpp (needs complete Pool).
        static PoolHandle create();

        // nullptr if empty handle or pool already marked dead.
        Pool* get() const noexcept;
        explicit operator bool() const noexcept;

        PoolControlBlock* control() const noexcept { return block_; }

        // Deleter/allocator pin (block_refs only; does not bump handle_refs).
        PoolBlockToken block_token() const noexcept;

    private: // methods
        friend class Pool;
        explicit PoolHandle(PoolControlBlock* block) noexcept;

        void retain() noexcept;
        void release() noexcept;

    private: // data members
        PoolControlBlock* block_{nullptr};
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
    // PoolBlockToken
    // ---------------------------------------------------------------------------

    inline PoolBlockToken::PoolBlockToken(PoolControlBlock* block) noexcept
        : block_(block)
    {
        retain();
    }


    inline PoolBlockToken::PoolBlockToken(const PoolBlockToken& other) noexcept
        : block_(other.block_)
    {
        retain();
    }


    inline PoolBlockToken::PoolBlockToken(PoolBlockToken&& other) noexcept
        : block_(other.block_)
    {
        other.block_ = nullptr;
    }


    inline PoolBlockToken::~PoolBlockToken()
    {
        release();
    }


    inline PoolBlockToken& PoolBlockToken::operator=(const PoolBlockToken& other) noexcept
    {
        if (this != &other)
        {
            release();
            block_ = other.block_;
            retain();
        }
        return *this;
    }


    inline PoolBlockToken& PoolBlockToken::operator=(PoolBlockToken&& other) noexcept
    {
        if (this != &other)
        {
            release();
            block_ = other.block_;
            other.block_ = nullptr;
        }
        return *this;
    }


    inline Pool* PoolBlockToken::get() const noexcept
    {
        if (!block_ || !block_->is_alive())
        {
            return nullptr;
        }
        return block_->pool();
    }


    inline bool PoolBlockToken::alive() const noexcept
    {
        return block_ && block_->is_alive();
    }


    inline PoolBlockToken::operator bool() const noexcept
    {
        return alive();
    }


    inline void PoolBlockToken::retain() noexcept
    {
        if (block_)
        {
            block_->add_block_ref();
        }
    }


    inline void PoolBlockToken::release() noexcept
    {
        if (block_)
        {
            block_->release_block_ref();
            block_ = nullptr;
        }
    }


    // ---------------------------------------------------------------------------
    // PoolHandle
    // ---------------------------------------------------------------------------

    inline PoolBlockToken PoolHandle::block_token() const noexcept
    {
        return PoolBlockToken(block_);
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

} // namespace spallocator


#endif // POOL_LIFETIME_HPP_
