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

#ifndef SPALLOCATOR_HPP_
#define SPALLOCATOR_HPP_

#include <cstddef>
#include <memory>
#include <ranges>

#include "helper.hpp"
#include "slab.hpp"
#include "pool.hpp"


namespace spallocator
{
    // Custom deleter for unique_ptr that uses Pool::deallocate
    template<typename T>
    struct PoolDeleter
    {
        spallocator::Pool& pool;

        void operator()(T* p) const
        {
            if (p)
            {
                p->~T();  // Explicitly call destructor since we use placement new
                pool.deallocate(reinterpret_cast<std::byte*>(p));
            }
        }
    };

    // Specialization for array types
    template<typename T>
    struct PoolDeleter<T[]>
    {
        spallocator::Pool& pool;

        void operator()(T* p) const  // Note: T*, not T*[]
        {
            if (p)
            {
                // Get the size header stored before the array
                std::byte* allocation = reinterpret_cast<std::byte*>(p);
                std::byte* header_ptr = allocation - sizeof(std::size_t);
                runtime_assert(
                    reinterpret_cast<std::uintptr_t>(header_ptr) % alignof(std::size_t) == 0,
                    std::format("Header pointer {} is not properly aligned for size_t", (void*)header_ptr));
                std::size_t size = *reinterpret_cast<std::size_t*>(header_ptr);

                // Call destructors in reverse order
                std::ranges::for_each(std::views::counted(p, size) | std::views::reverse,
                                      [](T& elem){ elem.~T(); });

                // Deallocate from the start of the header
                pool.deallocate(header_ptr);
            }
        }
    };

    // Type alias for unique_ptr with pool-based deallocation
    template<typename T>
    using unique_pool_ptr = std::unique_ptr<T, PoolDeleter<T>>;

    // Single object version
    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    constexpr unique_pool_ptr<T> make_pool_unique(spallocator::Pool& pool, Args&&... args)
    {
        void* mem = pool.allocate(sizeof(T), alignof(T));
        T* obj = new (mem) T(std::forward<Args>(args)...);
        return unique_pool_ptr<T>(obj, PoolDeleter<T>{pool});
    }

    // Unknown bound array version
    template<typename T>
        requires std::is_unbounded_array_v<T>
    constexpr unique_pool_ptr<T> make_pool_unique(spallocator::Pool& pool, std::size_t size)
    {
        using ElementType = std::remove_extent_t<T>;

        // Calculate total size: size header + array
        std::size_t header_size = sizeof(std::size_t);
        std::size_t array_size = sizeof(ElementType) * size;
        std::size_t alignment = alignof(ElementType) > alignof(std::size_t) ?
                                alignof(ElementType) : alignof(std::size_t);

        // Allocate memory for header + array
        std::byte* mem = pool.allocate(header_size + array_size, alignment);

        // Store the size in the header
        std::size_t* size_ptr = reinterpret_cast<std::size_t*>(mem);
        runtime_assert(
            reinterpret_cast<std::uintptr_t>(size_ptr) % alignof(std::size_t) == 0,
            std::format("Header pointer {} is not properly aligned for size_t", (void*)size_ptr));
        *size_ptr = size;

        // Get pointer to array location (after header)
        ElementType* array_ptr = reinterpret_cast<ElementType*>(mem + header_size);
        runtime_assert(
            reinterpret_cast<std::uintptr_t>(array_ptr) % alignment == 0,
            std::format("Array pointer {} is not properly aligned for {}", (void*)array_ptr, alignment));

        // Use placement new to construct each element
        for (std::size_t i = 0; i < size; ++i)
        {
            new (&array_ptr[i]) ElementType();
        }

        return unique_pool_ptr<T>(array_ptr, PoolDeleter<T>{pool});
    }

    // Known bound array version (deleted - use std::array instead)
    template<typename T, typename... Args>
        requires std::is_bounded_array_v<T>
    void make_pool_unique(spallocator::Pool& pool, Args&&... args) = delete;

}; // namespace spallocator


#endif // SPALLOCATOR_HPP_
