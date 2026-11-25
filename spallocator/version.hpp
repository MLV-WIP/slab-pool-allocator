/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2025-2026, Michael VanLoon
 * All rights reserved.
 *
 * Version macros for the slab-pool-allocator. Updated on main releases by the
 * shared MLV-WIP/release-tooling semantic-release workflow (prepare →
 * scripts/set-version.py). Dev / PR checkouts should prefer
 * `git describe --tags --match 'v*'` for a build identity; these macros track
 * the last released SemVer committed on the branch.
 */

#ifndef SPALLOCATOR_VERSION_HPP_
#define SPALLOCATOR_VERSION_HPP_

// Keep these three integers and the string in sync. scripts/set-version.py
// rewrites this block during a release.
#define SPALLOCATOR_VERSION_MAJOR 2
#define SPALLOCATOR_VERSION_MINOR 1
#define SPALLOCATOR_VERSION_PATCH 0
#define SPALLOCATOR_VERSION_STRING "2.1.0"

namespace spallocator
{
    inline constexpr int kVersionMajor = SPALLOCATOR_VERSION_MAJOR;
    inline constexpr int kVersionMinor = SPALLOCATOR_VERSION_MINOR;
    inline constexpr int kVersionPatch = SPALLOCATOR_VERSION_PATCH;
    inline constexpr char const* kVersionString = SPALLOCATOR_VERSION_STRING;
}

#endif // SPALLOCATOR_VERSION_HPP_
