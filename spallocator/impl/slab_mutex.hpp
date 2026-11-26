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

#ifndef SPALLOCATOR_IMPL_SLAB_MUTEX_HPP_
#define SPALLOCATOR_IMPL_SLAB_MUTEX_HPP_

#include <mutex>

#include "mlvlib/lock_traits.hpp"
#include "mlvlib/null_lock.hpp"
#include "mlvlib/spinlock.hpp"

// Compile-time slab lock policy selection (override via -D, ./configure
// --with-lock-policy=..., or `make LOCK_POLICY=...`).
// Do not link translation units built with mismatched SPALLOCATOR_LOCK_POLICY.
#define SPALLOCATOR_LOCK_SPINLOCK  1
#define SPALLOCATOR_LOCK_STD_MUTEX 2
#define SPALLOCATOR_LOCK_NULL      3

#ifndef SPALLOCATOR_LOCK_POLICY
#  define SPALLOCATOR_LOCK_POLICY SPALLOCATOR_LOCK_SPINLOCK
#endif

namespace spallocator
{
#if SPALLOCATOR_LOCK_POLICY == SPALLOCATOR_LOCK_SPINLOCK
    using DefaultSlabLock = mlvlib::SpinLock;
#elif SPALLOCATOR_LOCK_POLICY == SPALLOCATOR_LOCK_STD_MUTEX
    using DefaultSlabLock = std::mutex;
#elif SPALLOCATOR_LOCK_POLICY == SPALLOCATOR_LOCK_NULL
    using DefaultSlabLock = mlvlib::NullLock;
#else
#  error "Invalid SPALLOCATOR_LOCK_POLICY (expected SPALLOCATOR_LOCK_SPINLOCK, SPALLOCATOR_LOCK_STD_MUTEX, or SPALLOCATOR_LOCK_NULL)"
#endif

    static_assert(mlvlib::BasicLockable<DefaultSlabLock>,
                  "DefaultSlabLock must satisfy mlvlib::BasicLockable");
} // namespace spallocator

#endif // SPALLOCATOR_IMPL_SLAB_MUTEX_HPP_
