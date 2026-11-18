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

#ifndef SLAB_HPP_
#define SLAB_HPP_

#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "helper.hpp"
#include "spinlock.hpp"


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
        "kSlabTargetMultiplier must be at least 4 (min slots per buffer)");
    static_assert(kSlabTargetBytes >= kSlabTargetMultiplier,
        "kSlabTargetBytes must be >= kSlabTargetMultiplier");
    static_assert((kSlabTargetBytes & (kSlabTargetBytes - 1)) == 0,
        "kSlabTargetBytes must be a power of two");
    // Largest ladder ElemSize today is 4096 (axis 64).
    static_assert(kSlabTargetMultiplier * 4096ull >= kSlabTargetBytes,
        "kSlabTargetMultiplier too small for largest ElemSize under target");

    // Exact-pack buffer: slots = max(multiplier, target/ElemSize), zero tail.
    template<std::size_t ElemSize>
    constexpr std::size_t selectBufferSize()
    {
        constexpr std::size_t from_target = kSlabTargetBytes / ElemSize;
        constexpr std::size_t slots =
            from_target > kSlabTargetMultiplier ? from_target : kSlabTargetMultiplier;
        return slots * ElemSize;
    }


    class AbstractSlab
    {
    public: // methods
        virtual std::byte* allocateItem(std::size_t size) = 0;
        virtual void deallocateItem(std::byte* item) = 0;

        // Release fully empty slab buffers back to the OS. Default: no-op
        // (SlabProxy). Returns the number of slab buffers deleted.
        virtual std::size_t reclaimEmptySlabs(std::size_t keep_min = 1);
        virtual std::size_t getAllocatedMemory() const { return 0; }

        virtual ~AbstractSlab() = default;

    protected: // methods
        AbstractSlab() = default;

    private: // methods
        AbstractSlab(const AbstractSlab&) = delete;
        AbstractSlab& operator=(const AbstractSlab&) = delete;
        AbstractSlab(AbstractSlab&&) = delete;
        AbstractSlab& operator=(AbstractSlab&&) = delete;
    };


    // Runtime-sized bit vector over uint64_t words. Used for per-slab
    // availability so empty size classes do not embed bitset<max_slabs>
    // (~128 KiB for 4 KiB slabs). Word ops are load/mask/store; storage
    // grows geometrically with live bit count.
    class DynBitset
    {
    public:
        bool test(std::size_t bit) const;
        void set(std::size_t bit);
        void reset(std::size_t bit);

        // Append one bit valued 1 (new slab starts with free slots).
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


    // Compound key: every slot is Alignment-aligned because the buffer uses
    // align_val_t{Alignment} and ElemSize is a multiple of Alignment.
    template<std::size_t ElemSize, std::size_t Alignment = 16>
    class Slab: public AbstractSlab
    {
    public: // methods
        std::byte* allocateItem(std::size_t size) override;
        void deallocateItem(std::byte* item) override;

        constexpr std::size_t getElemSize() const { return ElemSize; }
        constexpr std::size_t getAlignment() const { return Alignment; }
        constexpr std::size_t getAllocSize() const { return slab_alloc_size; }
        std::size_t getAllocatedMemory() const override { return slab_data.size() * slab_alloc_size; }
        std::size_t getAvailabilityMapBytes() const { return slab_available_map.bytes(); }
        std::size_t getSlabCount() const { return slab_data.size(); }

        // Delete fully empty slab buffers; always retain at least keep_min
        // slabs (clamped to ≥ 1). Not called from the alloc/free hot path.
        std::size_t reclaimEmptySlabs(std::size_t keep_min = 1) override;

        std::optional<std::size_t> findSlabForItem(std::byte* item) const;

        Slab();
        ~Slab() override;

    private: // methods
        Slab(const Slab&) = delete;
        Slab& operator=(const Slab&) = delete;
        Slab(Slab&&) = delete;
        Slab& operator=(Slab&&) = delete;

        void allocateNewSlab();
        void destroySlabAt(std::size_t index);
    
    private: // data members
        static_assert(Alignment == 16 || Alignment == 32 || Alignment == 64,
            "Slab Alignment must be 16, 32, or 64");
        static_assert(ElemSize % Alignment == 0,
            "ElemSize must be a multiple of Alignment");
        static_assert(ElemSize >= Alignment,
            "ElemSize must be at least Alignment");

        static constexpr std::size_t slab_alloc_size{selectBufferSize<ElemSize>()};
        static constexpr std::size_t slots_per_buffer{slab_alloc_size / ElemSize};
        static_assert(slab_alloc_size == slots_per_buffer * ElemSize,
            "slab buffer must be an exact multiple of ElemSize (zero tail)");
        static_assert(slots_per_buffer >= kSlabTargetMultiplier,
            "slab buffer must hold at least kSlabTargetMultiplier slots");

        std::vector<std::byte*> slab_data;
        // Per-slab slot maps stay std::bitset (compile-time slot count).
        // Convention: bit set ⇒ slot occupied; clear ⇒ free.
        std::vector<std::bitset<slots_per_buffer>> slab_map;
        static constexpr std::size_t max_slabs{kMaxClassBytes / slab_alloc_size};
        static_assert(max_slabs >= 1,
            "kMaxClassBytes must allow at least one buffer of this ElemSize");
        DynBitset slab_available_map;

        std::map<std::byte*, std::size_t> base_address_map;

        // Next-fit cursors for allocateItem (indices into slab_data / freemap).
        // Reset when reclaim/swap-with-last invalidates buffer indices.
        std::size_t alloc_cursor_slab_{0};
        std::size_t alloc_cursor_slot_{0};

        SpinLock slab_lock;
    };


    //
    // SlabProxy does not manage its own memory; it delegates to
    // standard memory allocation methods for large memory
    // allocations that aren't suitable for small-memory slab
    // optimizations
    //
    class SlabProxy: public AbstractSlab
    {
    public: // methods
        std::byte* allocateItem(std::size_t size) override;
        void deallocateItem(std::byte* item) override;

        SlabProxy() = default;
        ~SlabProxy() override = default;

    private: // methods
        SlabProxy(const SlabProxy&) = delete;
        SlabProxy& operator=(const SlabProxy&) = delete;
        SlabProxy(SlabProxy&&) = delete;
        SlabProxy& operator=(SlabProxy&&) = delete;

    private: // data members
    };


    // ---------------------------------------------------------------------------
    // AbstractSlab
    // ---------------------------------------------------------------------------

    inline std::size_t AbstractSlab::reclaimEmptySlabs(std::size_t /*keep_min*/)
    {
        return 0;
    }


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


    // Helper for debug output (used by Slab definitions below)
    template<std::size_t N>
    std::string printHex(const std::bitset<N>& bits)
    {
        std::string out;

        // Process in 32-bit chunks from high to low
        for (std::size_t chunk = (N + 31) / 32; chunk-- > 0; )
        {
            std::size_t start = chunk * 32;
            std::size_t end = std::min(start + 32, N);

            // Extract 32-bit value
            uint32_t value = 0;
            for (std::size_t i = start; i < end; ++i)
            {
                if (bits[i])
                {
                    value |= (1U << (i - start));
                }
            }

            // Print with separator
            if (chunk < (N + 31) / 32 - 1)
            {
                out += " ";
            }
            out += std::format("{:08X}", value);
        }
        return out;
    }


    // Next clear bit (free slot) at or after `start`. Returns N if none.
    // Word walk + std::countr_zero — same CTZ hardware as libstdc++
    // bitset::_Find_next, without relying on that GNU extension.
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
        // Low-kWordBits (or N, if smaller) mask for to_ullong extraction.
        // Built from an ullong ctor — bitset::set is not constexpr until C++23.
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

            // Extract occupied bits [base, base+bits_in_word) as a machine word.
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

        // Next-fit: resume at (alloc_cursor_slab_, alloc_cursor_slot_). When every
        // live buffer is full, grow then retry.
        for (;;)
        {
            const std::size_t nslabs = slab_data.size();
            if (nslabs == 0)
            {
                allocateNewSlab();
                alloc_cursor_slab_ = 0;
                alloc_cursor_slot_ = 0;
                continue;
            }

            if (alloc_cursor_slab_ >= nslabs)
            {
                alloc_cursor_slab_ = 0;
                alloc_cursor_slot_ = 0;
            }

            std::size_t slab_index = alloc_cursor_slab_;
            std::size_t slot_start = alloc_cursor_slot_;
            for (std::size_t visited = 0; visited < nslabs; ++visited)
            {
                if (slab_available_map.test(slab_index))
                {
                    auto& slab_slots = slab_map[slab_index];
                    // Forward from the cursor; if that misses and the cursor was
                    // mid-buffer, wrap within this buffer before leaving it.
                    std::size_t item_index =
                        find_next_clear_bit(slab_slots, slot_start);
                    if (item_index >= slab_slots.size() && slot_start > 0)
                    {
                        item_index = find_next_clear_bit(slab_slots, 0);
                        if (item_index >= slot_start)
                        {
                            item_index = slab_slots.size(); // already scanned
                        }
                    }
                    if (item_index < slab_slots.size())
                    {
                        slab_slots.set(item_index);
                        if (slab_slots.all())
                        {
                            slab_available_map.reset(slab_index);
                        }

                        // Advance cursor past this slot (next-fit).
                        alloc_cursor_slab_ = slab_index;
                        alloc_cursor_slot_ = item_index + 1;
                        if (alloc_cursor_slot_ >= slab_slots.size())
                        {
                            alloc_cursor_slot_ = 0;
                            alloc_cursor_slab_ = slab_index + 1;
                            if (alloc_cursor_slab_ >= nslabs)
                            {
                                alloc_cursor_slab_ = 0;
                            }
                        }

                        debug_println("Item allocated ({}/{}), slab_map<{},{}>: {}",
                                      slab_index, item_index, ElemSize, Alignment,
                                      printHex(slab_slots));
                        return slab_data[slab_index] + item_index * ElemSize;
                    }
                }

                slab_index = slab_index + 1;
                if (slab_index >= nslabs)
                {
                    slab_index = 0;
                }
                slot_start = 0; // only the first buffer uses the saved slot cursor
            }

            // allocateNewSlab throws when max_slabs is reached.
            allocateNewSlab();
            alloc_cursor_slab_ = slab_data.size() - 1;
            alloc_cursor_slot_ = 0;
            debug_println("New slab<{},{}> allocated, total slabs: {}",
                          ElemSize, Alignment, slab_data.size());
        }
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    std::optional<std::size_t> Slab<ElemSize, Alignment>::findSlabForItem(std::byte* item) const
    {
        auto it = base_address_map.upper_bound(item);
        if (it != base_address_map.begin())
        {
            --it;
            auto slab_start = it->first;
            auto slab_end = slab_start + slab_alloc_size;
            // Predecessor start is only a candidate. Invalid pointers (or
            // addresses in gaps between slabs) can upper_bound into this path
            // without falling inside [slab_start, slab_end).
            if (item >= slab_start && item < slab_end)
            {
                return it->second;
            }

            debug_println(
                "Item {} is not within candidate slab range {} - {}; treating as unknown pointer",
                static_cast<void*>(item),
                static_cast<void*>(slab_start),
                static_cast<void*>(slab_end));
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

        // Find which slab this item belongs to
        if (auto slab_index_opt = findSlabForItem(item); !slab_index_opt.has_value())
        {
            throw std::invalid_argument("Invalid item pointer; no corresponding slab found");
        }
        else
        {
            auto slab_index = *slab_index_opt;

            auto slab_start = slab_data[slab_index];
            auto& slab_slots = slab_map[slab_index];

            // Require an exact slot base. Integer division alone would map an
            // interior pointer (e.g. slot + 1) onto the wrong bit and corrupt
            // the freemap — or free a live neighbor.
            const auto offset = item - slab_start;
            if (offset < 0)
            {
                throw std::invalid_argument(
                    "Invalid item pointer; address is before slab start");
            }
            const std::size_t byte_offset = static_cast<std::size_t>(offset);
            const std::size_t item_index = byte_offset / ElemSize;
            if (item_index >= slab_slots.size() ||
                item != slab_start + item_index * ElemSize)
            {
                throw std::invalid_argument(
                    "Invalid item pointer; not aligned to a slot boundary");
            }

            if (slab_slots.test(item_index))
            {
                // Free the item
                slab_slots.reset(item_index);
                // This slab now has free space
                slab_available_map.set(slab_index);
                debug_println("Item freed ({}/{}), slab_map: {}",
                              slab_index, item_index, printHex(slab_slots));
                return;
            }
            else
            {
                throw std::invalid_argument("Item is already free");
            }
        }

        throw std::invalid_argument("Invalid item pointer; item not found in slab");
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    Slab<ElemSize, Alignment>::Slab()
    {
        debug_println("Slab created with element size: {}, alignment: {}, allocation size: {}",
                      getElemSize(), getAlignment(), getAllocSize());

        allocateNewSlab();
    }

    template<std::size_t ElemSize, std::size_t Alignment>
    Slab<ElemSize, Alignment>::~Slab()
    {
        debug_println("Slab destroyed, freeing {} bytes of memory", getAllocatedMemory());
        for (auto ptr : slab_data)
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
    std::size_t Slab<ElemSize, Alignment>::reclaimEmptySlabs(std::size_t keep_min)
    {
        // Retain at least one slab so the class stays warm for the next alloc.
        if (keep_min < 1)
        {
            keep_min = 1;
        }

        std::scoped_lock<SpinLock> guard(slab_lock);

        std::size_t reclaimed = 0;
        std::size_t index = 0;
        while (index < slab_data.size())
        {
            if (slab_data.size() <= keep_min)
            {
                break;
            }

            if (!slab_map[index].none())
            {
                ++index;
                continue;
            }

            destroySlabAt(index);
            ++reclaimed;
            // New occupant at index (swap-with-last) must be examined next.
        }

        debug_println(
            "reclaimEmptySlabs<{},{}>: removed {}, remaining slabs={}",
            ElemSize, Alignment, reclaimed, slab_data.size());
        return reclaimed;
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    void Slab<ElemSize, Alignment>::destroySlabAt(std::size_t index)
    {
        std::byte* doomed = slab_data[index];
        base_address_map.erase(doomed);

        const std::size_t last = slab_data.size() - 1;
        if (index != last)
        {
            slab_data[index] = slab_data[last];
            slab_map[index] = slab_map[last];
            slab_available_map.swap_bits(index, last);
            base_address_map[slab_data[index]] = index;
        }

        slab_data.pop_back();
        slab_map.pop_back();
        slab_available_map.pop_back();

        // Buffer indices may have been swapped; drop the next-fit hint.
        alloc_cursor_slab_ = 0;
        alloc_cursor_slot_ = 0;

        ::operator delete[](doomed, std::align_val_t{Alignment});
    }


    template<std::size_t ElemSize, std::size_t Alignment>
    void Slab<ElemSize, Alignment>::allocateNewSlab()
    {
        if (slab_data.size() >= max_slabs)
        {
            throw std::out_of_range(
                std::format("Cannot allocate more than {} slabs of size {} bytes",
                            max_slabs, slab_alloc_size));
        }

        std::byte* new_slab = new(std::align_val_t{Alignment}) std::byte[slab_alloc_size];
        slab_data.push_back(new_slab);
        base_address_map[new_slab] = slab_data.size() - 1;
        slab_map.emplace_back();
        // New slab has free slots; grow availability words only as needed.
        slab_available_map.push_set();
    }


    // ---------------------------------------------------------------------------
    // SlabProxy
    // ---------------------------------------------------------------------------

    // Oversize / beyond-axis-max path. Always uses 64-byte aligned new/delete
    // so any Pool request alignment ≤ 64 is satisfied.
    inline constexpr std::size_t kProxyAlignment = 64;

    inline std::byte* SlabProxy::allocateItem(std::size_t elem_size)
    {
        // Active in all build types (not runtime_assert): mis-sized large
        // allocations must fail loudly rather than allocate or be rejected
        // only under DEBUG_BUILD. Lower bound is the smallest axis max (1KB).
        if (elem_size <= 1_KB)
        {
            throw std::invalid_argument(
                std::format("SlabProxy should only be used for large allocations, got {}", elem_size));
        }
        if (elem_size > kMaxAllocationBytes)
        {
            throw std::out_of_range(
                std::format("Requested size {} exceeds maximum allowed size for SlabProxy", elem_size));
        }

        std::byte* item = new(std::align_val_t{kProxyAlignment}) std::byte[elem_size];
        debug_println("Allocated {} bytes via SlabProxy (align={}), ptr={}",
                      elem_size, kProxyAlignment, static_cast<void*>(item));
        return item;
    }


    inline void SlabProxy::deallocateItem(std::byte* item)
    {
        debug_println("Deallocated item via SlabProxy, ptr={}", static_cast<void*>(item));

        ::operator delete[](item, std::align_val_t{kProxyAlignment});
    }


}; // namespace spallocator


#endif // SLAB_HPP_
