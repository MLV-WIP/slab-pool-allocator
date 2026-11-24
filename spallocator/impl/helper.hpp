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

#ifndef HELPER_HPP_
#define HELPER_HPP_

// General helpers (print, runtime_assert, size literals, debug flags) live in
// the mlvlib submodule. This header pulls them in and re-exports the symbols
// commonly used inside namespace spallocator, then defines allocator-only
// compile-time size caps.
//
// Requires -Imlvlib (submodule root) so #include "mlvlib/..." resolves.

#include <cstddef>

#include "mlvlib/helper.hpp"

namespace spallocator
{

    using mlvlib::operator""_KB;
    using mlvlib::operator""_MB;
    using mlvlib::operator""_GB;
    using mlvlib::print;
    using mlvlib::println;
    // debug_print / debug_println are macros from mlvlib/helper.hpp so that
    // arguments are not evaluated when VERBOSE_DEBUG is off.
    using mlvlib::runtime_assert;

    // Compile-time size caps (override via -D / ./configure). Not runtime-tunable.
#ifndef SPALLOCATOR_MAX_ALLOCATION_BYTES
#  define SPALLOCATOR_MAX_ALLOCATION_BYTES (1024ull * 1024ull * 1024ull) // 1 GiB
#endif
#ifndef SPALLOCATOR_MAX_CLASS_BYTES
#  define SPALLOCATOR_MAX_CLASS_BYTES (4ull * 1024ull * 1024ull * 1024ull) // 4 GiB
#endif

    inline constexpr std::size_t kMaxAllocationBytes =
        static_cast<std::size_t>(SPALLOCATOR_MAX_ALLOCATION_BYTES);
    inline constexpr std::size_t kMaxClassBytes =
        static_cast<std::size_t>(SPALLOCATOR_MAX_CLASS_BYTES);

    // Largest ladder slot today is 16384 (alignment class 64); trailer tax is 8,
    // proxy front header max is 64.
    static_assert(kMaxAllocationBytes >= 16384u + 64u,
        "kMaxAllocationBytes must cover largest size-class footprint plus metadata");
    static_assert(kMaxClassBytes >= 16u * 16384u,
        "kMaxClassBytes must fit at least one max-ladder Block (16 x 16384)");
    static_assert(kMaxClassBytes >= kMaxAllocationBytes,
        "kMaxClassBytes should be at least kMaxAllocationBytes");

} // namespace spallocator

#endif // HELPER_HPP_
