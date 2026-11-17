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

#ifndef SPALLOCATOR_HPP_
#define SPALLOCATOR_HPP_

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <ranges>
#include <algorithm>
#include <stdexcept>

#include "helper.hpp"
#include "slab.hpp"
#include "pool.hpp"


namespace spallocator
{
    // noexcept free for unique_ptr / allocator paths. Invalid pointers are
    // programming errors; abort rather than throw through a noexcept boundary.
    // No-op if the pool is already dead (arena bulk reclaim).
    inline void pool_deallocate_noexcept(PoolBlockToken& token, std::byte* p) noexcept
    {
        Pool* pool = token.get();
        if (!pool)
        {
            return; // !alive — arena already reclaimed storage
        }
        if (!pool->try_deallocate(p))
        {
            std::abort();
        }
    }

    inline void pool_deallocate_noexcept(Pool& pool, std::byte* p) noexcept
    {
        PoolBlockToken token = pool.block_token();
        pool_deallocate_noexcept(token, p);
    }

    // Custom deleter for unique_ptr that uses Pool::try_deallocate.
    // Holds a PoolBlockToken so arena ~Pool can mark dead and this no-ops
    // (skips ~T and free — documented bulk-reclaim model).
    template<typename T>
    struct PoolDeleter
    {
        PoolBlockToken token;

        explicit PoolDeleter(PoolBlockToken t) noexcept : token(std::move(t)) {}
        explicit PoolDeleter(Pool& pool) noexcept : token(pool.block_token()) {}
        explicit PoolDeleter(PoolHandle const& handle) noexcept
            : token(handle.block_token())
        {
        }

        void operator()(T* p) const noexcept
        {
            if (!p)
            {
                return;
            }
            if (!token.alive())
            {
                return; // arena: skip ~T and free
            }
            // Explicit dtor: storage came from placement new via make_pool_unique.
            p->~T();
            Pool* pool = token.get();
            if (!pool || !pool->try_deallocate(reinterpret_cast<std::byte*>(p)))
            {
                std::abort();
            }
        }
    };

    // Specialization for array types
    template<typename T>
    struct PoolDeleter<T[]>
    {
        PoolBlockToken token;
        const std::size_t size;

        PoolDeleter(PoolBlockToken t, std::size_t n) noexcept
            : token(std::move(t)), size(n)
        {
        }
        PoolDeleter(Pool& pool, std::size_t n) noexcept
            : token(pool.block_token()), size(n)
        {
        }
        PoolDeleter(PoolHandle const& handle, std::size_t n) noexcept
            : token(handle.block_token()), size(n)
        {
        }

        void operator()(T* p) const noexcept  // Note: T*, not T*[]
        {
            if (!p)
            {
                return;
            }
            if (!token.alive())
            {
                return; // arena: skip element dtors and free
            }
            // Destroy in reverse construction order (same as new[] / unique_ptr).
            std::ranges::for_each(std::views::counted(p, size) | std::views::reverse,
                                  [](T& elem){ elem.~T(); });
            Pool* pool = token.get();
            if (!pool || !pool->try_deallocate(reinterpret_cast<std::byte*>(p)))
            {
                std::abort();
            }
        }
    };

    // Type alias for unique_ptr with pool-based deallocation
    template<typename T>
    using unique_pool_ptr = std::unique_ptr<T, PoolDeleter<T>>;

    namespace detail
    {
        inline Pool* require_live_pool(PoolBlockToken const& token)
        {
            Pool* pool = token.get();
            if (!pool)
            {
                throw std::logic_error(
                    "pool factory: Pool is not alive (destroyed or empty handle)");
            }
            return pool;
        }
    }

    // Single object: placement new; on ctor throw, return the slot (strong guarantee).
    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    constexpr unique_pool_ptr<T> make_pool_unique(PoolBlockToken token, Args&&... args)
    {
        static_assert(alignof(T) <= kMaxAlignment,
            "make_pool_unique: alignof(T) exceeds Pool max alignment (64)");
        Pool* pool = detail::require_live_pool(token);
        void* mem = pool->allocate(sizeof(T), alignof(T));
        try
        {
            T* obj = new (mem) T(std::forward<Args>(args)...);
            return unique_pool_ptr<T>(obj, PoolDeleter<T>{std::move(token)});
        }
        catch (...)
        {
            // Constructor failed: object was never constructed; return the slot.
            pool->deallocate(reinterpret_cast<std::byte*>(mem));
            throw;
        }
    }

    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    constexpr unique_pool_ptr<T> make_pool_unique(Pool& pool, Args&&... args)
    {
        return make_pool_unique<T>(pool.block_token(), std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    constexpr unique_pool_ptr<T> make_pool_unique(PoolHandle const& handle, Args&&... args)
    {
        return make_pool_unique<T>(handle.block_token(), std::forward<Args>(args)...);
    }

    // Unbounded array: Pool::allocate returns the user pointer (header already
    // written). Strong exception guarantee if an element ctor throws.
    template<typename T>
        requires std::is_unbounded_array_v<T>
    constexpr unique_pool_ptr<T> make_pool_unique(PoolBlockToken token, std::size_t size)
    {
        using ElementType = std::remove_extent_t<T>;
        static_assert(alignof(ElementType) <= kMaxAlignment,
            "make_pool_unique: alignof(element) exceeds Pool max alignment (64)");

        Pool* pool = detail::require_live_pool(token);
        std::size_t array_size = sizeof(ElementType) * size;
        std::size_t alignment = alignof(ElementType) > alignof(std::size_t) ?
                                alignof(ElementType) : alignof(std::size_t);

        std::byte* mem = pool->allocate(array_size, alignment);
        ElementType* array_ptr = reinterpret_cast<ElementType*>(mem);

        std::size_t constructed = 0;
        try
        {
            for (; constructed < size; ++constructed)
            {
                new (&array_ptr[constructed]) ElementType();
            }
        }
        catch (...)
        {
            while (constructed > 0)
            {
                array_ptr[--constructed].~ElementType();
            }
            pool->deallocate(mem);
            throw;
        }

        return unique_pool_ptr<T>(array_ptr, PoolDeleter<T>{std::move(token), size});
    }

    template<typename T>
        requires std::is_unbounded_array_v<T>
    constexpr unique_pool_ptr<T> make_pool_unique(Pool& pool, std::size_t size)
    {
        return make_pool_unique<T>(pool.block_token(), size);
    }

    template<typename T>
        requires std::is_unbounded_array_v<T>
    constexpr unique_pool_ptr<T> make_pool_unique(PoolHandle const& handle, std::size_t size)
    {
        return make_pool_unique<T>(handle.block_token(), size);
    }

    // Known bound array version (deleted - use std::array instead)
    template<typename T, typename... Args>
        requires std::is_bounded_array_v<T>
    void make_pool_unique(Pool& pool, Args&&... args) = delete;

    template<typename T, typename... Args>
        requires std::is_bounded_array_v<T>
    void make_pool_unique(PoolHandle const& handle, Args&&... args) = delete;


    // =========================================================================
    // PoolAllocator - C++ Standard Allocator using Pool
    // =========================================================================

    // C++17-compliant allocator for STL containers and allocate_shared.
    // Design decisions (see also IMPLEMENTATION.md — Smart pointers):
    // 1. Holds PoolBlockToken (block_refs) — survives arena ~Pool; not handle_refs
    // 2. Stateful — unequal when control blocks differ
    // 3. Alignment-aware via Pool::allocate(bytes, alignof(T))
    // 4. POCCA/POCMA/POCS false — token is not transferred by container assign
    // 5. Equal iff same PoolControlBlock*; allocate throws if !alive; free no-ops if dead

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
        using is_always_equal = std::false_type;

        static_assert(alignof(T) <= kMaxAlignment,
            "PoolAllocator: alignof(T) exceeds Pool max alignment (64)");

        explicit PoolAllocator(PoolBlockToken token) noexcept
            : token_(std::move(token))
        {
        }
        explicit PoolAllocator(Pool& pool) noexcept
            : token_(pool.block_token())
        {
        }
        explicit PoolAllocator(PoolHandle const& handle) noexcept
            : token_(handle.block_token())
        {
        }

        PoolAllocator(const PoolAllocator&) noexcept = default;

        template<typename U>
        PoolAllocator(const PoolAllocator<U>& other) noexcept
            : token_(other.token_)
        {
        }

        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            {
                throw std::bad_array_new_length();
            }

            Pool* pool = token_.get();
            if (!pool)
            {
                throw std::bad_alloc();
            }

            std::size_t bytes = n * sizeof(T);
            std::byte* mem = pool->allocate(bytes, alignof(T));
            return reinterpret_cast<T*>(mem);
        }

        // Must not throw ([allocator.requirements]); bad pointers abort while
        // alive; dead pool → no-op (arena).
        void deallocate(T* p, std::size_t n) noexcept
        {
            (void)n;  // Size not needed for pool deallocation
            if (p)
            {
                pool_deallocate_noexcept(token_, reinterpret_cast<std::byte*>(p));
            }
        }

        template<typename U>
        friend bool operator==(const PoolAllocator& lhs, const PoolAllocator<U>& rhs) noexcept
        {
            return lhs.token_.control() == rhs.token_.control();
        }

        template<typename U>
        friend bool operator!=(const PoolAllocator& lhs, const PoolAllocator<U>& rhs) noexcept
        {
            return !(lhs == rhs);
        }

        template<typename U>
        friend class PoolAllocator;

        PoolBlockToken const& token() const noexcept { return token_; }

    private:
        PoolBlockToken token_;
    };

    // =========================================================================
    // make_pool_shared — allocate_shared + PoolAllocator (object + control block)
    // =========================================================================

    // Single object: like std::make_shared, but storage comes from the pool.
    // Benefits vs make_pool_unique: shared ownership, weak_ptr, one allocation
    // for object + shared_ptr control block.
    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    std::shared_ptr<T> make_pool_shared(PoolBlockToken token, Args&&... args)
    {
        PoolAllocator<T> alloc(std::move(token));
        return std::allocate_shared<T>(alloc, std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    std::shared_ptr<T> make_pool_shared(Pool& pool, Args&&... args)
    {
        return make_pool_shared<T>(pool.block_token(), std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
        requires (!std::is_array_v<T>)
    std::shared_ptr<T> make_pool_shared(PoolHandle const& handle, Args&&... args)
    {
        return make_pool_shared<T>(handle.block_token(), std::forward<Args>(args)...);
    }

    // Unbounded array (C++20): shared_ptr tracks length in its control block
    // (unlike make_pool_unique<T[]>, which stores size in PoolDeleter).
    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(PoolBlockToken token, std::size_t size)
    {
        using ElementType = std::remove_extent_t<T>;
        PoolAllocator<ElementType> alloc(std::move(token));
        return std::allocate_shared<T>(alloc, size);
    }

    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(Pool& pool, std::size_t size)
    {
        return make_pool_shared<T>(pool.block_token(), size);
    }

    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(PoolHandle const& handle, std::size_t size)
    {
        return make_pool_shared<T>(handle.block_token(), size);
    }

    // Unbounded array with fill value (C++20).
    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(PoolBlockToken token, std::size_t size,
                                        const std::remove_extent_t<T>& init_value)
    {
        using ElementType = std::remove_extent_t<T>;
        PoolAllocator<ElementType> alloc(std::move(token));
        return std::allocate_shared<T>(alloc, size, init_value);
    }

    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(Pool& pool, std::size_t size,
                                        const std::remove_extent_t<T>& init_value)
    {
        return make_pool_shared<T>(pool.block_token(), size, init_value);
    }

    template<typename T>
        requires std::is_unbounded_array_v<T>
    std::shared_ptr<T> make_pool_shared(PoolHandle const& handle, std::size_t size,
                                        const std::remove_extent_t<T>& init_value)
    {
        return make_pool_shared<T>(handle.block_token(), size, init_value);
    }

    template<typename T, typename... Args>
        requires std::is_bounded_array_v<T>
    void make_pool_shared(Pool& pool, Args&&... args) = delete;

}; // namespace spallocator


#endif // SPALLOCATOR_HPP_
