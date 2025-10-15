#include <cstdlib>
#include <gtest/gtest.h>

#include "spallocator/helper.hpp"
#include "spallocator/slab.hpp"


TEST(SlabTest, CreateSmallSlab)
{
    spallocator::Slab<64> small_slab;
    EXPECT_EQ(small_slab.getElemSize(), 64);
    EXPECT_EQ(small_slab.getSlabAllocSize(), 4096);
}

TEST(SlabTest, CreateLargeSlab)
{
    spallocator::Slab<8_K> large_slab;
    EXPECT_EQ(large_slab.getElemSize(), 8_K);
    EXPECT_EQ(large_slab.getSlabAllocSize(), 32_K);
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
