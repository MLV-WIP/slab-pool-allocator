#ifndef HELPER_HPP_
#define HELPER_HPP_

#include <cstddef>
#include <iostream>
#include <format>


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


#endif // HELPER_HPP_
