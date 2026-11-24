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

#ifndef SPALLOCATOR_IMPL_CRC32C_HPP_
#define SPALLOCATOR_IMPL_CRC32C_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  if defined(__SSE4_2__)
#    define SPALLOCATOR_CRC32C_HW 1
#    include <nmmintrin.h>
#  endif
#elif defined(__aarch64__)
#  if defined(__ARM_FEATURE_CRC32)
#    define SPALLOCATOR_CRC32C_HW 1
#    include <arm_acle.h>
#  endif
#endif

namespace spallocator
{
namespace detail
{

    // Castagnoli CRC-32C, reflected polynomial.
    inline constexpr std::uint32_t kCrc32cPoly = 0x82F63B78u;

    struct Crc32cTable
    {
        std::uint32_t t[256];

        constexpr Crc32cTable() : t{}
        {
            for (std::uint32_t i = 0; i < 256; ++i)
            {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                {
                    c = (c & 1u) ? (kCrc32cPoly ^ (c >> 1)) : (c >> 1);
                }
                t[i] = c;
            }
        }
    };

    inline constexpr Crc32cTable kCrc32cTable{};

    inline std::uint32_t crc32c_sw(const void* data, std::size_t len) noexcept
    {
        std::uint32_t crc = 0xffffffffu;
        const auto* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < len; ++i)
        {
            crc = kCrc32cTable.t[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
        }
        return crc ^ 0xffffffffu;
    }

#if defined(SPALLOCATOR_CRC32C_HW)
    inline std::uint32_t crc32c_hw(const void* data, std::size_t len) noexcept
    {
        std::uint32_t crc = 0xffffffffu;
        const auto* p = static_cast<const std::uint8_t*>(data);
#if defined(__SSE4_2__)
        while (len >= 8)
        {
            std::uint64_t v = 0;
            std::memcpy(&v, p, 8);
            crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, v));
            p += 8;
            len -= 8;
        }
        if (len >= 4)
        {
            std::uint32_t v = 0;
            std::memcpy(&v, p, 4);
            crc = _mm_crc32_u32(crc, v);
            p += 4;
            len -= 4;
        }
        while (len > 0)
        {
            crc = _mm_crc32_u8(crc, *p);
            ++p;
            --len;
        }
#elif defined(__ARM_FEATURE_CRC32)
        while (len >= 8)
        {
            std::uint64_t v = 0;
            std::memcpy(&v, p, 8);
            crc = __crc32cd(crc, v);
            p += 8;
            len -= 8;
        }
        if (len >= 4)
        {
            std::uint32_t v = 0;
            std::memcpy(&v, p, 4);
            crc = __crc32cw(crc, v);
            p += 4;
            len -= 4;
        }
        while (len > 0)
        {
            crc = __crc32cb(crc, *p);
            ++p;
            --len;
        }
#endif
        return crc ^ 0xffffffffu;
    }
#endif

} // namespace detail

    // CRC-32C (Castagnoli). Not a cryptographic MAC.
    inline std::uint32_t crc32c(const void* data, std::size_t len) noexcept
    {
#if defined(SPALLOCATOR_CRC32C_HW)
        return detail::crc32c_hw(data, len);
#else
        return detail::crc32c_sw(data, len);
#endif
    }

} // namespace spallocator

#endif // SPALLOCATOR_IMPL_CRC32C_HPP_
