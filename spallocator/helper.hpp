/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2025, Michael VanLoon
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

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <format>
#include <source_location>


// User-defined literals for binary prefixes (powers of 1024)
constexpr std::size_t operator""_KB(unsigned long long value) {
    return value * 1024;
}

constexpr std::size_t operator""_MB(unsigned long long value) {
    return value * 1024 * 1024;
}

constexpr std::size_t operator""_GB(unsigned long long value) {
    return value * 1024 * 1024 * 1024;
}


// print and println added in C++23, but we don't have them yet; make our own
// Adapted from GitHub Jhuighuy /TitSolver/source/tit/core/io_utils.hpp under MIT license
// --- start Jhuighuy ---
template<typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args)
{
    std::cout << std::format(fmt, std::forward<Args>(args)...);
}

template<typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args)
{
    std::cout << std::format(fmt, std::forward<Args>(args)...) << '\n';
}
// --- end Jhuighuy ---


inline void runtime_assert(bool condition,
                           const char* message,
                           const std::source_location& loc = std::source_location::current())
{
    if (!condition)
    {
        std::cerr << std::format(
            "Runtime assertion failed: {}\n  File: {}:{}\n  Function: {}\n",
            message, loc.file_name(), loc.line(), loc.function_name());
        std::abort();
    }
}

inline void runtime_assert(bool condition,
                           const std::string& message,
                           const std::source_location& loc = std::source_location::current())
{
    runtime_assert(condition, message.c_str(), loc);
}


#endif // HELPER_HPP_
