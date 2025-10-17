#include <cstdlib>
#include <gtest/gtest.h>

#include "spallocator/helper.hpp"
#include "spallocator/slab.hpp"
#include "spallocator/pool.hpp"


TEST(SlabTest, CreateSlabs)
{
    {
        spallocator::Slab<64> slab;
        EXPECT_EQ(slab.getElemSize(), 64);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 4_KB);
    }

    {
        spallocator::Slab<128> slab;
        EXPECT_EQ(slab.getElemSize(), 128);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 4_KB);
    }

    {
        spallocator::Slab<1_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 1_KB);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 4_KB);
    }

    {
        spallocator::Slab<2_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 2_KB);
        EXPECT_EQ(slab.getAllocSize(), 8_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 8_KB);
    }

    {
        spallocator::Slab<16_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 16_KB);
        EXPECT_EQ(slab.getAllocSize(), 64_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 64_KB);
    }

    {
        // not an even multiple of 1024
        spallocator::Slab<12345> slab;
        EXPECT_EQ(slab.getElemSize(), 12345);
        EXPECT_EQ(slab.getAllocSize(), 49380);
        EXPECT_EQ(slab.getAllocatedMemory(), 49380);
    }

}


TEST(SlabTest, AllocateItems)
{
    spallocator::Slab<128> slab;

    // initial allocation should be 4KB
    EXPECT_EQ(slab.getAllocatedMemory(), 4_KB);

    std::vector<std::byte*> items;
    for (int i = 0; i < 32; ++i)
    {
        auto item = slab.allocateItem(120);
        EXPECT_NE(item, nullptr);
        items.push_back(item);
    }

    // next allocation should cause a new slab to be allocated
    auto item = slab.allocateItem(120);
    EXPECT_NE(item, nullptr);
    items.push_back(item);

    EXPECT_EQ(slab.getAllocatedMemory(), 8_KB);

    for (int i = 0; i < 31; ++i)
    {
        auto item = slab.allocateItem(120);
        EXPECT_NE(item, nullptr);
        items.push_back(item);
    }

    // next allocation should cause a new slab to be allocated
    item = slab.allocateItem(120);
    EXPECT_NE(item, nullptr);
    items.push_back(item);

    EXPECT_EQ(slab.getAllocatedMemory(), 12_KB);

    // free all items
    for (auto it : items)
    {
        slab.deallocateItem(it);
    }

    // allocate again, should reuse freed items
    for (int i = 0; i < 65; ++i)
    {
        auto item = slab.allocateItem(120);
        EXPECT_NE(item, nullptr);
    }

    // same memory allocation should persist from previous allocations
    EXPECT_EQ(slab.getAllocatedMemory(), 12_KB);
}


TEST(PoolTest, Selector)
{
    spallocator::Pool pool;

    EXPECT_EQ(pool.selectSlab(16), 0);
    EXPECT_EQ(pool.selectSlab(32), 1);
    EXPECT_EQ(pool.selectSlab(48), 2);
    EXPECT_EQ(pool.selectSlab(64), 3);
    EXPECT_EQ(pool.selectSlab(96), 4);
    EXPECT_EQ(pool.selectSlab(128), 5);
    EXPECT_EQ(pool.selectSlab(192), 6);
    EXPECT_EQ(pool.selectSlab(256), 7);
    EXPECT_EQ(pool.selectSlab(384), 8);
    EXPECT_EQ(pool.selectSlab(512), 9);
    EXPECT_EQ(pool.selectSlab(768), 10);
    EXPECT_EQ(pool.selectSlab(1024), 11);
    EXPECT_EQ(pool.selectSlab(1500), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(2000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(3000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(4000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(5000), std::numeric_limits<std::size_t>::max());

    EXPECT_EQ(pool.selectSlab(1), 0);
    EXPECT_EQ(pool.selectSlab(15), 0);
    EXPECT_EQ(pool.selectSlab(17), 1);
    EXPECT_EQ(pool.selectSlab(31), 1);
    EXPECT_EQ(pool.selectSlab(33), 2);
    EXPECT_EQ(pool.selectSlab(47), 2);
    EXPECT_EQ(pool.selectSlab(49), 3);
    EXPECT_EQ(pool.selectSlab(63), 3);
    EXPECT_EQ(pool.selectSlab(65), 4);
    EXPECT_EQ(pool.selectSlab(95), 4);
    EXPECT_EQ(pool.selectSlab(97), 5);
    EXPECT_EQ(pool.selectSlab(127), 5);
    EXPECT_EQ(pool.selectSlab(129), 6);
    EXPECT_EQ(pool.selectSlab(191), 6);
    EXPECT_EQ(pool.selectSlab(193), 7);
    EXPECT_EQ(pool.selectSlab(255), 7);
    EXPECT_EQ(pool.selectSlab(257), 8);
    EXPECT_EQ(pool.selectSlab(383), 8);
    EXPECT_EQ(pool.selectSlab(385), 9);
    EXPECT_EQ(pool.selectSlab(511), 9);
    EXPECT_EQ(pool.selectSlab(513), 10);
    EXPECT_EQ(pool.selectSlab(767), 10);
    EXPECT_EQ(pool.selectSlab(769), 11);
    EXPECT_EQ(pool.selectSlab(1023), 11);
    EXPECT_EQ(pool.selectSlab(1025), std::numeric_limits<std::size_t>::max());
}

TEST(PoolTest, AllocateItems)
{
    spallocator::Pool pool;

    std::vector<std::byte*> items;

    // allocate various sizes
    for (std::size_t size : {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
                             1500, 2000, 3000, 4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items.push_back(item);
    }

    // free all items
    for (auto it : items)
    {
        pool.deallocate(it);
    }

    items.clear();

    // allocate again to ensure reuse of freed items
    for (std::size_t size : {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024,
                             1500, 2000, 3000, 4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items.push_back(item);
    }

    // free all items again
    for (auto it : items)
    {
        pool.deallocate(it);
    }
}


void pre_test()
{
    println("Running pre-test setup...");
}


int main(int argc, char** argv)
{
    pre_test();

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
