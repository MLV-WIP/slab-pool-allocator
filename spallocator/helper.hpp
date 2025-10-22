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
#include <print>
#include <source_location>


#ifndef NDEBUG
    constexpr bool DEBUG_BUILD = true;
#else
    constexpr bool DEBUG_BUILD = false;
#endif

#ifdef VERBOSE_DEBUG
    constexpr bool VERBOSE_DEBUG = DEBUG_BUILD;
#else
    constexpr bool VERBOSE_DEBUG = false;
#endif


namespace spallocator
{

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


    #if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
        using std::print;
        using std::println;
    #else
        #pragma message("C++23 print and println not available; using custom implementations")
        // print and println added in C++23, but we don't have them yet; make our own
        // Adapted from GitHub Jhuighuy /TitSolver/source/tit/core/io_utils.hpp under MIT license
        // --- start Jhuighuy ---
        template<typename... Args>
        inline void print(std::format_string<Args...> fmt, Args&&... args)
        {
            std::cout << std::format(fmt, std::forward<Args>(args)...);
        }

        template<typename... Args>
        inline void println(std::format_string<Args...> fmt, Args&&... args)
        {
            std::cout << std::format(fmt, std::forward<Args>(args)...) << '\n';
        }
        // --- end Jhuighuy ---
    #endif


    //
    // Debug-only print statements (no-op in release builds)
    // Note: this is a modern C++ paradigm for conditional compilation. Because
    // the if constexpr is evaluated at compile time, the code inside the if
    // block is only included in the binary if VERBOSE_DEBUG is true, much like
    // traditional preprocessor macros, but with the advantage of type safety
    // and readability, being actual C++ code rather than text substitutions.
    //
    template<typename... Args>
    inline void debug_print(std::format_string<Args...> fmt, Args&&... args)
    {
        if constexpr (VERBOSE_DEBUG) {
            print(fmt, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    inline void debug_println(std::format_string<Args...> fmt, Args&&... args)
    {
        if constexpr (VERBOSE_DEBUG) {
            println(fmt, std::forward<Args>(args)...);
        }
    }


    //
    // Runtime assertions are valuable because they are self-diagnosing code.
    //
    // What they are: checks that validate assumptions made by the program at
    // runtime. If an assumption is violated, the program can immediately halt
    // and report the error, rather than continuing in an invalid state which
    // could lead to more obscure bugs later on.
    //
    // What they are not: they are not a substitute for proper error handling
    // in production code. They are primarily a debugging aid to catch
    // programming errors during development. They should only be used in
    // places where assumptions are being validated that should reasonably
    // never be violated if the code is correct. They should never replace
    // runtime checks for invalid states that could occur due to external
    // factors (e.g., invalid API parameters, invalid user input, resource
    // exhaustion, etc.)
    //
    // See also: std::static_assert for compile-time assertions.
    //
    inline void runtime_assert(bool condition,
                               const char* message,
                               const std::source_location& loc = std::source_location::current())
    {
        if constexpr (DEBUG_BUILD)
        {
            if (!condition)
            {
                std::cerr << std::format(
                    "Runtime assertion failed: {}\n  File: {}:{}\n  Function: {}\n",
                    message, loc.file_name(), loc.line(), loc.function_name());
                std::abort();
            }
        }
    }

    inline void runtime_assert(bool condition,
                               const std::string& message,
                               const std::source_location& loc = std::source_location::current())
    {
        runtime_assert(condition, message.c_str(), loc);
    }

} // namespace spallocator


#endif // HELPER_HPP_
