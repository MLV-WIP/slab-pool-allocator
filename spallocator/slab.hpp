/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2025, Michael VanLoon
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

#ifndef SLAP_HPP_
#define SLAP_HPP_

#include <bitset>
#include <cstddef>
#include <format>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "helper.hpp"


namespace spallocator
{

    template<std::size_t ElemSize>
    constexpr std::size_t selectBufferSize() {
        // for small element sizes, pre-allocate a buffer large enough to
        // hold multiple elements to reduce the number of allocations
        if constexpr (ElemSize < 1_KB) {
            return 4_KB;
        } else {
            return ElemSize * 4;
        }
    }


    class AbstractSlab
    {
    public: // methods
        virtual std::byte* allocateItem(std::size_t size) = 0;
        virtual void deallocateItem(std::byte* item) = 0;

        virtual ~AbstractSlab() = default;

    protected: // methods
        AbstractSlab() = default;

    private: // methods
        AbstractSlab(const AbstractSlab&) = delete;
        AbstractSlab& operator=(const AbstractSlab&) = delete;
        AbstractSlab(AbstractSlab&&) = delete;
        AbstractSlab& operator=(AbstractSlab&&) = delete;
    };


    template<const std::size_t ElemSize>
    class Slab: public AbstractSlab
    {
    public: // methods
        std::byte* allocateItem(std::size_t size);
        void deallocateItem(std::byte* item);

        constexpr std::size_t getElemSize() const { return ElemSize; }
        constexpr std::size_t getAllocSize() const { return slab_alloc_size; }
        const std::size_t getAllocatedMemory() const { return slab_data.size() * slab_alloc_size; }

        std::optional<std::size_t> findSlabForItem(std::byte* item) const;

        Slab();
        virtual ~Slab();

    private: // methods
        Slab(const Slab&) = delete;
        Slab& operator=(const Slab&) = delete;
        Slab(Slab&&) = delete;
        Slab& operator=(Slab&&) = delete;

        void allocateNewSlab();
    
    private: // data members
        static constexpr std::size_t slab_alloc_size{selectBufferSize<ElemSize>()};
        static_assert(slab_alloc_size >= 4_KB, "Allocation size must be greater than 4096 bytes");
        static_assert(
            (ElemSize > 2_KB) || (slab_alloc_size % 1_KB == 0),
            "Allocation size must be a multiple of 1024 when ElemSize <= 2KB");

        // subsequent allocations are a multiple of...
        static constexpr std::size_t alloc_multiplier{2};
        static_assert(alloc_multiplier >= 2, "Allocation multiplier must be at least 2");
        static_assert(alloc_multiplier % 2 == 0, "Allocation multiplier must be a multiple of 2");

        std::vector<std::byte*> slab_data;
        // bitsets could be optimized more by creating custom bitset class
        // with 64-bit chunks that can be compared atomically
        std::vector<std::bitset<slab_alloc_size / ElemSize>> slab_map;
        static constexpr std::size_t max_slabs{4_GB / slab_alloc_size};
        std::bitset<max_slabs> slab_available_map;

        std::map<std::byte*, std::size_t> base_address_map;

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
        std::byte* allocateItem(std::size_t size);
        void deallocateItem(std::byte* item);

        SlabProxy() = default;
        virtual ~SlabProxy() = default;

    private: // methods
        SlabProxy(const SlabProxy&) = delete;
        SlabProxy& operator=(const SlabProxy&) = delete;
        SlabProxy(SlabProxy&&) = delete;
        SlabProxy& operator=(SlabProxy&&) = delete;

    private: // data members
    };


    // Helper for debug output
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


    template<const std::size_t ElemSize>
    std::byte* Slab<ElemSize>::allocateItem(std::size_t size)
    {
        runtime_assert(size <= ElemSize,
            std::format("Requested size {} exceeds slab element size {}", size, ElemSize));

        std::scoped_lock<SpinLock> guard(slab_lock);

        // Find a free item in the slabs
        for (std::size_t slab_index = 0; slab_index <= slab_map.size(); ++slab_index)
        {
            if (!slab_available_map.test(slab_index))
            {
                // this slab is full
                continue;
            }

            while (slab_index >= slab_data.size())
            {
                // need to allocate a new slab
                allocateNewSlab();
                //println("New slab<{}> allocated, total slabs: {}/{}",
                //        ElemSize, slab_data.size(), slab_map.size());
            }

            auto& slab_slots = slab_map[slab_index];
            for (std::size_t item_index = 0; item_index < slab_slots.size(); ++item_index)
            {
                if (!slab_slots.test(item_index))
                {
                    // Found a free item
                    slab_slots.set(item_index);
                    if (slab_slots.all())
                    {
                        // this slab is now full
                        slab_available_map.reset(slab_index);
                    }
                    //println("Item allocated ({}/{}), slab_map<{}>: {}",
                    //        slab_index, item_index, ElemSize, printHex(slab_slots));
                    return slab_data[slab_index] + item_index * ElemSize;
                }
            }
        }

        throw std::out_of_range(std::format("Memory for {}-byte slab has been exhausted", ElemSize));
    }


    template<const std::size_t ElemSize>
    std::optional<std::size_t> Slab<ElemSize>::findSlabForItem(std::byte* item) const
    {
        auto it = base_address_map.upper_bound(item);
        if (it != base_address_map.begin())
        {
            --it;
            auto slab_start = it->first;
            auto slab_end = slab_start + slab_alloc_size;
            if (item >= slab_start && item < slab_end)
            {
                return it->second;
            }
            else
            {
                // given the pointer base ranges, it should be here, but it turns
                // out not to be within the begin-end range
                println("Item {} cannot be found in slab range {} - {}",
                        static_cast<void*>(item),
                        static_cast<void*>(slab_start),
                        static_cast<void*>(slab_end));
            }
        }
        return std::nullopt;
    }


    template<const std::size_t ElemSize>
    void Slab<ElemSize>::deallocateItem(std::byte* item)
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
            auto slab_end = slab_start + slab_alloc_size;

            // Calculate the item index within the slab
            std::size_t item_index = (item - slab_start) / ElemSize;
            auto& slab_slots = slab_map[slab_index];
            if (slab_slots.test(item_index))
            {
                // Free the item
                slab_slots.reset(item_index);
                // This slab now has free space
                slab_available_map.set(slab_index);
                //println("Item freed ({}/{}), slab_map: {}",
                //        slab_index, item_index, printHex(slab_slots));
                return;
            }
            else
            {
                throw std::invalid_argument("Item is already free");
            }
        }

        throw std::invalid_argument("Invalid item pointer; item not found in slab");
    }


    template<const std::size_t ElemSize>
    Slab<ElemSize>::Slab()
    {
        //println("Slab created with element size: {}, allocation size: {}, and multiplier: {}",
        //        getElemSize(), getAllocSize(), alloc_multiplier);

        // all slabs are initially available
        slab_available_map.set();

        allocateNewSlab();
    }

    template<const std::size_t ElemSize>
    Slab<ElemSize>::~Slab()
    {
        //println("Slab destroyed, freeing {} bytes of memory", getAllocatedMemory());
        for (auto ptr : slab_data) {
            delete[] ptr;
        }
    }

    template<const std::size_t ElemSize>
    void Slab<ElemSize>::allocateNewSlab()
    {
        if (slab_data.size() >= max_slabs)
        {
            throw std::out_of_range(
                std::format("Cannot allocate more than {} slabs of size {} bytes",
                            max_slabs, slab_alloc_size));
        }

        static_assert(ElemSize >= 16,
            "Element size must be at least 16 bytes");
        static_assert(ElemSize % 16 == 0,
            "Element size must be a multiple of 16 bytes");
    
        // allocate a new slab of memory
        std::byte* new_slab = new(std::align_val_t{16}) std::byte[slab_alloc_size];
        slab_data.push_back(new_slab);
        base_address_map[new_slab] = slab_data.size() - 1;
        slab_map.emplace_back();
    }


    inline std::byte* SlabProxy::allocateItem(std::size_t elem_size)
    {
        runtime_assert(elem_size > 1_KB,
            std::format("SlabProxy should only be used for large allocations, got {}", elem_size));
        runtime_assert(elem_size <= 1_GB,
            std::format("Requested size {} exceeds maximum allowed size for SlabProxy", elem_size));

        // allocate memory using standard methods
        std::byte* item = new(std::align_val_t{16}) std::byte[elem_size];
        //println("Allocated {} bytes via SlabProxy, ptr={}",
        //        elem_size, static_cast<void*>(item));
        return item;
    }


    inline void SlabProxy::deallocateItem(std::byte* item)
    {
        // deallocate memory using standard methods
        //println("Deallocated item via SlabProxy, ptr={}", static_cast<void*>(item));
        delete[] item;
    }


}; // namespace spallocator


#endif // SLAP_HPP_
