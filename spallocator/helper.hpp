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
