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
    }

    {
        spallocator::Slab<128> slab;
        EXPECT_EQ(slab.getElemSize(), 128);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
    }

    {
        spallocator::Slab<1_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 1_KB);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
    }

    {
        spallocator::Slab<2_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 2_KB);
        EXPECT_EQ(slab.getAllocSize(), 8_KB);
    }

    {
        spallocator::Slab<16_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 16_KB);
        EXPECT_EQ(slab.getAllocSize(), 64_KB);
    }

    {
        // not an even multiple of 1024
        spallocator::Slab<12345> slab;
        EXPECT_EQ(slab.getElemSize(), 12345);
        EXPECT_EQ(slab.getAllocSize(), 49380);
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
