#ifndef HELPER_HPP_
#define HELPER_HPP_

#include <cstddef>
#include <iostream>
#include <format>


    // User-defined literals for binary prefixes (powers of 1024)
    constexpr std::size_t operator""_K(unsigned long long value) {
        return value * 1024;
    }

    constexpr std::size_t operator""_M(unsigned long long value) {
        return value * 1024 * 1024;
    }

    constexpr std::size_t operator""_G(unsigned long long value) {
        return value * 1024 * 1024 * 1024;
    }


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
