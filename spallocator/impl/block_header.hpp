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

#ifndef SPALLOCATOR_IMPL_BLOCK_HEADER_HPP_
#define SPALLOCATOR_IMPL_BLOCK_HEADER_HPP_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

namespace spallocator
{

    class ISlab;
    class Pool;

    // Per-Block header at the allocation base (M4-B). Slots begin after this
    // region; user pointers remain data-first within each slot (end tag only).
    //
    // Free uses span probe + align_down. Nonsense / coincidental headers must
    // fail closed (return false from try_deallocate) — never trust magic alone.

    inline constexpr std::uint32_t kBlockHeaderMagic0 = 0xB10C401Au;
    inline constexpr std::uint32_t kBlockHeaderMagic1 = 0xA104C01Bu;
    inline constexpr std::uint8_t kBlockFlagPublished = 0x01;

    inline constexpr std::size_t kBlockHeaderBytes = 64;

    // Candidate spans for align_down probe (exact set for default ladders +
    // head header). Ascending order: smaller lookback first for wild pointers.
    inline constexpr std::size_t kBlockSpanCandidates[] = {
        8192u, 16384u, 32768u, 65536u, 131072u, 262144u, 524288u};
    inline constexpr std::size_t kBlockSpanCandidateCount =
        sizeof(kBlockSpanCandidates) / sizeof(kBlockSpanCandidates[0]);

    // True if [base, base + kBlockHeaderBytes) is mapped (Linux msync probe).
    // Span probe must not SEGV on stack / junk pointers.
    inline bool block_header_pages_mapped(const std::byte* base) noexcept
    {
        constexpr std::uintptr_t kPageMask = ~std::uintptr_t{4095};
        const auto first = reinterpret_cast<std::uintptr_t>(base) & kPageMask;
        const auto last =
            (reinterpret_cast<std::uintptr_t>(base) + kBlockHeaderBytes - 1) &
            kPageMask;
        if (::msync(reinterpret_cast<void*>(first), 1, MS_ASYNC) != 0)
        {
            return false;
        }
        if (last != first &&
            ::msync(reinterpret_cast<void*>(last), 1, MS_ASYNC) != 0)
        {
            return false;
        }
        return true;
    }

    struct alignas(64) BlockHeader
    {
        std::uint32_t magic0{0};
        std::uint32_t span{0};
        std::uint32_t elem_size{0};
        std::uint16_t generation{0};
        std::uint8_t align_class{0};
        std::uint8_t flags{0};
        Pool* pool{nullptr};
        ISlab* slab{nullptr};
        std::uint64_t pool_cookie{0};
        std::uint32_t magic1{0};
        std::uint32_t reserved0{0};
        std::byte pad_[16]{};
    };

    static_assert(sizeof(BlockHeader) == kBlockHeaderBytes,
                  "BlockHeader must be exactly kBlockHeaderBytes");
    static_assert(alignof(BlockHeader) == 64,
                  "BlockHeader must be 64-byte aligned");

    inline constexpr std::size_t next_pow2_size(std::size_t n) noexcept
    {
        if (n <= 1)
        {
            return 1;
        }
        return std::bit_ceil(n);
    }

    // Structural + identity checks only (no slab ownership yet). Any failure
    // means "not a live Block header for this cookie/span" — caller must not
    // follow slab/pool pointers for work beyond rejecting the candidate.
    inline bool block_header_identity_ok(const BlockHeader& h,
                                         std::size_t expected_span,
                                         std::uint64_t expected_cookie) noexcept
    {
        if (h.magic0 != kBlockHeaderMagic0 || h.magic1 != kBlockHeaderMagic1)
        {
            return false;
        }
        if (h.pool_cookie != expected_cookie)
        {
            return false;
        }
        if (h.span != expected_span || h.span < kBlockHeaderBytes ||
            (h.span & (h.span - 1u)) != 0)
        {
            return false;
        }
        // elem_size need not be power-of-two (48, 96, …) but must be 16-aligned.
        if (h.elem_size == 0 || (h.elem_size % 16u) != 0)
        {
            return false;
        }
        if (h.align_class != 16 && h.align_class != 32 && h.align_class != 64)
        {
            return false;
        }
        if ((h.elem_size % h.align_class) != 0)
        {
            return false;
        }
        if ((h.flags & kBlockFlagPublished) == 0)
        {
            return false;
        }
        // Unknown flag bits ⇒ corrupt / future format we do not understand.
        if ((h.flags & ~kBlockFlagPublished) != 0)
        {
            return false;
        }
        if (h.pool == nullptr || h.slab == nullptr)
        {
            return false;
        }
        if (h.reserved0 != 0)
        {
            return false;
        }
        // Slot region may include a short unused pad after the last slot
        // ((span - header) % elem_size); that is normal for pow2 spans.
        const std::size_t slot_bytes =
            static_cast<std::size_t>(h.span) - kBlockHeaderBytes;
        if (slot_bytes / h.elem_size == 0)
        {
            return false;
        }
        return true;
    }

    inline std::byte* block_slots_start(std::byte* block_base) noexcept
    {
        return block_base + kBlockHeaderBytes;
    }

    inline const BlockHeader* block_header_at(const std::byte* block_base) noexcept
    {
        return reinterpret_cast<const BlockHeader*>(block_base);
    }

    inline BlockHeader* block_header_at(std::byte* block_base) noexcept
    {
        return reinterpret_cast<BlockHeader*>(block_base);
    }

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_BLOCK_HEADER_HPP_
