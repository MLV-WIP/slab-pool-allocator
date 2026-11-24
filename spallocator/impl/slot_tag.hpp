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

#ifndef SPALLOCATOR_IMPL_SLOT_TAG_HPP_
#define SPALLOCATOR_IMPL_SLOT_TAG_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "impl/crc32c.hpp"

namespace spallocator
{

    // Size-class Layout A trailer (8 B at slot_base + ElemSize - 8).
    // Distinct from proxy front-header kAllocMagic.
    inline constexpr std::uint8_t kTagMagicLive = 0xA1;
    inline constexpr std::uint8_t kTagMagicFreed = 0xA0;
    inline constexpr std::uint8_t kTagFormatV1 = 0;
    inline constexpr std::size_t kSlotTagBytes = 8;
    inline constexpr std::size_t kSizeClassTrailerTax = 8;

    // LE uint64: magic[7:0] | gen[23:8] | crc32c[55:24] | format[63:56]
    inline std::uint64_t pack_slot_tag(std::uint8_t magic,
                                       std::uint16_t generation,
                                       std::uint32_t crc,
                                       std::uint8_t format = kTagFormatV1) noexcept
    {
        return std::uint64_t{magic}
             | (std::uint64_t{generation} << 8)
             | (std::uint64_t{crc} << 24)
             | (std::uint64_t{format} << 56);
    }

    inline void unpack_slot_tag(std::uint64_t tag,
                                std::uint8_t& magic,
                                std::uint16_t& generation,
                                std::uint32_t& crc,
                                std::uint8_t& format) noexcept
    {
        magic = static_cast<std::uint8_t>(tag & 0xffu);
        generation = static_cast<std::uint16_t>((tag >> 8) & 0xffffu);
        crc = static_cast<std::uint32_t>((tag >> 24) & 0xffffffffu);
        format = static_cast<std::uint8_t>((tag >> 56) & 0xffu);
    }

    // CRC coverage: LE concat of user_ptr ‖ elem_size ‖ generation ‖ magic (19 B).
    inline std::uint32_t slot_tag_crc(std::byte* user_ptr,
                                      std::size_t elem_size,
                                      std::uint16_t generation,
                                      std::uint8_t magic) noexcept
    {
        std::uint8_t buf[19];
        const std::uint64_t ptr_bits = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(user_ptr));
        const std::uint64_t elem = static_cast<std::uint64_t>(elem_size);
        std::memcpy(buf + 0, &ptr_bits, 8);
        std::memcpy(buf + 8, &elem, 8);
        std::memcpy(buf + 16, &generation, 2);
        buf[18] = magic;
        return crc32c(buf, sizeof(buf));
    }

    inline void write_slot_tag(std::byte* slot_base,
                               std::size_t elem_size,
                               std::uint16_t generation) noexcept
    {
        const std::uint32_t crc =
            slot_tag_crc(slot_base, elem_size, generation, kTagMagicLive);
        const std::uint64_t tag =
            pack_slot_tag(kTagMagicLive, generation, crc, kTagFormatV1);
        std::memcpy(slot_base + elem_size - kSlotTagBytes, &tag, kSlotTagBytes);
    }

    // Returns true if LIVE tag matches map entry generation + CRC.
    inline bool verify_slot_tag(std::byte* slot_base,
                                std::size_t elem_size,
                                std::uint16_t expected_generation) noexcept
    {
        std::uint64_t tag = 0;
        std::memcpy(&tag, slot_base + elem_size - kSlotTagBytes, kSlotTagBytes);
        std::uint8_t magic = 0;
        std::uint16_t generation = 0;
        std::uint32_t crc = 0;
        std::uint8_t format = 0;
        unpack_slot_tag(tag, magic, generation, crc, format);
        if (magic != kTagMagicLive || format != kTagFormatV1)
        {
            return false;
        }
        if (generation != expected_generation)
        {
            return false;
        }
        const std::uint32_t expect =
            slot_tag_crc(slot_base, elem_size, expected_generation, kTagMagicLive);
        return crc == expect;
    }

    inline void clear_slot_tag(std::byte* slot_base, std::size_t elem_size) noexcept
    {
        // FREED marker; CRC left zeroed (not revalidated on free-after-free).
        const std::uint64_t tag =
            pack_slot_tag(kTagMagicFreed, 0, 0, kTagFormatV1);
        std::memcpy(slot_base + elem_size - kSlotTagBytes, &tag, kSlotTagBytes);
    }

    // Proxy front-header CRC: LE concat user_ptr ‖ alloc_size ‖ header_size ‖ magic.
    inline std::uint32_t proxy_header_crc(std::byte* user_ptr,
                                          std::uint32_t alloc_size,
                                          std::uint8_t header_size,
                                          std::uint8_t magic) noexcept
    {
        std::uint8_t buf[14];
        const std::uint64_t ptr_bits = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(user_ptr));
        std::memcpy(buf + 0, &ptr_bits, 8);
        std::memcpy(buf + 8, &alloc_size, 4);
        buf[12] = header_size;
        buf[13] = magic;
        return crc32c(buf, sizeof(buf));
    }

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_SLOT_TAG_HPP_
