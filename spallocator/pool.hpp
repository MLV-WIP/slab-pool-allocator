#ifndef POOL_HPP_
#define POOL_HPP_

#include "slab.hpp"


namespace spallocator
{

    struct Allocation
    {
        std::byte* ptr;
        std::size_t size;        
    };


    class Pool
    {
    public: // methods
        std::byte* allocate(std::size_t size);
        void deallocate(std::byte* item);

        Pool();
        ~Pool() = default;

        constexpr::size_t selectSlab(std::size_t size) const;

    private: // methods
        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;
        Pool(Pool&&) = delete;
        Pool& operator=(Pool&&) = delete;

    private: // data members
        std::vector<std::unique_ptr<AbstractSlab>> small_slabs;
        std::unordered_map<std::size_t, std::unique_ptr<AbstractSlab>> large_slabs;

        std::unordered_map<std::byte*, Allocation> allocations;
    };


    std::byte* Pool::allocate(std::size_t size)
    {
        Allocation alloc;
        alloc.size = size;

        // TODO:
        // allocate 8 additional bytes, store the size at the beginning of the block,
        // and return the pointer offset by 8 bytes
        // on deallocation, read the size from the 8 bytes before the pointer
        // and use that to select the proper slab

        auto slab_index = selectSlab(size);
        if (slab_index != std::numeric_limits<std::size_t>::max())
        {
            // small slab
            alloc.ptr = small_slabs[slab_index]->allocateItem();
            allocations[alloc.ptr] = alloc;
            println("Allocated {} bytes from small slab {}, ptr={}",
                    size, slab_index, static_cast<void*>(alloc.ptr));
            return alloc.ptr;
        }
        else
        {
            // large slab, create if not exists
            if (large_slabs.find(size) == large_slabs.end())
            {
                large_slabs[size] = nullptr;
                // large_slabs[size] = std::make_unique<Slab<size>>();
            }
            //alloc.ptr = large_slabs[size]->allocateItem();
            alloc.ptr = nullptr; // placeholder until large slab is implemented
            println("Allocated {} bytes from large slab, ptr={}",
                    size, static_cast<void*>(alloc.ptr));                    
            return alloc.ptr;
        }
    }


    void Pool::deallocate(std::byte* item)
    {
        if (item == nullptr)
        {
            return;
        }
        
        for (auto& slab : small_slabs)
        {
            try
            {
                slab->deallocateItem(item);
                return;
            }
            catch (const std::invalid_argument&)
            {
                // not in this slab, continue
            }
        }
        for (auto& slab : large_slabs)
        {
            try
            {
                slab.second->deallocateItem(item);
                return;
            }
            catch (const std::invalid_argument&)
            {
                // not in this slab, continue
            }
        }
    }

    constexpr std::size_t Pool::selectSlab(std::size_t size) const
    {
        if (size <= 16) return 0;
        else if (size <= 32) return 1;
        else if (size <= 48) return 2;
        else if (size <= 64) return 3;
        else if (size <= 96) return 4;
        else if (size <= 128) return 5;
        else if (size <= 192) return 6;
        else if (size <= 256) return 7;
        else if (size <= 384) return 8;
        else if (size <= 512) return 9;
        else if (size <= 768) return 10;
        else if (size <= 1_KB) return 11;
        else return std::numeric_limits<std::size_t>::max(); // indicates large slab
    }

    Pool::Pool()
    {
        // create slabs for small sizes (up to 1KB)
        small_slabs.push_back(std::make_unique<Slab<16>>());   // 0
        small_slabs.push_back(std::make_unique<Slab<32>>());   // 1
        small_slabs.push_back(std::make_unique<Slab<48>>());   // 2
        small_slabs.push_back(std::make_unique<Slab<64>>());   // 3
        small_slabs.push_back(std::make_unique<Slab<96>>());   // 4
        small_slabs.push_back(std::make_unique<Slab<128>>());  // 5
        small_slabs.push_back(std::make_unique<Slab<192>>());  // 6
        small_slabs.push_back(std::make_unique<Slab<256>>());  // 7
        small_slabs.push_back(std::make_unique<Slab<384>>());  // 8
        small_slabs.push_back(std::make_unique<Slab<512>>());  // 9
        small_slabs.push_back(std::make_unique<Slab<768>>());  // 10
        small_slabs.push_back(std::make_unique<Slab<1_KB>>()); // 11
    }

}; // namespace spallocator


#endif // POOL_HPP_
