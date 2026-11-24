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

#include <array>
#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "impl/helper.hpp"
#include "impl/islab.hpp"
#include "mlvlib/spinlock.hpp"


namespace spallocator
{
    using mlvlib::SpinLock;

    // Compile-time slab buffer packing knobs (override via -D / ./configure).
#ifndef SPALLOCATOR_SLAB_TARGET_BYTES
#  define SPALLOCATOR_SLAB_TARGET_BYTES (8u * 1024u)
#endif
#ifndef SPALLOCATOR_SLAB_TARGET_MULTIPLIER
#  define SPALLOCATOR_SLAB_TARGET_MULTIPLIER 16u
#endif

    inline constexpr std::size_t kSlabTargetBytes =
        SPALLOCATOR_SLAB_TARGET_BYTES;
    inline constexpr std::size_t kSlabTargetMultiplier =
        SPALLOCATOR_SLAB_TARGET_MULTIPLIER;

    static_assert(kSlabTargetMultiplier >= 4,
        "kSlabTargetMultiplier must be at least 4 (min slots per Block)");
    static_assert(kSlabTargetBytes >= kSlabTargetMultiplier,
        "kSlabTargetBytes must be >= kSlabTargetMultiplier");
    static_assert((kSlabTargetBytes & (kSlabTargetBytes - 1)) == 0,
        "kSlabTargetBytes must be a power of two");
    // Largest ladder ElemSize today is 16384 (alignment class 64).
    static_assert(kSlabTargetMultiplier * 16384ull >= kSlabTargetBytes,
        "kSlabTargetMultiplier too small for largest ElemSize under target");

    // Exact-pack Block: slots = max(multiplier, target/ElemSize), zero tail.
    // (SPALLOCATOR_SLAB_TARGET_* knobs size Blocks; names kept for config stability.)
    template<std::size_t ElemSize>
    constexpr std::size_t selectBlockSize()
    {
        constexpr std::size_t from_target = kSlabTargetBytes / ElemSize;
        constexpr std::size_t slots =
            from_target > kSlabTargetMultiplier ? from_target : kSlabTargetMultiplier;
        return slots * ElemSize;
    }


    // Runtime-sized bit vector over uint64_t words. Used for per-Slab
    // Block availability so empty size classes do not embed bitset<max_blocks>
    // (~128 KiB for 4 KiB Blocks). Word ops are load/mask/store; storage
    // grows geometrically with live bit count.
    class DynBitset
    {
    public:
        bool test(std::size_t bit) const;
        void set(std::size_t bit);
        void reset(std::size_t bit);

        // Append one bit valued 1 (new Block starts with free slots).
        void push_set();
        void swap_bits(std::size_t a, std::size_t b);

        // Drop the last bit (after swap-with-last removal). Keeps word capacity.
        void pop_back();

        std::size_t size() const { return size_; }
        // Heap buffer footprint of the word array (capacity, not size).
        std::size_t bytes() const { return words_.capacity() * sizeof(std::uint64_t); }

    private:
        std::vector<std::uint64_t> words_;
        std::size_t size_{0};
    };


    // Incomplete until after Slab; C++17 vector permits this while ~Slab /
    // method bodies are defined only once SlotFreemap is complete.
    template<std::size_t N>
    class SlotFreemap;


    // Compound key: every slot is Alignment-aligned because the Block uses
    // align_val_t{Alignment} and ElemSize is a multiple of Alignment.
    template<std::size_t ElemSize, std::size_t Alignment = 16>
    class Slab: public ISlab
    {
    public: // methods
        std::byte* allocateItem(std::size_t size) override;
        void deallocateItem(std::byte* item) override;

        constexpr std::size_t getElemSize() const { return ElemSize; }
        constexpr std::size_t getAlignment() const { return Alignment; }
        constexpr std::size_t getAllocSize() const { return block_bytes; }
        std::size_t getAllocatedMemory() const override { return block_bases.size() * block_bytes; }
        std::size_t getAvailabilityMapBytes() const { return block_available_map.bytes(); }
        std::size_t getBlockCount() const { return block_bases.size(); }

        // Delete fully empty data blocks; always retain at least keep_min
        // blocks (clamped to ≥ 1). Not called from the alloc/free hot path.
        std::size_t reclaimEmptyBlocks(std::size_t keep_min = 1) override;

        // `slot_base` must be an exact Slot start (not a Pool user pointer).
        std::optional<std::size_t> findBlockForSlot(std::byte* slot_base) const;

        // preallocate_first_block: map one data block at construction (warm
        // start). false leaves the engine empty until the first allocateItem
        // (Pool uses this for large / high-align classes — see pool.hpp).
        explicit Slab(bool preallocate_first_block = false);
        ~Slab() override;

    private: // methods
        Slab(const Slab&) = delete;
        Slab& operator=(const Slab&) = delete;
        Slab(Slab&&) = delete;
        Slab& operator=(Slab&&) = delete;

        void allocateNewBlock();
        void destroyBlockAt(std::size_t index);
    
    private: // data members
        static_assert(Alignment == 16 || Alignment == 32 || Alignment == 64,
            "Slab Alignment must be 16, 32, or 64");
        static_assert(ElemSize % Alignment == 0,
            "ElemSize must be a multiple of Alignment");
        static_assert(ElemSize >= Alignment,
            "ElemSize must be at least Alignment");

        static constexpr std::size_t block_bytes{selectBlockSize<ElemSize>()};
        static constexpr std::size_t slots_per_block{block_bytes / ElemSize};
        static_assert(block_bytes == slots_per_block * ElemSize,
            "Block must be an exact multiple of ElemSize (zero tail)");
        static_assert(slots_per_block >= kSlabTargetMultiplier,
            "Block must hold at least kSlabTargetMultiplier slots");

        std::vector<std::byte*> block_bases;
        // Per-Block slot freemap (SlotFreemap): compile-time slot count,
        // word array storage. Convention: bit set ⇒ occupied; clear ⇒ free.
        std::vector<SlotFreemap<slots_per_block>> block_freemaps;
        static constexpr std::size_t max_blocks{kMaxClassBytes / block_bytes};
        static_assert(max_blocks >= 1,
            "kMaxClassBytes must allow at least one Block of this ElemSize");
        DynBitset block_available_map;

        std::map<std::byte*, std::size_t> base_address_map;

        // Next-fit cursors for allocateItem (indices into block_bases / freemap).
        // Reset when reclaim/swap-with-last invalidates Block indices.
        std::size_t alloc_cursor_block_{0};
        std::size_t alloc_cursor_slot_{0};

        SpinLock slab_lock;
    };


    // Per-buffer slot freemap: fixed uint64_t words instead of std::bitset.
    // Convention: set ⇒ occupied, clear ⇒ free. Direct word access avoids bitset
    // shift + to_ullong on the find-next-clear hot path.
    template<std::size_t N>
    class SlotFreemap
    {
    public:
        static constexpr std::size_t kBitCount = N;
        static constexpr std::size_t kWordBits = 64;
        static constexpr std::size_t kWordCount = (N + kWordBits - 1) / kWordBits;

        constexpr std::size_t size() const noexcept { return N; }

        bool test(std::size_t i) const noexcept;
        void set(std::size_t i) noexcept;
        void reset(std::size_t i) noexcept;
        bool none() const noexcept;
        bool all() const noexcept;

        // Next clear bit at or after `start`. Returns N if none.
        std::size_t find_next_clear(std::size_t start) const noexcept;

        const std::array<std::uint64_t, kWordCount>& words() const noexcept;

    private:
        std::array<std::uint64_t, kWordCount> words_{};
    };


    // ---------------------------------------------------------------------------
    // DynBitset
    // ---------------------------------------------------------------------------

    inline bool DynBitset::test(std::size_t bit) const
    {
        return (words_[bit >> 6] >> (bit & 63)) & 1ull;
    }

    inline void DynBitset::set(std::size_t bit)
    {
        words_[bit >> 6] |= (std::uint64_t{1} << (bit & 63));
    }

    inline void DynBitset::reset(std::size_t bit)
    {
        words_[bit >> 6] &= ~(std::uint64_t{1} << (bit & 63));
    }

    inline void DynBitset::push_set()
    {
        const std::size_t bit = size_;
        const std::size_t word = bit >> 6;
        if (word >= words_.size())
        {
            const std::size_t need = word + 1;
            const std::size_t grown = words_.empty() ? need : words_.size() * 2;
            words_.resize(grown > need ? grown : need, 0);
        }
        ++size_;
        set(bit);
    }

    inline void DynBitset::swap_bits(std::size_t a, std::size_t b)
    {
        if (a == b)
        {
            return;
        }
        const bool a_set = test(a);
        if (test(b))
        {
            set(a);
        }
        else
        {
            reset(a);
        }
        if (a_set)
        {
            set(b);
        }
        else
        {
            reset(b);
        }
    }

    inline void DynBitset::pop_back()
    {
        if (size_ == 0)
        {
            return;
        }
        --size_;
    }


    // ---------------------------------------------------------------------------
    // SlotFreemap
    // ---------------------------------------------------------------------------

    template<std::size_t N>
    inline bool SlotFreemap<N>::test(std::size_t i) const noexcept
    {
        return (words_[i / kWordBits] >> (i % kWordBits)) & 1ull;
    }

    template<std::size_t N>
    inline void SlotFreemap<N>::set(std::size_t i) noexcept
    {
        words_[i / kWordBits] |= (std::uint64_t{1} << (i % kWordBits));
    }

    template<std::size_t N>
    inline void SlotFreemap<N>::reset(std::size_t i) noexcept
    {
        words_[i / kWordBits] &= ~(std::uint64_t{1} << (i % kWordBits));
    }

    template<std::size_t N>
    inline bool SlotFreemap<N>::none() const noexcept
    {
        for (std::size_t w = 0; w < kWordCount; ++w)
        {
            if (words_[w] != 0)
            {
                return false;
            }
        }
        return true;
    }

    template<std::size_t N>
    inline bool SlotFreemap<N>::all() const noexcept
    {
        constexpr std::size_t full_words = N / kWordBits;
        for (std::size_t w = 0; w < full_words; ++w)
        {
            if (words_[w] != ~std::uint64_t{0})
            {
                return false;
            }
        }
        if constexpr ((N % kWordBits) != 0)
        {
            const std::uint64_t mask =
                (std::uint64_t{1} << (N % kWordBits)) - std::uint64_t{1};
            if ((words_[full_words] & mask) != mask)
            {
                return false;
            }
        }
        return true;
    }

    template<std::size_t N>
    inline std::size_t SlotFreemap<N>::find_next_clear(std::size_t start) const noexcept
    {
        if (start >= N)
        {
            return N;
        }

        std::size_t word = start / kWordBits;
        std::size_t bit0 = start % kWordBits;
        for (; word < kWordCount; ++word, bit0 = 0)
        {
            const std::size_t base = word * kWordBits;
            const std::size_t bits_in_word =
                (base + kWordBits <= N) ? kWordBits : (N - base);
            const std::uint64_t width_mask =
                (bits_in_word == kWordBits)
                    ? ~std::uint64_t{0}
                    : ((std::uint64_t{1} << bits_in_word) - std::uint64_t{1});
            const std::uint64_t skip_mask =
                (bit0 == 0) ? ~std::uint64_t{0} : (~std::uint64_t{0} << bit0);
            const std::uint64_t free_word =
                (~words_[word]) & width_mask & skip_mask;
            if (free_word != 0)
            {
                return base + static_cast<std::size_t>(std::countr_zero(free_word));
            }
        }
        return N;
    }

    template<std::size_t N>
    inline const std::array<std::uint64_t, SlotFreemap<N>::kWordCount>&
    SlotFreemap<N>::words() const noexcept
    {
        return words_;
    }


    // Helper for debug output (used by Slab definitions below)
    template<std::size_t N>
    std::string printHex(const SlotFreemap<N>& bits)
    {
        std::string out;
        for (std::size_t chunk = (N + 31) / 32; chunk-- > 0; )
        {
            const std::size_t start = chunk * 32;
            const std::size_t end = std::min(start + 32, N);
            std::uint32_t value = 0;
            for (std::size_t i = start; i < end; ++i)
            {
                if (bits.test(i))
                {
                    value |= (1U << (i - start));
                }
            }
            if (chunk < (N + 31) / 32 - 1)
            {
                out += " ";
            }
            out += std::format("{:08X}", value);
        }
        return out;
    }


    // Next clear bit — SlotFreemap path (production freemap).
    template<std::size_t N>
    std::size_t find_next_clear_bit(const SlotFreemap<N>& occupied, std::size_t start)
    {
        return occupied.find_next_clear(start);
    }


    // Kept for BitsetSearchTest / algorithm parity checks.
    // Freemap convention: set ⇒ occupied, clear ⇒ free.
    template<std::size_t N>
    std::size_t find_next_clear_bit(const std::bitset<N>& occupied, std::size_t start)
    {
        if (start >= N)
        {
            return N;
        }

        using word_t = unsigned long long;
        constexpr std::size_t kWordBits = std::numeric_limits<word_t>::digits; // 64
        constexpr std::size_t kMaskBits = N < kWordBits ? N : kWordBits;
        constexpr word_t kLowWordBits =
            (kMaskBits == kWordBits)
                ? ~word_t{0}
                : ((word_t{1} << kMaskBits) - word_t{1});
        constexpr std::bitset<N> kLowWordMask{kLowWordBits};

        for (std::size_t base = (start / kWordBits) * kWordBits; base < N;
             base += kWordBits)
        {
            const std::size_t bits_in_word =
                (base + kWordBits <= N) ? kWordBits : (N - base);
            const word_t width_mask =
                (bits_in_word == kWordBits)
                    ? ~word_t{0}
                    : ((word_t{1} << bits_in_word) - word_t{1});

            const word_t occupied_word =
                ((occupied >> base) & kLowWordMask).to_ullong() & width_mask;

            const std::size_t bit0 = (base < start) ? (start - base) : 0;
            const word_t skip_mask =
                (bit0 == 0) ? ~word_t{0} : (~word_t{0} << bit0);
            const word_t free_word = (~occupied_word) & width_mask & skip_mask;
            if (free_word != 0)
            {
                return base + static_cast<std::size_t>(std::countr_zero(free_word));
            }
        }
        return N;
    }


    // ---------------------------------------------------------------------------
    // Slab
    // ---------------------------------------------------------------------------

    template<std::size_t ElemSize, std::size_t Alignment>
    std::byte* Slab<ElemSize, Alignment>::allocateItem(std::size_t size)
    {
        // Must remain active in release builds: an oversized request would
        // otherwise be placed in a too-small slot (silent overrun).
        if (size > ElemSize)
        {
            throw std::invalid_argument(
                std::format("Requested size {} exceeds slab element size {}", size, ElemSize));
        }

        std::scoped_lock<SpinLock> guard(slab_lock);

        // Next-fit: resume at (alloc_cursor_block_, alloc_cursor_slot_). When every
        // live Block is full, grow then retry.
        for (;;)
        {
            const std::size_t nblocks = block_bases.size();
            if (nblocks == 0)
            {
                allocateNewBlock();
                alloc_cursor_block_ = 0;
                alloc_cursor_slot_ = 0;
                continue;
            }

            if (alloc_cursor_block_ >= nblocks)
            {
                alloc_cursor_block_ = 0;
                alloc_cursor_slot_ = 0;
            }

            std::size_t block_index = alloc_cursor_block_;
            std::size_t slot_start = alloc_cursor_slot_;
            for (std::size_t visited = 0; visited < nblocks; ++visited)
            {
                if (block_available_map.test(block_index))
                {
                    auto& block_slots = block_freemaps[block_index];
                    // Next-fit fast path: if the cursor slot is free, take it
                    // without a freemap word scan (common for LIFO / local churn).
                    std::size_t item_index = block_slots.size();
                    if (slot_start < block_slots.size() && !block_slots.test(slot_start))
                    {
                        item_index = slot_start;
                    }
                    else
                    {
                        // Forward from the cursor; if that misses and the cursor
                        // was mid-Block, wrap within this Block before leaving.
                        item_index = find_next_clear_bit(block_slots, slot_start);
                        if (item_index >= block_slots.size() && slot_start > 0)
                        {
                            item_index = find_next_clear_bit(block_slots, 0);
                            if (item_index >= slot_start)
                            {
                                item_index = block_slots.size(); // already scanned
                            }
                        }
                    }
                    if (item_index < block_slots.size())
                    {
                        block_slots.set(item_index);
                        if (block_slots.all())
                        {
                            block_available_map.reset(block_index);
                        }

                        // Advance cursor past this slot (next-fit).
                        alloc_cursor_block_ = block_index;
                        alloc_cursor_slot_ = item_index + 1;
                        if (alloc_cursor_slot_ >= block_slots.size())
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
                                      printHex(block_slots));
                        return block_bases[block_index] + item_index * ElemSize;
                    }
                }

                block_index = block_index + 1;
                if (block_index >= nblocks)
                {
                    block_index = 0;
                }
                slot_start = 0; // only the first Block uses the saved slot cursor
            }

            // allocateNewBlock throws when max_blocks is reached.
            allocateNewBlock();
            alloc_cursor_block_ = block_bases.size() - 1;
            alloc_cursor_slot_ = 0;
            debug_println("New block<{},{}> allocated, total blocks: {}",
                          ElemSize, Alignment, block_bases.size());
        }
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    std::optional<std::size_t> Slab<ElemSize, Alignment>::findBlockForSlot(std::byte* slot_base) const
    {
        // Single-Block fast path: skip the address map (common steady state).
        if (block_bases.size() == 1)
        {
            std::byte* const block_start = block_bases[0];
            std::byte* const block_end = block_start + block_bytes;
            if (slot_base >= block_start && slot_base < block_end)
            {
                return 0;
            }
            debug_println(
                "Slot base {} is not within sole block range {} - {}; treating as unknown pointer",
                static_cast<void*>(slot_base),
                static_cast<void*>(block_start),
                static_cast<void*>(block_end));
            return std::nullopt;
        }

        auto it = base_address_map.upper_bound(slot_base);
        if (it != base_address_map.begin())
        {
            --it;
            auto block_start = it->first;
            auto block_end = block_start + block_bytes;
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


    template<std::size_t ElemSize, std::size_t Alignment>
    void Slab<ElemSize, Alignment>::deallocateItem(std::byte* item)
    {
        if (!item)
        {
            return;
        }

        std::scoped_lock<SpinLock> guard(slab_lock);

        // Find which Block this item belongs to
        if (auto block_index_opt = findBlockForSlot(item); !block_index_opt.has_value())
        {
            throw std::invalid_argument("Invalid item pointer; no corresponding block found");
        }
        else
        {
            auto block_index = *block_index_opt;

            auto block_start = block_bases[block_index];
            auto& block_slots = block_freemaps[block_index];

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
            if (item_index >= block_slots.size() ||
                item != block_start + item_index * ElemSize)
            {
                throw std::invalid_argument(
                    "Invalid item pointer; not aligned to a slot boundary");
            }

            if (block_slots.test(item_index))
            {
                // Free the item
                block_slots.reset(item_index);
                // This Block now has free space
                block_available_map.set(block_index);
                debug_println("Item freed ({}/{}), block_freemaps: {}",
                              block_index, item_index, printHex(block_slots));
                return;
            }
            else
            {
                throw std::invalid_argument("Item is already free");
            }
        }

        throw std::invalid_argument("Invalid item pointer; item not found in block");
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    Slab<ElemSize, Alignment>::Slab(bool preallocate_first_block)
    {
        debug_println(
            "Slab created with element size: {}, alignment: {}, allocation size: {}, "
            "preallocate_first_block={}",
            getElemSize(), getAlignment(), getAllocSize(), preallocate_first_block);

        if (preallocate_first_block)
        {
            allocateNewBlock();
        }
    }

    template<std::size_t ElemSize, std::size_t Alignment>
    Slab<ElemSize, Alignment>::~Slab()
    {
        debug_println("Slab destroyed, freeing {} bytes of memory", getAllocatedMemory());
        for (auto ptr : block_bases)
        {
            // C++23 is improved to handle aligned deallocation automatically.
            // For earlier standards, we need to explicitly pass the alignment
            // to the delete operator. Unfortunately, this is incomplete in
            // gcc-14's implementation of the address sanitizer in spite of
            // otherwise decent C++23 support, so we need to use the older C++17
            // style deallocation here for portability
            ::operator delete[](ptr, std::align_val_t{Alignment});

            // Preferred C++23 form that we are avoiding for now due to above issues:
            //delete[] ptr;
        }
    }

    template<std::size_t ElemSize, std::size_t Alignment>
    std::size_t Slab<ElemSize, Alignment>::reclaimEmptyBlocks(std::size_t keep_min)
    {
        // Retain at least one Block so the class stays warm for the next alloc.
        if (keep_min < 1)
        {
            keep_min = 1;
        }

        std::scoped_lock<SpinLock> guard(slab_lock);

        std::size_t reclaimed = 0;
        std::size_t index = 0;
        while (index < block_bases.size())
        {
            if (block_bases.size() <= keep_min)
            {
                break;
            }

            if (!block_freemaps[index].none())
            {
                ++index;
                continue;
            }

            destroyBlockAt(index);
            ++reclaimed;
            // New occupant at index (swap-with-last) must be examined next.
        }

        debug_println(
            "reclaimEmptyBlocks<{},{}>: removed {}, remaining blocks={}",
            ElemSize, Alignment, reclaimed, block_bases.size());
        return reclaimed;
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    void Slab<ElemSize, Alignment>::destroyBlockAt(std::size_t index)
    {
        std::byte* doomed = block_bases[index];
        base_address_map.erase(doomed);

        const std::size_t last = block_bases.size() - 1;
        if (index != last)
        {
            block_bases[index] = block_bases[last];
            block_freemaps[index] = block_freemaps[last];
            block_available_map.swap_bits(index, last);
            base_address_map[block_bases[index]] = index;
        }

        block_bases.pop_back();
        block_freemaps.pop_back();
        block_available_map.pop_back();

        // Block indices may have been swapped; drop the next-fit hint.
        alloc_cursor_block_ = 0;
        alloc_cursor_slot_ = 0;

        ::operator delete[](doomed, std::align_val_t{Alignment});
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    void Slab<ElemSize, Alignment>::allocateNewBlock()
    {
        if (block_bases.size() >= max_blocks)
        {
            throw std::out_of_range(
                std::format("Cannot allocate more than {} blocks of size {} bytes",
                            max_blocks, block_bytes));
        }

        std::byte* new_block = new(std::align_val_t{Alignment}) std::byte[block_bytes];
        block_bases.push_back(new_block);
        base_address_map[new_block] = block_bases.size() - 1;
        block_freemaps.emplace_back();
        // New Block has free slots; grow availability words only as needed.
        block_available_map.push_set();
    }



} // namespace spallocator

#endif // SPALLOCATOR_IMPL_SLAB_HPP_
