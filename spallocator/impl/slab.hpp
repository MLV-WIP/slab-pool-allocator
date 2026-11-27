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

#ifndef SPALLOCATOR_IMPL_SLAB_HPP_
#define SPALLOCATOR_IMPL_SLAB_HPP_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "impl/block.hpp"
#include "impl/helper.hpp"
#include "impl/islab.hpp"
#include "impl/slab_mutex.hpp"


namespace spallocator
{

    // Runtime-sized bit vector over uint64_t words. Used for per-Slab Block
    // availability and empty-live tracking so empty size classes do not embed
    // bitset<max_blocks> (~128 KiB for 4 KiB Blocks). Word ops are load/mask/
    // store; find_next_set scans 64 blocks per word with std::countr_zero.
    class DynBitset
    {
    public:
        static constexpr std::size_t kWordBits = 64;

        bool test(std::size_t bit) const noexcept;
        void set(std::size_t bit) noexcept;
        void reset(std::size_t bit) noexcept;

        // Reserve word storage for at least `num_bits` bits without changing size().
        void reserve(std::size_t num_bits);

        // Append one bit valued 1 or 0 (grows word storage geometrically).
        void push_set();
        void push_clear();

        // Next set bit at or after `start`. Returns size() if none.
        std::size_t find_next_set(std::size_t start) const noexcept;

        std::size_t size() const noexcept { return size_; }
        // Heap buffer footprint of the word array (capacity, not size).
        std::size_t bytes() const noexcept { return words_.capacity() * sizeof(std::uint64_t); }

    private:
        void ensure_capacity_for_push();

        std::vector<std::uint64_t> words_;
        std::size_t size_{0};
    };


    // Compound key: every slot is Alignment-aligned because each Block uses
    // align_val_t{Alignment} and ElemSize is a multiple of Alignment.
    template<std::size_t ElemSize,
             std::size_t Alignment = 16,
             mlvlib::BasicLockable Lock = DefaultSlabLock>
    class Slab: public ISlab
    {
    public: // types
        using lock_type = Lock;
        using lock_traits = mlvlib::lock_traits<Lock>;
        using block_type = Block<ElemSize, Alignment>;

    public: // methods
        AllocateItemResult allocateItem(std::size_t size) override;
        void deallocateItem(std::byte* item,
                            std::uint32_t block_index = kInvalidBlockIndex,
                            std::uint16_t slot_index = 0,
                            std::uint16_t block_generation = 0) override;

        constexpr std::size_t getElemSize() const noexcept { return ElemSize; }
        constexpr std::size_t getAlignment() const noexcept { return Alignment; }
        constexpr std::size_t getAllocSize() const noexcept { return block_bytes; }
        std::size_t getAllocatedMemory() const override { return live_block_count_ * block_bytes; }
        std::size_t getAvailabilityMapBytes() const noexcept { return block_available_map_.bytes(); }
        std::size_t getBlockCount() const noexcept { return live_block_count_; }
        std::size_t getBlockCapacity() const noexcept { return blocks_.size(); }

        bool isThreadSafe() const noexcept override { return lock_traits::is_thread_safe; }
        std::string_view lockPolicyName() const noexcept override { return lock_traits::name; }

        // Release backing buffers of fully empty Blocks back to the OS while
        // retaining at least keep_min live Blocks (clamped to >= 1). Block
        // indices remain stable; trimmed shells are reused in O(1) on growth.
        std::size_t reclaimEmptyBlocks(std::size_t keep_min = 1) override;

        // `slot_base` must be an exact Slot start (not a Pool user pointer).
        // O(1) when 1 live Block; O(log m) otherwise via base_address_map_.
        std::optional<std::size_t> findBlockForSlot(std::byte* slot_base) const;

        // preallocate_first_block: map one data block at construction (warm
        // start). false leaves the engine empty until the first allocateItem
        // (Pool uses this for large / high-align classes — see pool.hpp).
        explicit Slab(bool preallocate_first_block = false);
        ~Slab() override;

    private: // types
        struct RawBufferGuard
        {
            std::byte* ptr{nullptr};

            RawBufferGuard() noexcept = default;
            explicit RawBufferGuard(std::byte* p) noexcept : ptr(p) {}
            ~RawBufferGuard() noexcept
            {
                if (ptr != nullptr)
                {
                    block_type::freeRawBuffer(ptr);
                }
            }

            RawBufferGuard(const RawBufferGuard&) = delete;
            RawBufferGuard& operator=(const RawBufferGuard&) = delete;

            std::byte* release() noexcept
            {
                std::byte* p = ptr;
                ptr = nullptr;
                return p;
            }
        };

    private: // methods
        Slab(const Slab&) = delete;
        Slab& operator=(const Slab&) = delete;
        Slab(Slab&&) = delete;
        Slab& operator=(Slab&&) = delete;

        // Try to allocate one slot from an existing live Block. Caller holds slab_lock.
        std::optional<AllocateItemResult> tryAllocateFromExistingLocked();

        // Cold growth path: allocates backing buffer outside slab_lock, publishes
        // (or reuses a concurrently grown Block), and allocates one slot.
        AllocateItemResult allocateSlow();

        // Publish a pre-allocated buffer into a trimmed Block (O(1)) or append
        // a new Block. Caller must hold slab_lock.
        std::size_t publishBlockBuffer(RawBufferGuard& raw_buf);

    private: // data members
        static constexpr std::size_t block_bytes{block_type::block_bytes};
        static constexpr std::size_t slots_per_block{block_type::slots_per_block};
        static constexpr std::size_t max_blocks{kMaxClassBytes / block_bytes};
        static_assert(max_blocks >= 1,
            "kMaxClassBytes must allow at least one Block of this ElemSize");
        static_assert(max_blocks <= std::numeric_limits<std::uint32_t>::max(),
            "max_blocks must fit in uint32_t block_index routing hint");

        // Index-stable collection of Blocks. Trimmed Blocks keep buffer_ == nullptr
        // and are recycled via trimmed_blocks_ without shifting indices.
        std::vector<block_type> blocks_;
        std::uint16_t next_block_generation_{1};
        std::size_t live_block_count_{0};

        // Bit i == 1 iff blocks_[i].hasFree() (live buffer and >= 1 free slot).
        DynBitset block_available_map_;

        // LIFO stack of trimmed Block indices (buffer_ == nullptr) for O(1) refill.
        std::vector<std::uint32_t> trimmed_blocks_;

        // Cold O(log m) address-range index used ONLY for hint-less frees
        // (align-8 Pool frees and direct unhinted Slab::deallocateItem) and
        // findBlockForSlot(). Never touched on the O(1) hinted free path.
        std::map<std::byte*, std::size_t> base_address_map_;

        // Next-fit cursors for allocateItem (indices into blocks_ / freemap).
        std::size_t alloc_cursor_block_{0};
        std::size_t alloc_cursor_slot_{0};

        // Placed last so [[no_unique_address]] collapses empty Lock types (NullLock
        // in release) to 0 bytes without exposing tail padding to later members.
        MLVLIB_NO_UNIQUE_ADDRESS Lock slab_lock;
    };


    // ---------------------------------------------------------------------------
    // DynBitset
    // ---------------------------------------------------------------------------

    inline bool DynBitset::test(std::size_t bit) const noexcept
    {
        return (words_[bit >> 6] >> (bit & 63)) & 1ull;
    }

    inline void DynBitset::set(std::size_t bit) noexcept
    {
        words_[bit >> 6] |= (std::uint64_t{1} << (bit & 63));
    }

    inline void DynBitset::reset(std::size_t bit) noexcept
    {
        words_[bit >> 6] &= ~(std::uint64_t{1} << (bit & 63));
    }

    inline void DynBitset::reserve(std::size_t num_bits)
    {
        if (num_bits == 0)
        {
            return;
        }
        const std::size_t need = (num_bits + kWordBits - 1) / kWordBits;
        if (need > words_.size())
        {
            const std::size_t grown = words_.empty() ? need : words_.size() * 2;
            words_.resize(grown > need ? grown : need, 0);
        }
    }

    inline void DynBitset::ensure_capacity_for_push()
    {
        reserve(size_ + 1);
    }

    inline void DynBitset::push_set()
    {
        ensure_capacity_for_push();
        const std::size_t bit = size_++;
        set(bit);
    }

    inline void DynBitset::push_clear()
    {
        ensure_capacity_for_push();
        const std::size_t bit = size_++;
        reset(bit);
    }

    inline std::size_t DynBitset::find_next_set(std::size_t start) const noexcept
    {
        if (start >= size_)
        {
            return size_;
        }

        const std::size_t num_words = (size_ + kWordBits - 1) / kWordBits;
        std::size_t word = start / kWordBits;
        std::size_t bit0 = start % kWordBits;
        for (; word < num_words; ++word, bit0 = 0)
        {
            const std::size_t base = word * kWordBits;
            const std::size_t bits_in_word =
                (base + kWordBits <= size_) ? kWordBits : (size_ - base);
            const std::uint64_t width_mask =
                (bits_in_word == kWordBits)
                    ? ~std::uint64_t{0}
                    : ((std::uint64_t{1} << bits_in_word) - std::uint64_t{1});
            const std::uint64_t skip_mask =
                (bit0 == 0) ? ~std::uint64_t{0} : (~std::uint64_t{0} << bit0);
            const std::uint64_t active = words_[word] & width_mask & skip_mask;
            if (active != 0)
            {
                return base + static_cast<std::size_t>(std::countr_zero(active));
            }
        }
        return size_;
    }


    // ---------------------------------------------------------------------------
    // Slab
    // ---------------------------------------------------------------------------

    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    std::optional<AllocateItemResult>
    Slab<ElemSize, Alignment, Lock>::tryAllocateFromExistingLocked()
    {
        mlvlib::assert_lock_held(slab_lock);

        const std::size_t nblocks = blocks_.size();
        if (nblocks == 0)
        {
            return std::nullopt;
        }

        if (alloc_cursor_block_ >= nblocks)
        {
            alloc_cursor_block_ = 0;
            alloc_cursor_slot_ = 0;
        }

        std::size_t block_index = nblocks;
        std::size_t slot_start = 0;

        // O(1) cursor fast path (common LIFO / sequential steady state).
        if (block_available_map_.test(alloc_cursor_block_))
        {
            block_index = alloc_cursor_block_;
            slot_start = alloc_cursor_slot_;
        }
        else
        {
            // Word-level scan (64 blocks per uint64_t) with wrap.
            block_index = block_available_map_.find_next_set(alloc_cursor_block_);
            if (block_index >= nblocks && alloc_cursor_block_ > 0)
            {
                block_index = block_available_map_.find_next_set(0);
            }
        }

        if (block_index >= nblocks)
        {
            return std::nullopt;
        }

        auto& blk = blocks_[block_index];
        const std::size_t item_index = blk.allocateSlot(slot_start);
        if (item_index >= slots_per_block)
        {
            return std::nullopt;
        }

        // Only update availability bitmap on non-full -> full transition.
        if (blk.isFull())
        {
            block_available_map_.reset(block_index);
        }

        // Advance next-fit cursor.
        alloc_cursor_block_ = block_index;
        alloc_cursor_slot_ = item_index + 1;
        if (alloc_cursor_slot_ >= slots_per_block)
        {
            alloc_cursor_slot_ = 0;
            alloc_cursor_block_ = block_index + 1;
            if (alloc_cursor_block_ >= nblocks)
            {
                alloc_cursor_block_ = 0;
            }
        }

        debug_println("Item allocated ({}/{}), block_freemaps<{},{}>: {}",
                      block_index, item_index, ElemSize, Alignment,
                      printHex(blk.freemap()));

        AllocateItemResult out;
        out.ptr = blk.slotAddress(item_index);
        out.block_index = static_cast<std::uint32_t>(block_index);
        out.slot_index = static_cast<std::uint16_t>(item_index);
        out.block_generation = blk.generation();
        return out;
    }


    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    AllocateItemResult Slab<ElemSize, Alignment, Lock>::allocateSlow()
    {
        // Declared BEFORE lock so any redundant staged buffer is freed AFTER
        // slab_lock is released (both on normal return and on exception).
        RawBufferGuard staged_buf;

        for (;;)
        {
            if (staged_buf.ptr == nullptr)
            {
                staged_buf.ptr = block_type::allocateRawBuffer();
            }

            typename lock_traits::guard_type guard(slab_lock);

            // Another thread may have grown or freed a slot while we allocated
            // outside the lock. If so, consume it and let staged_buf free after
            // guard releases slab_lock.
            if (auto existing = tryAllocateFromExistingLocked(); existing.has_value())
            {
                return *existing;
            }

            if (live_block_count_ >= max_blocks)
            {
                throw std::out_of_range(
                    std::format("Cannot allocate more than {} blocks of size {} bytes",
                                max_blocks, block_bytes));
            }

            const std::size_t published_index = publishBlockBuffer(staged_buf);
            alloc_cursor_block_ = published_index;
            alloc_cursor_slot_ = 0;
            debug_println("New block<{},{}> published at index {}, live blocks: {}",
                          ElemSize, Alignment, published_index, live_block_count_);

            if (auto fresh = tryAllocateFromExistingLocked(); fresh.has_value())
            {
                return *fresh;
            }
        }
    }


    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    AllocateItemResult Slab<ElemSize, Alignment, Lock>::allocateItem(std::size_t size)
    {
        // Must remain active in release builds: an oversized request would
        // otherwise be placed in a too-small slot (silent overrun).
        if (size > ElemSize)
        {
            throw std::invalid_argument(
                std::format("Requested size {} exceeds slab element size {}", size, ElemSize));
        }

        {
            typename lock_traits::guard_type guard(slab_lock);

            if (auto existing = tryAllocateFromExistingLocked(); existing.has_value())
            {
                return *existing;
            }

            if (live_block_count_ >= max_blocks)
            {
                throw std::out_of_range(
                    std::format("Cannot allocate more than {} blocks of size {} bytes",
                                max_blocks, block_bytes));
            }
        }

        return allocateSlow();
    }


    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    std::optional<std::size_t> Slab<ElemSize, Alignment, Lock>::findBlockForSlot(std::byte* slot_base) const
    {
        if (base_address_map_.empty())
        {
            return std::nullopt;
        }

        // Single-live-Block fast path: O(1) without tree traversal.
        if (base_address_map_.size() == 1)
        {
            const auto it = base_address_map_.begin();
            std::byte* const block_start = it->first;
            std::byte* const block_end = block_start + block_bytes;
            if (slot_base >= block_start && slot_base < block_end)
            {
                return it->second;
            }
            debug_println(
                "Slot base {} is not within sole block range {} - {}; treating as unknown pointer",
                static_cast<void*>(slot_base),
                static_cast<void*>(block_start),
                static_cast<void*>(block_end));
            return std::nullopt;
        }

        auto it = base_address_map_.upper_bound(slot_base);
        if (it != base_address_map_.begin())
        {
            --it;
            std::byte* const block_start = it->first;
            std::byte* const block_end = block_start + block_bytes;

            // Predecessor start is only a candidate. Invalid pointers (or
            // addresses in gaps between Blocks) can upper_bound into this path
            // without falling inside [block_start, block_end).
            if (slot_base >= block_start && slot_base < block_end)
            {
                return it->second;
            }

            debug_println(
                "Slot base {} is not within candidate block range {} - {}; treating as unknown pointer",
                static_cast<void*>(slot_base),
                static_cast<void*>(block_start),
                static_cast<void*>(block_end));
        }
        return std::nullopt;
    }


    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    void Slab<ElemSize, Alignment, Lock>::deallocateItem(std::byte* item,
                                                         std::uint32_t block_index,
                                                         std::uint16_t slot_index,
                                                         std::uint16_t block_generation)
    {
        if (!item)
        {
            return;
        }

        typename lock_traits::guard_type guard(slab_lock);

        const bool has_index = (block_index != kInvalidBlockIndex);
        const bool has_gen = (block_generation != 0);

        if (has_index && has_gen)
        {
            // O(1) indexed fast path: zero std::map lookup, zero integer division.
            if (block_index >= blocks_.size())
            {
                throw std::invalid_argument(
                    "Invalid item pointer; block_index out of range");
            }

            auto& blk = blocks_[block_index];
            if (!blk.isLive() || blk.generation() != block_generation)
            {
                debug_println(
                    "Invalid item pointer; Block not live or generation mismatch: "
                    "block_index={}, live={}, blk_gen={}, hint_gen={}",
                    block_index, blk.isLive(), blk.generation(), block_generation);
                throw std::invalid_argument(
                    "Invalid item pointer; block generation mismatch");
            }

            if (slot_index >= slots_per_block || item != blk.slotAddress(slot_index))
            {
                debug_println(
                    "Invalid item pointer; slot_index address mismatch: "
                    "slot_index={}, blk_addr={}, blk_slot_addr={}",
                    slot_index, static_cast<void*>(item), static_cast<void*>(blk.slotAddress(slot_index)));
                throw std::invalid_argument(
                    "Invalid item pointer; slot_index address mismatch");
            }

            if (!blk.testSlot(slot_index))
            {
                debug_println(
                    "Invalid item pointer; slot already free: "
                    "block_index={}, slot_index={}",
                    block_index, slot_index);
                throw std::invalid_argument("Item is already free");
            }

            blk.deallocateSlot(slot_index);
            if (blk.liveCount() == slots_per_block - 1)
            {
                block_available_map_.set(block_index);
            }
            debug_println("Item freed via hint ({}/{}), block_freemaps: {}",
                          block_index, slot_index, printHex(blk.freemap()));
            return;
        }

        if (has_index != has_gen)
        {
            debug_println(
                "Invalid item pointer; inconsistent block routing hint: "
                "has_index={}, has_gen={}",
                has_index, has_gen);
            throw std::invalid_argument(
                "Invalid item pointer; inconsistent block routing hint");
        }

        // Cold unhinted fallback (align-8 Pool frees and direct unhinted Slab calls).
        // O(1) for 1 live Block, O(log m) via base_address_map_ otherwise.
        const auto block_index_opt = findBlockForSlot(item);
        if (!block_index_opt.has_value())
        {
            debug_println(
                "Invalid item pointer; no corresponding block found: "
                "item={}",
                static_cast<void*>(item));
            throw std::invalid_argument("Invalid item pointer; no corresponding block found");
        }

        const std::size_t resolved_block = *block_index_opt;
        auto& blk = blocks_[resolved_block];
        std::byte* const block_start = blk.buffer();

        // Require an exact slot base. Integer division alone would map an
        // interior pointer (e.g. slot + 1) onto the wrong bit and corrupt
        // the freemap — or free a live neighbor.
        const auto offset = item - block_start;
        if (offset < 0)
        {
            throw std::invalid_argument(
                "Invalid item pointer; address is before block start");
        }
        const std::size_t byte_offset = static_cast<std::size_t>(offset);
        const std::size_t item_index = byte_offset / ElemSize;
        if (item_index >= slots_per_block || item != blk.slotAddress(item_index))
        {
            throw std::invalid_argument(
                "Invalid item pointer; not aligned to a slot boundary");
        }

        if (!blk.testSlot(item_index))
        {
            throw std::invalid_argument("Item is already free");
        }

        blk.deallocateSlot(item_index);
        if (blk.liveCount() == slots_per_block - 1)
        {
            block_available_map_.set(resolved_block);
        }
        debug_println("Item freed ({}/{}), block_freemaps: {}",
                      resolved_block, item_index, printHex(blk.freemap()));
    }


    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    Slab<ElemSize, Alignment, Lock>::Slab(bool preallocate_first_block)
    {
        debug_println(
            "Slab created with element size: {}, alignment: {}, allocation size: {}, "
            "preallocate_first_block={}",
            getElemSize(), getAlignment(), getAllocSize(), preallocate_first_block);

        if (preallocate_first_block)
        {
            RawBufferGuard staged_buf(block_type::allocateRawBuffer());
            typename lock_traits::guard_type guard(slab_lock);
            publishBlockBuffer(staged_buf);
        }
    }

    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    Slab<ElemSize, Alignment, Lock>::~Slab()
    {
        debug_println("Slab destroyed, freeing {} bytes of memory", getAllocatedMemory());
        // Each live Block's RAII destructor frees its aligned backing buffer.
    }

    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    std::size_t Slab<ElemSize, Alignment, Lock>::reclaimEmptyBlocks(std::size_t keep_min)
    {
        // Retain at least one live Block so the class stays warm for the next alloc.
        if (keep_min < 1)
        {
            keep_min = 1;
        }

        // Declared BEFORE lock so backing buffers are deleted OUTSIDE slab_lock.
        std::vector<std::byte*> to_free;
        {
            typename lock_traits::guard_type guard(slab_lock);

            // Every empty live Block has its bit set in block_available_map_, so
            // word-level find_next_set skips full and already-trimmed Blocks 64 at a time.
            std::size_t scan = 0;
            while (live_block_count_ > keep_min)
            {
                const std::size_t idx = block_available_map_.find_next_set(scan);
                if (idx >= blocks_.size())
                {
                    break;
                }
                scan = idx + 1;

                auto& blk = blocks_[idx];
                if (!blk.isLive() || !blk.isEmpty())
                {
                    continue;
                }

                // Reserve/push metadata before mutating Block state so bad_alloc
                // leaves the Slab consistent without leaking or double-freeing.
                to_free.push_back(blk.buffer());
                try
                {
                    trimmed_blocks_.push_back(static_cast<std::uint32_t>(idx));
                }
                catch (...)
                {
                    to_free.pop_back();
                    throw;
                }

                std::byte* doomed = blk.detachBuffer();
                base_address_map_.erase(doomed);
                block_available_map_.reset(idx);
                --live_block_count_;

                if (alloc_cursor_block_ == idx)
                {
                    alloc_cursor_slot_ = 0;
                }
            }

            debug_println(
                "reclaimEmptyBlocks<{},{}>: detached {}, remaining live blocks={} (capacity={})",
                ElemSize, Alignment, to_free.size(), live_block_count_, blocks_.size());
        }

        for (std::byte* doomed : to_free)
        {
            block_type::freeRawBuffer(doomed);
        }
        return to_free.size();
    }


    template<std::size_t ElemSize, std::size_t Alignment, mlvlib::BasicLockable Lock>
    std::size_t Slab<ElemSize, Alignment, Lock>::publishBlockBuffer(RawBufferGuard& raw_buf)
    {
        mlvlib::assert_lock_held(slab_lock);

        const std::uint16_t gen = next_block_generation_;
        std::uint16_t next_gen = static_cast<std::uint16_t>(gen + 1);
        if (next_gen == 0)
        {
            next_gen = 1; // 0 reserved for "no hint"
        }

        // O(1) reuse of a previously trimmed Block shell when available.
        if (!trimmed_blocks_.empty())
        {
            const std::size_t idx = trimmed_blocks_.back();
            // Insert into address map first so bad_alloc leaves Slab untouched.
            base_address_map_.emplace(raw_buf.ptr, idx);
            trimmed_blocks_.pop_back();
            blocks_[idx].attachBuffer(raw_buf.release(), gen);
            block_available_map_.set(idx);
            next_block_generation_ = next_gen;
            ++live_block_count_;
            return idx;
        }

        // Append a new Block with strong exception safety: do all potentially-
        // throwing allocations/reserves before mutating any container size.
        const std::size_t idx = blocks_.size();
        auto [map_it, inserted] = base_address_map_.emplace(raw_buf.ptr, idx);
        (void)inserted;
        try
        {
            blocks_.reserve(idx + 1);
            block_available_map_.reserve(idx + 1);
        }
        catch (...)
        {
            base_address_map_.erase(map_it);
            throw;
        }

        // Non-throwing final commit (all capacities >= idx + 1).
        blocks_.emplace_back(raw_buf.release(), gen);
        block_available_map_.push_set();
        next_block_generation_ = next_gen;
        ++live_block_count_;
        return idx;
    }

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_SLAB_HPP_
