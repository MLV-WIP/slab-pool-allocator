#ifndef SLAP_HPP_
#define SLAP_HPP_

#include <cstddef>
#include <type_traits>


namespace spallocator
{

    
    template<const std::size_t ElemSize>
    class SmallSlab
    {
    public:
        SmallSlab();
        ~SmallSlab() = default;
        SmallSlab(const SmallSlab&) = delete;
        SmallSlab& operator=(const SmallSlab&) = delete;
        SmallSlab(SmallSlab&&) = delete;
        SmallSlab& operator=(SmallSlab&&) = delete;

        constexpr std::size_t getElemSize() const { return ElemSize; }
        constexpr std::size_t getSlabAllocSize() const { return slab_alloc_size; }

    private:
        static constexpr std::size_t slab_alloc_size{4096};
        static_assert(slab_alloc_size > 2048, "Allocation size must be greater than 2048");
        static_assert(slab_alloc_size % 1024 == 0, "Allocation size must be a multiple of 1024");


    };


    template<const std::size_t ElemSize>
    class LargeSlab
    {
    public:
        LargeSlab();
        ~LargeSlab() = default;
        LargeSlab(const LargeSlab&) = delete;
        LargeSlab& operator=(const LargeSlab&) = delete;
        LargeSlab(LargeSlab&&) = delete;
        LargeSlab& operator=(LargeSlab&&) = delete;

        constexpr std::size_t getElemSize() const { return ElemSize; }
        constexpr std::size_t getSlabAllocSize() const { return slab_alloc_size; }

    private:
        static constexpr std::size_t initial_alloc_multiplier{4}; // allocate 4 at a time
        static constexpr std::size_t slab_alloc_size{ElemSize * initial_alloc_multiplier};

        // subsequent allocations are a multiple of...
        static constexpr std::size_t alloc_multiplier{2};
    };


    template<const std::size_t ElemSize>
    class Slab
    {
    public:
        static_assert(ElemSize >= 8, "Element size must be at least 8 bytes");
        static_assert(ElemSize % 8 == 0, "Element size must be a multiple of 8 bytes");
        using slab_type = typename std::conditional<(ElemSize <= 1024),
            SmallSlab<ElemSize>, LargeSlab<ElemSize>>::type;

        Slab();
        ~Slab() = default;
        Slab(const Slab&) = delete;
        Slab& operator=(const Slab&) = delete;
        Slab(Slab&&) = delete;
        Slab& operator=(Slab&&) = delete;

        constexpr std::size_t getElemSize() const { return ElemSize; }
        constexpr std::size_t getSlabAllocSize() const { return slab.getSlabAllocSize(); }

    private:
        slab_type slab;
    };


    template<const std::size_t ElemSize>
    Slab<ElemSize>::Slab()
    {
        println("Slab created with element size: {} and allocation size: {}",
                getElemSize(), getSlabAllocSize());
    }

    template<const std::size_t ElemSize>
    SmallSlab<ElemSize>::SmallSlab()
    {
        println("Small slab created with element size: {} and allocation size: {}",
                getElemSize(), getSlabAllocSize());
    }

    template<const std::size_t ElemSize>
    LargeSlab<ElemSize>::LargeSlab()
    {
        println("Large slab created with element size: {} and allocation size: {}",
                getElemSize(), getSlabAllocSize());
    }

}; // namespace spallocator


#endif // SLAP_HPP_
