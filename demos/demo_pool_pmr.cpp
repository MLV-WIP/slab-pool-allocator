/*
 * Demo: PoolMemoryResource with std::pmr containers, vs PoolAllocator, plus
 * an in-demo tracing decorator (Roy-style) that is not a library type.
 */

#include <memory_resource>
#include <vector>

#include "spallocator/impl/helper.hpp"
#include "spallocator/pool.hpp"
#include "spallocator/spallocator.hpp"
#include "spallocator/pool_pmr.hpp"

using namespace spallocator;

// Demo-only decorator: log allocate/deallocate sizes, forward to upstream.
// Not part of the library — mirrors the book's tracing_resource idea locally.
class TracingMemoryResource : public std::pmr::memory_resource
{
public: // methods
    explicit TracingMemoryResource(std::pmr::memory_resource* upstream) noexcept;

protected: // methods
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* p, std::size_t bytes,
                       std::size_t alignment) noexcept override;
    bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override;

private: // data members
    std::pmr::memory_resource* upstream_;
};


inline TracingMemoryResource::TracingMemoryResource(
    std::pmr::memory_resource* upstream) noexcept
    : upstream_(upstream)
{
}


inline void* TracingMemoryResource::do_allocate(std::size_t bytes,
                                                std::size_t alignment)
{
    println("  [trace] allocate {} bytes (align {})", bytes, alignment);
    return upstream_->allocate(bytes, alignment);
}


inline void TracingMemoryResource::do_deallocate(void* p, std::size_t bytes,
                                                 std::size_t alignment) noexcept
{
    println("  [trace] deallocate {} bytes (align {})", bytes, alignment);
    upstream_->deallocate(p, bytes, alignment);
}


inline bool TracingMemoryResource::do_is_equal(
    std::pmr::memory_resource const& other) const noexcept
{
    return this == &other;
}


int main()
{
    Pool pool;

    println("=== PoolMemoryResource (std::pmr) demo ===\n");

    // 1. Polymorphic resource + pmr::vector
    println("1. PoolMemoryResource + std::pmr::vector<int>:");
    {
        PoolMemoryResource resource(pool);
        std::pmr::vector<int> v{&resource};
        for (int i = 0; i < 8; ++i)
        {
            v.push_back(i * 10);
        }
        print("   values:");
        for (int x : v)
        {
            print(" {}", x);
        }
        println("");
        println("   live allocations: {}", pool.live_allocation_count());
        v.clear();
    }
    println("   vector gone; live allocations: {}\n", pool.live_allocation_count());

    // 2. Contrast with the type-based PoolAllocator path (preferred when T can
    //    bake the allocator in — no virtual dispatch on every allocate).
    println("2. Side-by-side: PoolAllocator<T> + std::vector (type-based):");
    {
        PoolAllocator<int> alloc(pool);
        std::vector<int, PoolAllocator<int>> v(alloc);
        for (int i = 0; i < 8; ++i)
        {
            v.push_back(i * 10);
        }
        print("   values:");
        for (int x : v)
        {
            print(" {}", x);
        }
        println("");
        println("   Prefer this path when the container type can carry the");
        println("   allocator; use PoolMemoryResource when you need pmr /");
        println("   runtime composition (virtual allocate/deallocate).");
    }
    println("");

    // 3. Tracing decorator over PoolMemoryResource
    println("3. In-demo TracingMemoryResource over PoolMemoryResource:");
    {
        PoolMemoryResource pool_resource(pool);
        TracingMemoryResource tracing(&pool_resource);
        std::pmr::vector<int> v{&tracing};
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);
        println("   sum = {}", v[0] + v[1] + v[2]);
        v.clear();
    }

    println("\n=== Demo completed ===");
    return 0;
}
