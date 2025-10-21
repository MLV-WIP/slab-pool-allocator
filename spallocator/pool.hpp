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
        std::byte* allocate(std::size_t size, std::size_t alignment = 8);
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

        SpinLock lock;
    };


    std::byte* Pool::allocate(std::size_t item_size, std::size_t alignment /* = 8 */)
    {
        Allocation alloc;
        alloc.size = item_size;

        if (item_size > 1_GB)
        {
            // excessively large allocations don't belong in the pool
            // allocator; use other allocation methods instead
            throw std::out_of_range("Allocation size exceeds maximum limit for pool allocator");
        }

        runtime_assert((alignment >= 4) && ((alignment & (alignment - 1)) == 0),
            "Alignment must be a power of two and at least 4 bytes");
        runtime_assert(alignment <= 16,
            "Alignment greater than 16 bytes is not supported");
        if (alignment < 4 || alignment > 16)
        {
            throw std::invalid_argument("Unsupported alignment requested");
        }
        // ... else the slab native buffer alignment is 16, which can cover
        // all the supported alignment requests

        // Store the actual allocated size and the alignment in bytes directly
        // before the data returned to the user. We may need to pad for
        // alignment, but the size will always be in the preceding 4 bytes,
        // and the alignment in the 1 byte preceding that. This allows us to
        // use the size for quick lookup during deallocation.
        std::size_t header_size = 8 < alignment ? alignment : 8;
        std::size_t alloc_size = item_size + header_size;
        alloc.size = alloc_size;

        AbstractSlab* slab = nullptr;
        {
            std::scoped_lock<SpinLock> guard(lock);
            auto slab_index = selectSlab(alloc_size);
            if (slab_index != std::numeric_limits<std::size_t>::max())
            {
                slab = small_slabs[slab_index].get();
            }
            else
            {
                slab = &large_slab;
            }
        }
        alloc.ptr = slab->allocateItem(alloc_size);
        //println("Allocated {} bytes at ptr={}, slab={}",
        //        alloc_size, static_cast<void*>(alloc.ptr),
        //        (slab == &large_slab) ? "large_slab" : "small_slab");

        uint32_t* size_ptr = reinterpret_cast<uint32_t*>(alloc.ptr + header_size - 4);
        *size_ptr = uint32_t(alloc_size & 0xffffffff);

        uint8_t* header_size_ptr = reinterpret_cast<uint8_t*>(alloc.ptr + header_size - 5);
        *header_size_ptr = uint8_t(header_size & 0xff);

        return alloc.ptr + header_size;
    }


    void Pool::deallocate(std::byte* item)
    {
        if (item == nullptr)
        {
            return;
        }

        uint32_t* size_ptr = reinterpret_cast<uint32_t*>(item - 4);
        std::size_t alloc_size = *size_ptr;

        uint8_t* header_size_ptr = reinterpret_cast<uint8_t*>(item - 5);
        uint8_t header_size = *header_size_ptr;

        std::byte* original_ptr = item - header_size;

        AbstractSlab* slab = nullptr;
        {
            std::scoped_lock<SpinLock> guard(lock);
            auto slab_index = selectSlab(alloc_size);
            if (slab_index != std::numeric_limits<std::size_t>::max())
            {
                slab = small_slabs[slab_index].get();
            }
            else
            {
                slab = &large_slab;
            }
        }
        //println("Deallocating {} bytes at ptr={}, slab_index={}",
        //        alloc_size, static_cast<void*>(original_ptr), slab_index);
        slab->deallocateItem(original_ptr);
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
