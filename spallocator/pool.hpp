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

#ifndef POOL_HPP_
#define POOL_HPP_

#include "slab.hpp"


namespace spallocator
{

    struct Allocation
    {
        std::byte* ptr;
        std::size_t size;        
    };


    class Pool
    {
    public: // methods
        std::byte* allocate(std::size_t size);
        void deallocate(std::byte* item);

        Pool();
        ~Pool() = default;

        constexpr::size_t selectSlab(std::size_t size) const;

    private: // methods
        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;
        Pool(Pool&&) = delete;
        Pool& operator=(Pool&&) = delete;

    private: // data members
        std::vector<std::unique_ptr<AbstractSlab>> small_slabs;
        SlabProxy large_slab;
    };


    std::byte* Pool::allocate(std::size_t item_size)
    {
        Allocation alloc;
        alloc.size = item_size;

        if (item_size > 1_GB)
        {
            // excessively large allocations don't belong in the pool
            // allocator; use other allocation methods instead
            throw std::out_of_range("Allocation size exceeds maximum limit for pool allocator");
        }

        // store the actual allocated size just before the returned pointer
        // for quick lookup of allocator during deallocation
        std::size_t alloc_size = item_size + 4;
        alloc.size = alloc_size;

        auto slab_index = selectSlab(alloc_size);
        if (slab_index != std::numeric_limits<std::size_t>::max())
        {
            alloc.ptr = small_slabs[slab_index]->allocateItem(alloc_size);
            println("Allocated {} ({}) bytes from small slab {}, ptr={}",
                    alloc_size, item_size, slab_index, static_cast<void*>(alloc.ptr));
        }
        else
        {
            alloc.ptr = large_slab.allocateItem(alloc_size);
            println("Allocated {} ({}) bytes from large slab, ptr={}",
                    alloc_size, item_size, static_cast<void*>(alloc.ptr));
        }
        uint32_t* size_ptr = reinterpret_cast<uint32_t*>(alloc.ptr);
        *size_ptr = uint32_t(alloc_size & 0xFFFF);

        return alloc.ptr + 4;
    }


    void Pool::deallocate(std::byte* item)
    {
        if (item == nullptr)
        {
            return;
        }

        std::byte* original_ptr = item - 4;
        uint32_t* size_ptr = reinterpret_cast<uint32_t*>(original_ptr);
        std::size_t alloc_size = *size_ptr;

        auto slab_index = selectSlab(alloc_size);
        println("Deallocating {} bytes at ptr={}, slab_index={}",
                alloc_size, static_cast<void*>(original_ptr), slab_index);
        if (slab_index != std::numeric_limits<std::size_t>::max())
        {
            small_slabs[slab_index]->deallocateItem(original_ptr);
        }
        else
        {
            large_slab.deallocateItem(original_ptr);
        }
    }


    constexpr std::size_t Pool::selectSlab(std::size_t size) const
    {
        if (size <= 16) return 0;
        else if (size <= 32) return 1;
        else if (size <= 48) return 2;
        else if (size <= 64) return 3;
        else if (size <= 96) return 4;
        else if (size <= 128) return 5;
        else if (size <= 192) return 6;
        else if (size <= 256) return 7;
        else if (size <= 384) return 8;
        else if (size <= 512) return 9;
        else if (size <= 768) return 10;
        else if (size <= 1_KB) return 11;
        else return std::numeric_limits<std::size_t>::max(); // indicates large slab
    }

    Pool::Pool()
    {
        // create slabs for small sizes (up to 1KB)
        small_slabs.push_back(std::make_unique<Slab<16>>());   // 0
        small_slabs.push_back(std::make_unique<Slab<32>>());   // 1
        small_slabs.push_back(std::make_unique<Slab<48>>());   // 2
        small_slabs.push_back(std::make_unique<Slab<64>>());   // 3
        small_slabs.push_back(std::make_unique<Slab<96>>());   // 4
        small_slabs.push_back(std::make_unique<Slab<128>>());  // 5
        small_slabs.push_back(std::make_unique<Slab<192>>());  // 6
        small_slabs.push_back(std::make_unique<Slab<256>>());  // 7
        small_slabs.push_back(std::make_unique<Slab<384>>());  // 8
        small_slabs.push_back(std::make_unique<Slab<512>>());  // 9
        small_slabs.push_back(std::make_unique<Slab<768>>());  // 10
        small_slabs.push_back(std::make_unique<Slab<1_KB>>()); // 11
    }

}; // namespace spallocator


#endif // POOL_HPP_
