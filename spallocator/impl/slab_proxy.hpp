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

#ifndef SPALLOCATOR_IMPL_SLAB_PROXY_HPP_
#define SPALLOCATOR_IMPL_SLAB_PROXY_HPP_

#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <stdexcept>

#include "impl/helper.hpp"
#include "impl/islab.hpp"

namespace spallocator
{

    //
    // SlabProxy does not manage its own memory; it delegates to
    // standard memory allocation methods for large memory
    // allocations that aren't suitable for small-memory slab
    // optimizations
    //
    class SlabProxy: public ISlab
    {
    public: // methods
        AllocateItemResult allocateItem(std::size_t size) override;
        void deallocateItem(std::byte* item,
                            std::byte* block_base = nullptr,
                            std::uint16_t block_generation = 0) override;

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
    // SlabProxy
    // ---------------------------------------------------------------------------

    // Oversize / beyond-alignment-class-max path. Always uses 64-byte aligned new/delete
    // so any Pool request alignment ≤ 64 is satisfied.
    inline constexpr std::size_t kProxyAlignment = 64;

    inline AllocateItemResult SlabProxy::allocateItem(std::size_t elem_size)
    {
        // Active in all build types (not runtime_assert): mis-sized large
        // allocations must fail loudly rather than allocate or be rejected
        // only under DEBUG_BUILD. Lower bound guards misuse; real overflow
        // footprints exceed the alignment-class ladder max (see kLadder*).
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
        return AllocateItemResult{item, nullptr, 0};
    }


    inline void SlabProxy::deallocateItem(std::byte* item,
                                          std::byte* /*block_base*/,
                                          std::uint16_t /*block_generation*/)
    {
        debug_println("Deallocated item via SlabProxy, ptr={}", static_cast<void*>(item));

        ::operator delete[](item, std::align_val_t{kProxyAlignment});
    }

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_SLAB_PROXY_HPP_
