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

#ifndef SPALLOCATOR_IMPL_ISLAB_HPP_
#define SPALLOCATOR_IMPL_ISLAB_HPP_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace spallocator
{

    // Result of allocateItem. For Slab, block_base/generation identify the
    // owning Block so Pool can stash routing hints in the front header.
    // SlabProxy leaves block_base null and generation 0.
    struct AllocateItemResult
    {
        std::byte* ptr = nullptr;
        std::byte* block_base = nullptr;
        std::uint16_t block_generation = 0;

        // Convenience for call sites that only need the slot / allocation base.
        operator std::byte*() const noexcept { return ptr; }
    };

    class ISlab
    {
    public: // methods
        virtual AllocateItemResult allocateItem(std::size_t size) = 0;

        // Optional block_base + generation: when block_base is non-null (Pool
        // free with header hints), Slab verifies generation and uses an
        // exact-base lookup instead of an address-range search.
        virtual void deallocateItem(std::byte* item,
                                    std::byte* block_base = nullptr,
                                    std::uint16_t block_generation = 0) = 0;

        // Release fully empty Blocks back to the OS. Default: no-op
        // (SlabProxy). Returns the number of Blocks deleted.
        virtual std::size_t reclaimEmptyBlocks(std::size_t keep_min = 1);
        virtual std::size_t getAllocatedMemory() const { return 0; }

        // Read-only concurrency contract metadata for this backend.
        virtual bool isThreadSafe() const noexcept = 0;
        virtual std::string_view lockPolicyName() const noexcept = 0;

        virtual ~ISlab() = default;

    protected: // methods
        ISlab() = default;

    private: // methods
        ISlab(const ISlab&) = delete;
        ISlab& operator=(const ISlab&) = delete;
        ISlab(ISlab&&) = delete;
        ISlab& operator=(ISlab&&) = delete;
    };

    // ---------------------------------------------------------------------------
    // ISlab
    // ---------------------------------------------------------------------------

    inline std::size_t ISlab::reclaimEmptyBlocks(std::size_t /*keep_min*/)
    {
        return 0;
    }

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_ISLAB_HPP_
