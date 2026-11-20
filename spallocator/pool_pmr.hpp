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

#ifndef SPALLOCATOR_POOL_PMR_HPP_
#define SPALLOCATOR_POOL_PMR_HPP_

#include <cstddef>
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <utility>

#include "pool_lifetime.hpp"


namespace spallocator
{
    // std::pmr::memory_resource backed by a Pool domain.
    //
    // Additive header (not pulled by spallocator.hpp) so non-PMR clients avoid
    // <memory_resource>. Prefer PoolAllocator<T> when the container type can
    // bake the allocator in (no virtual dispatch). Use this for polymorphic
    // pmr containers / composition.
    //
    // Holds a PoolBlockToken (block_refs) — same arena model as PoolAllocator:
    // allocate throws bad_alloc if !alive; deallocate no-ops if dead.
    class PoolMemoryResource : public std::pmr::memory_resource
    {
    public: // methods
        explicit PoolMemoryResource(PoolBlockToken token) noexcept;
        explicit PoolMemoryResource(Pool& pool) noexcept;
        explicit PoolMemoryResource(PoolHandle const& handle) noexcept;

        PoolBlockToken const& token() const noexcept { return token_; }

    protected: // methods
        void* do_allocate(std::size_t bytes, std::size_t alignment) override;
        void do_deallocate(void* p, std::size_t bytes,
                           std::size_t alignment) noexcept override;
        bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override;

    private: // data members
        PoolBlockToken token_;
    };

} // namespace spallocator


// Pool::allocate / block_token / alignment constants (Pool was forward-declared).
#include "pool.hpp"


namespace spallocator
{
    // ---------------------------------------------------------------------------
    // PoolMemoryResource
    // ---------------------------------------------------------------------------

    inline PoolMemoryResource::PoolMemoryResource(PoolBlockToken token) noexcept
        : token_(std::move(token))
    {
    }


    inline PoolMemoryResource::PoolMemoryResource(Pool& pool) noexcept
        : token_(pool.block_token())
    {
    }


    inline PoolMemoryResource::PoolMemoryResource(PoolHandle const& handle) noexcept
        : token_(handle.block_token())
    {
    }


    inline void* PoolMemoryResource::do_allocate(std::size_t bytes,
                                                 std::size_t alignment)
    {
        Pool* pool = token_.get();
        if (!pool)
        {
            throw std::bad_alloc();
        }

        // PMR may pass alignment 1; Pool's floor is kMinAlignment (4).
        // Round up when below the floor; reject non-power-of-two / oversize.
        std::size_t align = alignment < kMinAlignment ? kMinAlignment : alignment;
        if (align > kMaxAlignment || (align & (align - 1)) != 0)
        {
            throw std::bad_alloc();
        }

        try
        {
            return pool->allocate(bytes, align);
        }
        catch (std::bad_alloc const&)
        {
            throw;
        }
        catch (...)
        {
            // Pool uses invalid_argument / out_of_range for contract fails;
            // memory_resource::allocate must surface bad_alloc.
            throw std::bad_alloc();
        }
    }


    inline void PoolMemoryResource::do_deallocate(void* p, std::size_t,
                                                  std::size_t) noexcept
    {
        if (!p)
        {
            return;
        }

        // Same policy as pool_deallocate_noexcept (avoid pulling spallocator.hpp).
        Pool* pool = token_.get();
        if (!pool)
        {
            return; // !alive — arena already reclaimed storage
        }
        if (!pool->try_deallocate(static_cast<std::byte*>(p)))
        {
            std::abort();
        }
    }


    inline bool PoolMemoryResource::do_is_equal(
        std::pmr::memory_resource const& other) const noexcept
    {
        auto const* o = dynamic_cast<PoolMemoryResource const*>(&other);
        if (!o)
        {
            return false;
        }
        return token_.control() == o->token_.control();
    }

} // namespace spallocator

#endif // SPALLOCATOR_POOL_PMR_HPP_
