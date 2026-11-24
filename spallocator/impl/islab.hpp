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
#include <tuple>

#include "impl/block_registry.hpp"

namespace spallocator
{

    class Pool;

    // ptr, ElemSize (0 for SlabProxy), Block generation (0 for SlabProxy).
    using AllocateItemResult = std::tuple<std::byte*, std::size_t, std::uint16_t>;

    class ISlab
    {
    public: // methods
        // Size-class Slab returns ElemSize + Block generation so Pool can write
        // the end tag without a Block-map lookup on allocate. SlabProxy returns
        // {ptr, 0, 0}.
        virtual AllocateItemResult allocateItem(std::size_t size) = 0;

        // Optional block_base: when non-null (Pool free path), Slab skips the
        // address-map range search and uses an exact-base lookup instead.
        virtual void deallocateItem(std::byte* item, std::byte* block_base = nullptr) = 0;

        // Release fully empty Blocks back to the OS. Default: no-op
        // (SlabProxy). Returns the number of Blocks deleted.
        virtual std::size_t reclaimEmptyBlocks(std::size_t keep_min = 1);
        virtual std::size_t getAllocatedMemory() const { return 0; }

        // Pool installs its Block registry after constructing size-class engines.
        // Default no-op (SlabProxy). Slab registers any already-published Blocks.
        virtual void set_block_registry(BlockRegistry* /*registry*/) {}

        // Pool identity for M4-B Block headers (cookie + back-pointer).
        virtual void set_pool_context(Pool* /*pool*/, std::uint64_t /*cookie*/) {}

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
