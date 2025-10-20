/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2025, Michael VanLoon
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <cstdlib>
#include <gtest/gtest.h>

#include "spallocator/helper.hpp"
#include "spallocator/spinlock.hpp"
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

    std::map<std::size_t, std::byte*> items;

    // allocate various sizes
    for (std::size_t size : {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                             160, 192, 224, 256, 320, 384, 448, 512, 640, 768,
                             896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
                             3584, 4096, 4512, 768, 1024, 1500, 2000, 3000,
                             4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items[size] = item;
    }

    // free all items
    for (auto it : items)
    {
        println("Deallocating item of size {} at ptr={}", it.first, static_cast<void*>(it.second));
        pool.deallocate(it.second);
    }

    items.clear();

    // allocate again to ensure reuse of freed items
    for (std::size_t size : {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                             160, 192, 224, 256, 320, 384, 448, 512, 640, 768,
                             896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
                             3584, 4096, 4512, 768, 1024, 1500, 2000, 3000,
                             4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items[size] = item;
    }

    // free all items again
    for (auto it : items)
    {
        println("Deallocating item of size {} at ptr={}", it.first, static_cast<void*>(it.second));
        pool.deallocate(it.second);
    }
}


TEST(SpinLockTest, BasicLocking)
{
    int counter = 0;

    SpinLock lock;

    ++counter;
    lock.lock();
    // critical section
    EXPECT_EQ(++counter, 2);
    lock.unlock();

    // try_lock should succeed when not locked
    EXPECT_EQ(++counter, 3);
    lock.try_lock();
    // critical section
    EXPECT_EQ(++counter, 4);
    lock.unlock();

    // lock again
    EXPECT_EQ(++counter, 5);
    lock.lock();
    // critical section
    EXPECT_EQ(++counter, 6);        
    lock.unlock();

    EXPECT_EQ(++counter, 7);
}


//
// NOTE: This test may occasionally fail due to timing issues. These are
// clearly intentional race conditions with highly probabilistic outcomes.
//
TEST(SpinLockTest, Backoff)
{
    int tested_value = 0x55555555;

    SpinLock lock;

    lock.lock();

    // Start a thread that will attempt to acquire the lock
    std::thread t([&lock, &tested_value]() {
        lock.lock();
        // critical section
        EXPECT_EQ(tested_value, 0xAAAAAAAA);
        tested_value ^= 0xFFFFFFFF;
        EXPECT_EQ(tested_value, 0x55555555);
        lock.unlock();
    });

    EXPECT_EQ(tested_value, 0x55555555);
    tested_value ^= 0xFFFFFFFF;
    EXPECT_EQ(tested_value, 0xAAAAAAAA);

    // Sleep for a short duration to ensure the other thread attempts to lock
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(tested_value, 0xAAAAAAAA);

    // Unlock the main thread's lock
    lock.unlock();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(tested_value, 0x55555555);
    tested_value ^= 0xFFFFFFFF;
    EXPECT_EQ(tested_value, 0xAAAAAAAA);

    // Wait for the other thread to finish
    t.join();
}


TEST(SpinLockTest, TryLockContention)
{
    int tested_value = 0;

    SpinLock lock;

    lock.lock();

    // Start a thread that will attempt to acquire the lock using try_lock
    std::thread t([&lock, &tested_value]() {
        while (!lock.try_lock())
        {
            // failed to acquire lock, yield and try again
            std::this_thread::yield();
        }
        // critical section
        tested_value = 42;
        lock.unlock();
    });

    // Sleep for a short duration to ensure the other thread attempts to lock
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(tested_value, 0);

    // Unlock the main thread's lock
    lock.unlock();

    // Wait for the other thread to finish
    t.join();

    EXPECT_EQ(tested_value, 42);
}


TEST(SpinLockTest, LockContention)
{
    int tested_value = 0;

    SpinLock lock;

    lock.lock();

    // Start a thread that will attempt to acquire the lock
    std::thread t([&lock, &tested_value]() {
        lock.lock();
        // critical section
        tested_value = 99;
        lock.unlock();
    });

    // Sleep for a short duration to ensure the other thread attempts to lock
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(tested_value, 0);

    // Unlock the main thread's lock
    lock.unlock();

    // Wait for the other thread to finish
    t.join();

    EXPECT_EQ(tested_value, 99);
}


TEST(SpinLockTest, MultipleThreads)
{
    constexpr int num_threads = 10;
    constexpr int increments_per_thread = 1000;

    int counter = 0;
    SpinLock lock;

    auto worker = [&counter, &lock]() {
        for (int i = 0; i < increments_per_thread; ++i)
        {
            lock.lock();
            ++counter;
            lock.unlock();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(worker);
    }

    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(counter, num_threads * increments_per_thread);
}


//
// Use RAII locking with std::scoped_lock and std::unique_lock
//
TEST(SpinLockTest, stdLock)
{
    int counter = 0;
    SpinLock lock;

    {
        std::scoped_lock<SpinLock> slock(lock);
        ++counter;
        EXPECT_EQ(counter, 1);
    } // slock goes out of scope and unlocks

    {
        std::scoped_lock<SpinLock> slock(lock);
        ++counter;
        EXPECT_EQ(counter, 2);
    } // slock goes out of scope and unlocks

    EXPECT_EQ(counter, 2);

    // with thread, starting unlocked
    {
        counter = 0;
        std::thread t([&counter, &lock]() {
            std::scoped_lock<SpinLock> slock(lock);
            ++counter;
            EXPECT_EQ(counter, 1);
        });

        EXPECT_EQ(counter, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(counter, 1);

        {
            std::scoped_lock<SpinLock> slock(lock);
            ++counter;
            EXPECT_EQ(counter, 2);
        } // slock goes out of scope and unlocks

        t.join();
        EXPECT_EQ(counter, 2);
    }

    // with thread, starting locked
    {
        counter = 0;
        std::unique_lock<SpinLock> ulock(lock); // main thread locks

        std::thread t([&counter, &lock]() {
            std::unique_lock<SpinLock> ulock(lock);
            ++counter;
            EXPECT_EQ(counter, 1);
        });

        EXPECT_EQ(counter, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(counter, 0);

        ulock.unlock(); // main thread unlocks
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(counter, 1);

        {
            std::unique_lock<SpinLock> ulock(lock);
            ++counter;
            EXPECT_EQ(counter, 2);
        } // ulock goes out of scope and unlocks

        t.join();
        EXPECT_EQ(counter, 2);
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
