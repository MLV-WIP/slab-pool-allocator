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
#include <limits>
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

        const std::size_t size;

        void operator()(T* p) const  // Note: T*, not T*[]
        {
            if (p)
            {
                // Call destructors in reverse order
                std::ranges::for_each(std::views::counted(p, size) | std::views::reverse,
                                      [](T& elem){ elem.~T(); });
                pool.deallocate(reinterpret_cast<std::byte*>(p));
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

        std::size_t array_size = sizeof(ElementType) * size;
        std::size_t alignment = alignof(ElementType) > alignof(std::size_t) ?
                                alignof(ElementType) : alignof(std::size_t);

        // Allocate memory for header + array
        std::byte* mem = pool.allocate(array_size, alignment);
        ElementType* array_ptr = reinterpret_cast<ElementType*>(mem);

        // Use placement new to construct each element
        for (std::size_t i = 0; i < size; ++i)
        {
            new (&array_ptr[i]) ElementType();
        }

        return unique_pool_ptr<T>(array_ptr, PoolDeleter<T>{pool, size});
    }

    // Known bound array version (deleted - use std::array instead)
    template<typename T, typename... Args>
        requires std::is_bounded_array_v<T>
    void make_pool_unique(spallocator::Pool& pool, Args&&... args) = delete;


    // =========================================================================
    // PoolAllocator - C++ Standard Allocator using Pool
    // =========================================================================

    // PoolAllocator is a C++17-compliant allocator that uses spallocator::Pool
    // for memory allocation. This enables integration with std::shared_ptr and
    // other STL components that accept custom allocators.
    //
    // Key design decisions:
    // 1. Stores a reference to the pool (not owned) - pool must outlive allocator
    // 2. Stateful allocator - different instances with different pools compare unequal
    // 3. Uses pool's alignment-aware allocation for proper object placement
    // 4. Propagates on container copy/move/swap (POCCA/POCMA/POCS all false)
    //    because the pool is not owned and must not transfer

    template<typename T>
    class PoolAllocator
    {
    public:
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using propagate_on_container_copy_assignment = std::false_type;
        using propagate_on_container_move_assignment = std::false_type;
        using propagate_on_container_swap = std::false_type;
        using is_always_equal = std::false_type;  // Different pools = different allocators

        // Constructor - requires a pool reference
        explicit PoolAllocator(spallocator::Pool& pool) noexcept : pool_ref(pool) {}

        // Copy constructor
        PoolAllocator(const PoolAllocator&) noexcept = default;

        // Rebind constructor - allows allocating different types with same pool
        template<typename U>
        PoolAllocator(const PoolAllocator<U>& other) noexcept : pool_ref(other.pool_ref) {}

        // Allocate n objects of type T
        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            {
                throw std::bad_array_new_length();
            }

            std::size_t bytes = n * sizeof(T);
            std::size_t alignment = alignof(T);

            std::byte* mem = pool_ref.allocate(bytes, alignment);
            return reinterpret_cast<T*>(mem);
        }

        // Deallocate memory
        void deallocate(T* p, std::size_t n) noexcept
        {
            (void)n;  // Size not needed for pool deallocation
            pool_ref.deallocate(reinterpret_cast<std::byte*>(p));
        }

        // Equality comparison - allocators are equal only if they use the same pool
        template<typename U>
        friend bool operator==(const PoolAllocator& lhs, const PoolAllocator<U>& rhs) noexcept
        {
            return &lhs.pool_ref == &rhs.pool_ref;
        }

        template<typename U>
        friend bool operator!=(const PoolAllocator& lhs, const PoolAllocator<U>& rhs) noexcept
        {
            return !(lhs == rhs);
        }

        // Public access to pool for rebind constructor
        template<typename U>
        friend class PoolAllocator;

    private:
        spallocator::Pool& pool_ref;
    };

    // =========================================================================
    // make_pool_shared - Factory functions for std::shared_ptr with Pool
    // =========================================================================

    // Single object version
    // Uses std::allocate_shared with PoolAllocator to create a shared_ptr
    // that allocates both the object and control block from the pool.
    //
    // Example:
    //   auto obj = make_pool_shared<MyClass>(pool, arg1, arg2);
    //
    // Benefits over make_pool_unique:
    // - Reference counted sharing
    // - Thread-safe reference counting
    // - Weak pointer support
    // - Single allocation for object + control block (like std::make_shared)

    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    std::shared_ptr<T> make_pool_shared(spallocator::Pool& pool, Args&&... args)
    {
        PoolAllocator<T> alloc(pool);
        return std::allocate_shared<T>(alloc, std::forward<Args>(args)...);
    }

    // Unbounded array version (C++20)
    // Creates a shared_ptr to an array with default-initialized elements.
    //
    // Example:
    //   auto arr = make_pool_shared<int[]>(pool, 100);
    //
    // Note: Unlike make_pool_unique which stores size in a header,
    // std::shared_ptr<T[]> tracks array size internally in the control block.
    // This means we don't need custom size tracking.

    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(spallocator::Pool& pool, std::size_t size)
    {
        using ElementType = std::remove_extent_t<T>;
        PoolAllocator<ElementType> alloc(pool);
        return std::allocate_shared<T>(alloc, size);
    }

    // Unbounded array version with initialization value (C++20)
    // Creates a shared_ptr to an array with all elements initialized to a value.
    //
    // Example:
    //   auto arr = make_pool_shared<int[]>(pool, 100, 42);  // 100 ints, all = 42

    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(spallocator::Pool& pool, std::size_t size,
                                        const std::remove_extent_t<T>& init_value)
    {
        using ElementType = std::remove_extent_t<T>;
        PoolAllocator<ElementType> alloc(pool);
        return std::allocate_shared<T>(alloc, size, init_value);
    }

    // Known bound array version (deleted - use std::array instead)
    template<typename T, typename... Args>
        requires std::is_bounded_array_v<T>
    void make_pool_shared(spallocator::Pool& pool, Args&&... args) = delete;

}; // namespace spallocator


#endif // SPALLOCATOR_HPP_
