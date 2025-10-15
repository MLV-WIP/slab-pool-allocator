#ifndef SLAP_HPP_
#define SLAP_HPP_

#include <cstddef>
#include <type_traits>


namespace spallocator
{

    template<std::size_t ElemSize>
    constexpr std::size_t selectBufferSize() {
        if constexpr (ElemSize <= 1_KB) {
            return 4_KB;
        } else if constexpr (ElemSize <= 2_KB) {
            return 8_KB;
        } else {
            return ElemSize * 4;
        }
    }


    template<const std::size_t ElemSize>
    class Slab
    {
    public:
        Slab();
        ~Slab() = default;
        Slab(const Slab&) = delete;
        Slab& operator=(const Slab&) = delete;
        Slab(Slab&&) = delete;
        Slab& operator=(Slab&&) = delete;

        constexpr std::size_t getElemSize() const { return ElemSize; }
        constexpr std::size_t getAllocSize() const { return slab_alloc_size; }

    private:
        static constexpr std::size_t slab_alloc_size{selectBufferSize<ElemSize>()};
        static_assert(slab_alloc_size >= 4_KB, "Allocation size must be greater than 4096 bytes");
        static_assert(
            (ElemSize > 2_KB) || (slab_alloc_size % 1_KB == 0),
            "Allocation size must be a multiple of 1024 when ElemSize <= 2KB");

        // subsequent allocations are a multiple of...
        static constexpr std::size_t alloc_multiplier{2};
        static_assert(alloc_multiplier >= 2, "Allocation multiplier must be at least 2");
        static_assert(alloc_multiplier % 2 == 0, "Allocation multiplier must be a multiple of 2");
    };


    template<const std::size_t ElemSize>
    Slab<ElemSize>::Slab()
    {
        println("Slab created with element size: {}, allocation size: {}, and multiplier: {}",
                getElemSize(), getAllocSize(), alloc_multiplier);
    }

}; // namespace spallocator


#endif // SLAP_HPP_
