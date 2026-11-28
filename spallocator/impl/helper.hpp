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

// -----------------------------------------------------------------------------
// Hot/cold inlining and code-placement hints (spallocator/impl/helper.hpp)
// -----------------------------------------------------------------------------
// Allocator hot paths (Slab::allocateItem, Slab::deallocateItem) are called
// millions of times per second behind an ISlab virtual dispatch. Even a never-
// taken error throw, std::format call, or slow-growth fallback inside a hot
// function can:
//   1. Pull std::string / std::format temporaries and C++ EH landing pads into
//      the hot function's stack frame, forcing callee-saved register spills in
//      the prologue/epilogue on EVERY hot call.
//   2. Inflate the function's IR size enough to trip compiler inlining budgets,
//      preventing tiny hot helpers from inlining into allocateItem/deallocateItem.
//   3. Spread hot instructions across extra I-cache lines and branch-predictor
//      entries.
//
// We use three distinct macros because "don't inline into the caller" and
// "optimize the callee as cold/rare" are independent decisions. Each macro is
// #ifndef-guarded so builds can override them if needed.
//
// 1. SPALLOCATOR_COLD_NOINLINE
//    - GCC/Clang: [[gnu::cold, gnu::noinline]]
//    - MSVC:      __declspec(noinline)
//    - Effect on caller:
//        * Never inlined into the caller, keeping the caller's stack frame,
//          register pressure, and instruction footprint minimal.
//        * GCC/Clang automatically treat branches leading to a [[gnu::cold]]
//          callee as unlikely (like [[unlikely]] at every call site), keeping
//          the hot path straight-line fall-through.
//    - Effect on callee:
//        * Compiled for size (-Os) and placed in a cold text section
//          (.text.unlikely) away from hot I-cache lines.
//    - Use for: truly rare slow paths (Slab::allocateSlow,
//      Slab::findAvailableBlockLocked, Block::allocateSlotSlow) and [[noreturn]]
//      out-of-line throw helpers (detail::throw*). Do NOT use on paths that can
//      be hot for some callers (e.g., align <= 8 unhinted frees).
//
// 2. SPALLOCATOR_NOINLINE
//    - GCC/Clang: [[gnu::noinline]]
//    - MSVC:      __declspec(noinline)
//    - Effect on caller:
//        * Never inlined into the caller, keeping the primary hot path compact,
//          avoiding extra callee-saved spills, and enabling tail-call dispatch.
//    - Effect on callee:
//        * Still compiled at full -O3 in the normal hot .text section and free
//          to inline its own callees (unlike SPALLOCATOR_COLD_NOINLINE).
//    - Use for: secondary fast paths that must not bloat the primary path yet
//      run hot in specific workloads (e.g., Slab::deallocateUnhintedLocked for
//      align <= 8 Pool allocations and direct unhinted Slab::deallocateItem).
//
// 3. SPALLOCATOR_ALWAYS_INLINE
//    - GCC/Clang: [[gnu::always_inline]] inline
//    - MSVC:      __forceinline
//    - Effect:
//        * Forces inlining of a small hot helper even when compiler cost
//          heuristics hesitate around lock guards or template wrappers, and
//          preserves `inline` linkage for header-defined templates.
//        * Eliminates call/return overhead and lets values (such as
//          AllocateItemResult returned in registers) stay in registers across
//          the boundary.
//    - Use sparingly and pair with SPALLOCATOR_COLD_NOINLINE on any slow
//      branches inside the helper so only the tiny hot core is inlined (e.g.,
//      Slab::tryAllocateFromExistingLocked).
// -----------------------------------------------------------------------------

#ifndef SPALLOCATOR_COLD_NOINLINE
#  if defined(__GNUC__) || defined(__clang__)
#    define SPALLOCATOR_COLD_NOINLINE [[gnu::cold, gnu::noinline]]
#  elif defined(_MSC_VER)
#    define SPALLOCATOR_COLD_NOINLINE __declspec(noinline)
#  else
#    define SPALLOCATOR_COLD_NOINLINE
#  endif
#endif

#ifndef SPALLOCATOR_NOINLINE
#  if defined(__GNUC__) || defined(__clang__)
#    define SPALLOCATOR_NOINLINE [[gnu::noinline]]
#  elif defined(_MSC_VER)
#    define SPALLOCATOR_NOINLINE __declspec(noinline)
#  else
#    define SPALLOCATOR_NOINLINE
#  endif
#endif

#ifndef SPALLOCATOR_ALWAYS_INLINE
#  if defined(__GNUC__) || defined(__clang__)
#    define SPALLOCATOR_ALWAYS_INLINE [[gnu::always_inline]] inline
#  elif defined(_MSC_VER)
#    define SPALLOCATOR_ALWAYS_INLINE __forceinline
#  else
#    define SPALLOCATOR_ALWAYS_INLINE inline
#  endif
#endif

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

    // Largest ladder slot today is 16384 (alignment class 64); max header is 64.
    static_assert(kMaxAllocationBytes >= 16384u + 64u,
        "kMaxAllocationBytes must cover largest size-class footprint plus header");
    static_assert(kMaxClassBytes >= 16u * 16384u,
        "kMaxClassBytes must fit at least one max-ladder Block (16 x 16384)");
    static_assert(kMaxClassBytes >= kMaxAllocationBytes,
        "kMaxClassBytes should be at least kMaxAllocationBytes");

} // namespace spallocator

#endif // HELPER_HPP_
