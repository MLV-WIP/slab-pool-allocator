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

#ifndef SPALLOCATOR_IMPL_BLOCK_REGISTRY_HPP_
#define SPALLOCATOR_IMPL_BLOCK_REGISTRY_HPP_

#include <cstddef>
#include <cstdint>

namespace spallocator
{

    class ISlab;

    // Pool Block ownership record for size-class free routing. Maintained on
    // grow/trim; free looks up by user pointer (exact slot base).
    struct BlockEntry
    {
        ISlab* slab{nullptr};
        std::size_t elem_size{0};
        std::size_t block_bytes{0};
        std::uint16_t generation{0};
    };

    // Observer installed on size-class Slabs. Grow/trim call these at the
    // staged publish / unpublish points (see docs/design/slot-end-tag.md).
    class BlockRegistry
    {
    public:
        virtual ~BlockRegistry() = default;

        // Map-exclusive insert: Block is not yet allocatable in the Slab.
        virtual void on_block_born(std::byte* base, const BlockEntry& entry) = 0;

        // Map-exclusive erase: must run before delete[] of base.
        virtual void on_block_died(std::byte* base) = 0;

    protected:
        BlockRegistry() = default;

    private:
        BlockRegistry(const BlockRegistry&) = delete;
        BlockRegistry& operator=(const BlockRegistry&) = delete;
        BlockRegistry(BlockRegistry&&) = delete;
        BlockRegistry& operator=(BlockRegistry&&) = delete;
    };

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_BLOCK_REGISTRY_HPP_
