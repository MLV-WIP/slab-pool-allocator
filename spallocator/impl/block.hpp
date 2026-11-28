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

#ifndef SPALLOCATOR_IMPL_BLOCK_HPP_
#define SPALLOCATOR_IMPL_BLOCK_HPP_

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <new>
#include <string>

#include "impl/helper.hpp"

namespace spallocator
{
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
        void clear() noexcept;
        bool none() const noexcept;
        bool all() const noexcept;

        // Next clear bit at or after `start`. Returns N if none.
        std::size_t find_next_clear(std::size_t start) const noexcept;

        const std::array<std::uint64_t, kWordCount>& words() const noexcept;

    private:
        std::array<std::uint64_t, kWordCount> words_{};
    };


    // Owns one aligned backing buffer, its generation tag, live slot count, and
    // per-slot freemap. Kept non-copyable and noexcept-movable so Slab can store
    // Blocks in an index-stable std::vector<Block> without reallocation copies.
    // A trimmed Block has buffer_ == nullptr (empty shell eligible for O(1) refill).
    template<std::size_t ElemSize, std::size_t Alignment = 16>
    class Block
    {
    public:
        static_assert(Alignment == 16 || Alignment == 32 || Alignment == 64,
            "Block Alignment must be 16, 32, or 64");
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
        static_assert(slots_per_block <= std::numeric_limits<std::uint16_t>::max(),
            "slots_per_block must fit in uint16_t routing hint");

        Block() noexcept = default;
        Block(std::byte* buffer, std::uint16_t generation) noexcept
            : buffer_(buffer), generation_(generation), live_count_(0)
        {}

        ~Block() noexcept
        {
            if (buffer_ != nullptr)
            {
                freeRawBuffer(buffer_);
            }
        }

        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;

        Block(Block&& other) noexcept
            : buffer_(other.buffer_),
              generation_(other.generation_),
              live_count_(other.live_count_),
              freemap_(other.freemap_)
        {
            other.buffer_ = nullptr;
            other.generation_ = 0;
            other.live_count_ = 0;
            other.freemap_.clear();
        }

        Block& operator=(Block&& other) noexcept
        {
            if (this != &other)
            {
                if (buffer_ != nullptr)
                {
                    freeRawBuffer(buffer_);
                }
                buffer_ = other.buffer_;
                generation_ = other.generation_;
                live_count_ = other.live_count_;
                freemap_ = other.freemap_;
                other.buffer_ = nullptr;
                other.generation_ = 0;
                other.live_count_ = 0;
                other.freemap_.clear();
            }
            return *this;
        }

        static std::byte* allocateRawBuffer()
        {
            return new(std::align_val_t{Alignment}) std::byte[block_bytes];
        }

        static void freeRawBuffer(std::byte* ptr) noexcept
        {
            // Explicit align_val_t delete for portability across C++20/23 + ASan.
            ::operator delete[](ptr, std::align_val_t{Alignment});
        }

        bool isLive() const noexcept { return buffer_ != nullptr; }
        bool isEmpty() const noexcept { return live_count_ == 0; }
        bool isFull() const noexcept { return live_count_ == slots_per_block; }
        bool hasFree() const noexcept
        {
            return buffer_ != nullptr && live_count_ < slots_per_block;
        }

        std::byte* buffer() const noexcept { return buffer_; }
        std::uint16_t generation() const noexcept { return generation_; }
        std::uint16_t liveCount() const noexcept { return live_count_; }
        const SlotFreemap<slots_per_block>& freemap() const noexcept { return freemap_; }

        std::byte* slotAddress(std::size_t slot_index) const noexcept
        {
            return buffer_ + slot_index * ElemSize;
        }

        bool containsAddress(const std::byte* ptr) const noexcept
        {
            return buffer_ != nullptr && ptr >= buffer_ && ptr < buffer_ + block_bytes;
        }

        void attachBuffer(std::byte* buf, std::uint16_t gen) noexcept
        {
            buffer_ = buf;
            generation_ = gen;
            live_count_ = 0;
            freemap_.clear();
        }

        // Caller must verify isLive() && isEmpty() and free the returned buffer
        // (typically after releasing the slab lock).
        std::byte* detachBuffer() noexcept
        {
            std::byte* doomed = buffer_;
            buffer_ = nullptr;
            live_count_ = 0;
            freemap_.clear();
            return doomed;
        }

        // Next-fit slot allocation starting at slot_start (wraps within this Block).
        // Fast path checks slot_start directly (inlineable); cold word scan is
        // split out so Slab::allocateItem keeps its hot path flat.
        // Returns slots_per_block if full or not live.
        std::size_t allocateSlot(std::size_t slot_start) noexcept
        {
            if (hasFree() && slot_start < slots_per_block && !freemap_.test(slot_start))
            {
                freemap_.set(slot_start);
                ++live_count_;
                return slot_start;
            }
            return allocateSlotSlow(slot_start);
        }

        bool testSlot(std::size_t slot_index) const noexcept
        {
            return freemap_.test(slot_index);
        }

        void deallocateSlot(std::size_t slot_index) noexcept
        {
            freemap_.reset(slot_index);
            --live_count_;
        }

    private:
        SPALLOCATOR_COLD_NOINLINE std::size_t allocateSlotSlow(std::size_t slot_start) noexcept
        {
            if (!hasFree())
            {
                return slots_per_block;
            }

            std::size_t item_index = freemap_.find_next_clear(slot_start);
            if (item_index >= slots_per_block && slot_start > 0)
            {
                item_index = freemap_.find_next_clear(0);
                if (item_index >= slot_start)
                {
                    item_index = slots_per_block;
                }
            }

            if (item_index < slots_per_block)
            {
                freemap_.set(item_index);
                ++live_count_;
            }
            return item_index;
        }

        std::byte* buffer_{nullptr};
        std::uint16_t generation_{0};
        // Packed in the 6-byte alignment gap before freemap_'s uint64_t array:
        // gives O(1) isEmpty() / isFull() at zero extra sizeof(Block).
        std::uint16_t live_count_{0};
        SlotFreemap<slots_per_block> freemap_{};
    };


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
    inline void SlotFreemap<N>::clear() noexcept
    {
        words_.fill(0);
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


    // Helper for debug output
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

        // Avoid evaluating `1 << 64` in the discarded ternary arm (clang -Werror).
        constexpr word_t kLowWordBits = []() constexpr {
            if constexpr (kMaskBits >= kWordBits)
            {
                return ~word_t{0};
            }
            else
            {
                return (word_t{1} << kMaskBits) - word_t{1};
            }
        }();
        constexpr std::bitset<N> kLowWordMask{kLowWordBits};

        for (std::size_t base = (start / kWordBits) * kWordBits; base < N;
             base += kWordBits)
        {
            const std::size_t bits_in_word =
                (base + kWordBits <= N) ? kWordBits : (N - base);
            const word_t width_mask =
                (bits_in_word >= kWordBits)
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

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_BLOCK_HPP_
