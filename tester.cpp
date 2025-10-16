#include <cstdlib>
#include <gtest/gtest.h>

#include "spallocator/helper.hpp"
#include "spallocator/slab.hpp"


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
        auto item = slab.allocateItem();
        EXPECT_NE(item, nullptr);
        items.push_back(item);
    }

    // next allocation should cause a new slab to be allocated
    auto item = slab.allocateItem();
    EXPECT_NE(item, nullptr);
    items.push_back(item);

    EXPECT_EQ(slab.getAllocatedMemory(), 8_KB);

    for (int i = 0; i < 31; ++i)
    {
        auto item = slab.allocateItem();
        EXPECT_NE(item, nullptr);
        items.push_back(item);
    }

    // next allocation should cause a new slab to be allocated
    item = slab.allocateItem();
    EXPECT_NE(item, nullptr);
    items.push_back(item);

    EXPECT_EQ(slab.getAllocatedMemory(), 12_KB);

    // free all items
    for (auto it : items)
    {
        slab.freeItem(it);
    }

    // allocate again, should reuse freed items
    for (int i = 0; i < 65; ++i)
    {
        auto item = slab.allocateItem();
        EXPECT_NE(item, nullptr);
    }

    // same memory allocation should persist from previous allocations
    EXPECT_EQ(slab.getAllocatedMemory(), 12_KB);
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
