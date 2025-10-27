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
#include "spallocator/lifetimeobserver.hpp"
#include "spallocator/spallocator.hpp"

using namespace std::literals;
using namespace spallocator;


TEST(SlabTest, CreateSlabs)
{
    {
        Slab<64> slab;
        EXPECT_EQ(slab.getElemSize(), 64u);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 4_KB);
    }

    {
        Slab<128> slab;
        EXPECT_EQ(slab.getElemSize(), 128u);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 4_KB);
    }

    {
        Slab<1_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 1_KB);
        EXPECT_EQ(slab.getAllocSize(), 4_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 4_KB);
    }

    {
        Slab<2_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 2_KB);
        EXPECT_EQ(slab.getAllocSize(), 8_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 8_KB);
    }

    {
        Slab<16_KB> slab;
        EXPECT_EQ(slab.getElemSize(), 16_KB);
        EXPECT_EQ(slab.getAllocSize(), 64_KB);
        EXPECT_EQ(slab.getAllocatedMemory(), 64_KB);
    }

    {
        // not an even multiple of 1024
        Slab<12336> slab;
        EXPECT_EQ(slab.getElemSize(), 12336u);
        EXPECT_EQ(slab.getAllocSize(), 49344u);
        EXPECT_EQ(slab.getAllocatedMemory(), 49344u);
    }

}


TEST(SlabTest, AllocateItems)
{
    Slab<128> slab;

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


TEST(SlabTest, DeallocateInvalidItem)
{
    Slab<256> slab;

    auto item1 = slab.allocateItem(200);
    EXPECT_NE(item1, nullptr);

    // invalid pointer (not from this slab)
    std::byte* invalid_item = reinterpret_cast<std::byte*>(std::malloc(256));
    EXPECT_THROW(slab.deallocateItem(invalid_item), std::invalid_argument);
    std::free(invalid_item);

    // double free
    slab.deallocateItem(item1);
    EXPECT_THROW(slab.deallocateItem(item1), std::invalid_argument);
}


TEST(SlabTest, Alignment)
{
    Slab<64> slab;

    std::vector<std::byte*> items;
    for (int i = 0; i < 10; ++i)
    {
        auto item = slab.allocateItem(60);
        EXPECT_NE(item, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(item) % 16, 0u); // 16-byte alignment
        items.push_back(item);
    }

    for (auto it : items)
    {
        slab.deallocateItem(it);
    }
}


TEST(PoolTest, Selector)
{
    Pool pool;

    EXPECT_EQ(pool.selectSlab(16), 0u);
    EXPECT_EQ(pool.selectSlab(32), 1u);
    EXPECT_EQ(pool.selectSlab(48), 2u);
    EXPECT_EQ(pool.selectSlab(64), 3u);
    EXPECT_EQ(pool.selectSlab(96), 4u);
    EXPECT_EQ(pool.selectSlab(128), 5u);
    EXPECT_EQ(pool.selectSlab(192), 6u);
    EXPECT_EQ(pool.selectSlab(256), 7u);
    EXPECT_EQ(pool.selectSlab(384), 8u);
    EXPECT_EQ(pool.selectSlab(512), 9u);
    EXPECT_EQ(pool.selectSlab(768), 10u);
    EXPECT_EQ(pool.selectSlab(1024), 11u);
    EXPECT_EQ(pool.selectSlab(1500), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(2000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(3000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(4000), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(pool.selectSlab(5000), std::numeric_limits<std::size_t>::max());

    EXPECT_EQ(pool.selectSlab(1), 0u);
    EXPECT_EQ(pool.selectSlab(15), 0u);
    EXPECT_EQ(pool.selectSlab(17), 1u);
    EXPECT_EQ(pool.selectSlab(31), 1u);
    EXPECT_EQ(pool.selectSlab(33), 2u);
    EXPECT_EQ(pool.selectSlab(47), 2u);
    EXPECT_EQ(pool.selectSlab(49), 3u);
    EXPECT_EQ(pool.selectSlab(63), 3u);
    EXPECT_EQ(pool.selectSlab(65), 4u);
    EXPECT_EQ(pool.selectSlab(95), 4u);
    EXPECT_EQ(pool.selectSlab(97), 5u);
    EXPECT_EQ(pool.selectSlab(127), 5u);
    EXPECT_EQ(pool.selectSlab(129), 6u);
    EXPECT_EQ(pool.selectSlab(191), 6u);
    EXPECT_EQ(pool.selectSlab(193), 7u);
    EXPECT_EQ(pool.selectSlab(255), 7u);
    EXPECT_EQ(pool.selectSlab(257), 8u);
    EXPECT_EQ(pool.selectSlab(383), 8u);
    EXPECT_EQ(pool.selectSlab(385), 9u);
    EXPECT_EQ(pool.selectSlab(511), 9u);
    EXPECT_EQ(pool.selectSlab(513), 10u);
    EXPECT_EQ(pool.selectSlab(767), 10u);
    EXPECT_EQ(pool.selectSlab(769), 11u);
    EXPECT_EQ(pool.selectSlab(1023), 11u);
    EXPECT_EQ(pool.selectSlab(1025), std::numeric_limits<std::size_t>::max());
}


TEST(PoolTest, AllocateItems)
{
    Pool pool;

    std::map<std::size_t, std::byte*> items;

    // allocate various sizes
    for (std::size_t size : {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                             160, 192, 224, 256, 320, 384, 448, 512, 640, 768,
                             896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
                             3584, 4096, 4512, 1500, 2000, 3000,
                             4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items[size] = item;
    }

    // free all items
    for (auto it : items)
    {
        pool.deallocate(it.second);
    }

    items.clear();

    // allocate again to ensure reuse of freed items
    for (std::size_t size : {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                             160, 192, 224, 256, 320, 384, 448, 512, 640, 768,
                             896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
                             3584, 4096, 4512, 1500, 2000, 3000,
                             4000, 5000, 8000, 16000, 32000})
    {
        auto item = pool.allocate(size);
        EXPECT_NE(item, nullptr);
        items[size] = item;
    }

    // free all items again
    for (auto it : items)
    {
        pool.deallocate(it.second);
    }
}


TEST(PoolTest, uniqueBytePoolPtr)
{
    Pool pool;

    // Create the deleter lambda
    auto deleter = [&pool](std::byte* p) {
        pool.deallocate(p);
    };

    using unique_byte_pool_ptr = std::unique_ptr<std::byte, decltype(deleter)>;

    {
        unique_byte_pool_ptr item1(pool.allocate(128), deleter);
        EXPECT_NE(item1.get(), nullptr);

        unique_byte_pool_ptr item2(pool.allocate(2048), deleter);
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item3 = pool.allocate(512);
    EXPECT_NE(item3, nullptr);
    pool.deallocate(item3);

    using shared_byte_pool_ptr = std::shared_ptr<std::byte>;

    {
        shared_byte_pool_ptr item1(pool.allocate(256),
            [&pool](std::byte* p) { pool.deallocate(p); });
        EXPECT_NE(item1.get(), nullptr);

        shared_byte_pool_ptr item2(pool.allocate(4096),
            [&pool](std::byte* p) { pool.deallocate(p); });
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item4 = pool.allocate(1024);
    EXPECT_NE(item4, nullptr);
    pool.deallocate(item4);
}


TEST(PoolTest, sharedBytePoolPtr)
{
    Pool pool;

    // Create the deleter lambda
    auto deleter = [&pool](std::byte* p) {
        pool.deallocate(p);
    };

    using shared_byte_pool_ptr = std::shared_ptr<std::byte>;

    {
        shared_byte_pool_ptr item1(pool.allocate(128), deleter);
        EXPECT_NE(item1.get(), nullptr);

        shared_byte_pool_ptr item2(pool.allocate(2048), deleter);
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item3 = pool.allocate(512);
    EXPECT_NE(item3, nullptr);
    pool.deallocate(item3);

    using shared_byte_pool_ptr = std::shared_ptr<std::byte>;

    {
        shared_byte_pool_ptr item1(pool.allocate(256), deleter);
        EXPECT_NE(item1.get(), nullptr);

        shared_byte_pool_ptr item2(pool.allocate(4096), deleter);
        EXPECT_NE(item2.get(), nullptr);
    } // items go out of scope and are deallocated

    // allocate again to ensure no issues
    auto item4 = pool.allocate(1024);
    EXPECT_NE(item4, nullptr);
    pool.deallocate(item4);
}


TEST(PoolTest, uniquePoolPtr)
{
    Pool pool;

    {
        auto item1 = make_pool_unique<int>(pool, 128);
        EXPECT_NE(item1.get(), nullptr);
        EXPECT_EQ(*item1, 128);

        auto item2 = make_pool_unique<int>(pool);
        EXPECT_NE(item2.get(), nullptr);
        EXPECT_EQ(*item2, 0);
        *item2 = 42;
        EXPECT_EQ(*item2, 42);

        auto item3 = make_pool_unique<int[]>(pool, 10);
        EXPECT_NE(item3.get(), nullptr);

        for (int i = 0; i < 10; ++i)
        {
            item3[i] = i * 10;
        }

        for (int i = 0; i < 10; ++i)
        {
            EXPECT_EQ(item3[i], i * 10);
        }
    } // items go out of scope and are deallocated
}


TEST(PoolTest, sharedPoolPtr)
{
    Pool pool;

    {
        auto item1 = make_pool_shared<int>(pool, 128);
        EXPECT_NE(item1.get(), nullptr);
        EXPECT_EQ(*item1, 128);

        auto item2 = make_pool_shared<int>(pool);
        EXPECT_NE(item2.get(), nullptr);
        EXPECT_EQ(*item2, 0);
        *item2 = 42;
        EXPECT_EQ(*item2, 42);

        auto item3 = make_pool_shared<int[]>(pool, 10);
        EXPECT_NE(item3.get(), nullptr);

        for (int i = 0; i < 10; ++i)
        {
            item3[i] = i * 10;
        }

        for (int i = 0; i < 10; ++i)
        {
            EXPECT_EQ(item3[i], i * 10);
        }

        item1 = item2;
        EXPECT_EQ(*item1, 42);
        EXPECT_EQ(*item2, 42);
        *item1 = 84;

        auto item4 = item3;
        for (int i = 0; i < 10; ++i)
        {
            EXPECT_EQ(item4[i], i * 10);
        }

        auto item5 = item1;
        EXPECT_EQ(*item5, 84);
        EXPECT_EQ(*item1, 84);
        EXPECT_EQ(*item2, 84);

        *item5 = 99;
        EXPECT_EQ(*item1, 99);
        EXPECT_EQ(*item2, 99);
        EXPECT_EQ(*item5, 99);
    } // items go out of scope and are deallocated
}


TEST(PoolTest, Alignment)
{
    Pool pool;

    for (size_t align : { 4, 8, 16 })
    {
        std::vector<std::byte*> items;
        for (int i = 0; i <= 64; ++i)
        {
            for (size_t size = 1; size <= 128; ++size)
            {
                auto item = pool.allocate(size, align);
                EXPECT_NE(item, nullptr);
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(item) % align, 0u); // align-byte alignment
                items.push_back(item);
            }
        }

        for (auto it : items)
        {
            pool.deallocate(it);
        }
    }
}


TEST(PoolTest, MultiThreadTest)
{
    std::size_t const num_cores = std::thread::hardware_concurrency();
    println("Detected {} hardware threads", num_cores);
    ASSERT_GT(num_cores, 0u);
    const std::size_t num_threads = num_cores * 8 / 10;

    Pool pool;

    constexpr int allocations_per_thread = 10000;

    std::random_device rd;

    auto worker = [&pool, &rd]() {
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, 1000);
        std::vector<std::byte*> items;
        for (int i = 0; i < allocations_per_thread; ++i)
        {
            size_t size = 16 + dist(gen);
            auto item = pool.allocate(size);
            EXPECT_NE(item, nullptr);
            items.push_back(item);
        }

        for (auto it : items)
        {
            pool.deallocate(it);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(worker);
    }

    for (auto& t : threads)
    {
        t.join();
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
    uint32_t tested_value = 0x55555555;

    SpinLock lock;

    lock.lock();

    // Start a thread that will attempt to acquire the lock
    std::thread t([&lock, &tested_value]() {
        lock.lock();
        // critical section
        EXPECT_EQ(tested_value, 0xAAAAAAAAu);
        tested_value ^= 0xFFFFFFFFu;
        EXPECT_EQ(tested_value, 0x55555555u);
        lock.unlock();
    });

    EXPECT_EQ(tested_value, 0x55555555u);
    tested_value ^= 0xFFFFFFFFu;
    EXPECT_EQ(tested_value, 0xAAAAAAAAu);

    // Sleep for a short duration to ensure the other thread attempts to lock
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(tested_value, 0xAAAAAAAAu);

    // Unlock the main thread's lock
    lock.unlock();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    lock.lock();
    EXPECT_EQ(tested_value, 0x55555555u);
    tested_value ^= 0xFFFFFFFFu;
    EXPECT_EQ(tested_value, 0xAAAAAAAAu);
    lock.unlock();

    // Wait for the other thread to finish
    t.join();
    EXPECT_EQ(tested_value, 0xAAAAAAAAu);
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

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        {
            std::scoped_lock<SpinLock> slock(lock);
            EXPECT_EQ(counter, 1);
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

        {
            std::unique_lock<SpinLock> ulock(lock);
            EXPECT_EQ(counter, 1);
            ++counter;
            EXPECT_EQ(counter, 2);
        } // ulock goes out of scope and unlocks

        t.join();
        EXPECT_EQ(counter, 2);
    }
}


TEST(SpinLockTest, ManyThreads)
{
    std::size_t const num_cores = std::thread::hardware_concurrency();
    println("Detected {} hardware threads", num_cores);
    ASSERT_GT(num_cores, 0u);
    const std::size_t num_threads = num_cores * 8 / 10;

    constexpr std::size_t increments_per_thread = 10000;

    uint32_t counter = 0;
    SpinLock lock;

    // lock initially to ensure all threads contend
    std::unique_lock<SpinLock> ulock(lock); // lock for setup

    auto worker = [&counter, &lock]() {
        for (size_t i = 0; i < increments_per_thread; ++i)
        {
            std::scoped_lock<SpinLock> slock(lock);
            ++counter;
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(worker);
    }

    EXPECT_EQ(counter, 0u);
    ulock.unlock(); // unlock to allow threads to proceed

    for (auto& t : threads)
    {
        t.join();
    }

    EXPECT_EQ(counter, num_threads * increments_per_thread);
}


TEST(LifetimeObserverTest, SimpleObjectOwnership)
{
    class TestObject: public LifetimeObserver
    {
    public:
        TestObject() = default;
        ~TestObject() = default;
    };

    {
        TestObject obj;
        EXPECT_EQ(obj.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj.getCount(TestObject::e_refType::observer), 0);
        {
            LifetimeObserver observer = obj.getObserver(); // weak reference
            EXPECT_TRUE(observer.isAlive());
            EXPECT_EQ(obj.getCount(TestObject::e_refType::owner), 1);
            EXPECT_EQ(obj.getCount(TestObject::e_refType::observer), 1);
            EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
            EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);
        }
        EXPECT_TRUE(obj.isAlive());
        EXPECT_EQ(obj.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj.getCount(TestObject::e_refType::observer), 0);
    }

    {
        TestObject* obj = new TestObject();
        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 0);

        LifetimeObserver observer = obj->getObserver(); // weak reference
        EXPECT_TRUE(obj->isAlive());
        EXPECT_TRUE(observer.isAlive());
        EXPECT_TRUE(observer);

        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 1);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);

        delete obj;
        EXPECT_FALSE(observer.isAlive());
        EXPECT_FALSE(observer);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 0);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);
    }
}

/*
TEST(LifetimeObserverTest, MoveSemantics)
{
    class TestObject: public LifetimeObserver
    {
    public:
        TestObject() = default;
        ~TestObject() = default;
    };

    {
        TestObject obj1;
        EXPECT_EQ(obj1.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1.getCount(TestObject::e_refType::observer), 0);

        LifetimeObserver obj2 = std::move(obj1);
        EXPECT_FALSE(obj1.isAlive());
        EXPECT_TRUE(obj2.isAlive());

        EXPECT_EQ(obj2.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj2.getCount(TestObject::e_refType::observer), 0);
    }
}
*/

TEST(LifetimeObserverTest, CopySemantics)
{
    struct TestObject: public LifetimeObserver
    {
        TestObject() = default;
        TestObject(int value) : i(value) {}
        ~TestObject() = default;

        int i = 0;
    };

    {
        TestObject* obj1 = new TestObject{1};
        EXPECT_EQ(obj1->i, 1);
        EXPECT_TRUE(obj1->isAlive());
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::observer), 0);

        TestObject* obj2 = new TestObject{2};
        EXPECT_EQ(obj2->i, 2);
        EXPECT_TRUE(obj2->isAlive());
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::observer), 0);

        LifetimeObserver observer(obj1->getObserver());
        EXPECT_TRUE(obj1->isAlive());
        EXPECT_TRUE(observer.isAlive());
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::observer), 1);
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::observer), 0);

        {
            LifetimeObserver observer2(obj2->getObserver());
            EXPECT_TRUE(obj2->isAlive());
            EXPECT_TRUE(observer2.isAlive());
            EXPECT_TRUE(observer.isAlive());

            EXPECT_EQ(obj2->getCount(TestObject::e_refType::owner), 1);
            EXPECT_EQ(obj2->getCount(TestObject::e_refType::observer), 1);
            EXPECT_EQ(observer2.getCount(TestObject::e_refType::owner), 1);
            EXPECT_EQ(observer2.getCount(TestObject::e_refType::observer), 1);
            EXPECT_EQ(obj1->getCount(TestObject::e_refType::owner), 1);
            EXPECT_EQ(obj1->getCount(TestObject::e_refType::observer), 1);
            EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
            EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);
        }
        // observer2 goes out of scope

        EXPECT_TRUE(obj1->isAlive());
        EXPECT_TRUE(observer.isAlive());
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::observer), 1);
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::observer), 0);

        // obj2 becomes a fully unique, owned copy of obj1. Unique meaning
        // they do not share ownership of the same underlying object; they
        // uniquely own separate objects.
        *obj2 = *obj1; // copy assignment
        EXPECT_EQ(obj1->i, 1);
        EXPECT_EQ(obj2->i, 1);
        EXPECT_TRUE(obj1->isAlive());
        EXPECT_TRUE(obj2->isAlive());
        EXPECT_TRUE(observer.isAlive());
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::observer), 1);
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj2->getCount(TestObject::e_refType::observer), 0);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);

        TestObject* obj3 = new TestObject{3};
        EXPECT_EQ(obj3->i, 3);
        EXPECT_TRUE(obj3->isAlive());
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::observer), 0);

        *obj3 = *obj1; // copy assignment
        EXPECT_EQ(obj1->i, 1);
        EXPECT_EQ(obj3->i, 1);
        EXPECT_TRUE(obj1->isAlive());
        EXPECT_TRUE(obj3->isAlive());
        EXPECT_TRUE(observer.isAlive());
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::observer), 1);
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::observer), 0);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);

        delete obj2;
        EXPECT_TRUE(obj1->isAlive());
        EXPECT_TRUE(observer.isAlive());
        EXPECT_TRUE(obj3->isAlive());
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1->getCount(TestObject::e_refType::observer), 1);
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::observer), 0);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);

        delete obj1;
        EXPECT_FALSE(observer.isAlive());
        EXPECT_TRUE(obj3->isAlive());
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 0);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj3->getCount(TestObject::e_refType::observer), 0);

        delete obj3;
        EXPECT_FALSE(observer.isAlive());
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 0);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);
    }

    {
        TestObject obj1;
        EXPECT_EQ(obj1.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1.getCount(TestObject::e_refType::observer), 0);

        LifetimeObserver observer(obj1.getObserver()); // weak reference
        EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 1);

        LifetimeObserver observer2(obj1); // weak reference
        EXPECT_EQ(observer2.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(observer2.getCount(TestObject::e_refType::observer), 2);

        LifetimeObserver observer3 = obj1.getObserver(); // weak reference
        EXPECT_EQ(observer3.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(observer3.getCount(TestObject::e_refType::observer), 3);

        //LifetimeObserver observer4 = obj1; // weak reference
        //EXPECT_EQ(observer4.getCount(TestObject::e_refType::owner), 1);
        //EXPECT_EQ(observer4.getCount(TestObject::e_refType::observer), 4);
    }
}

/*
TEST(LifetimeObserverTest, SelfAssignment)
{
    class TestObject: public LifetimeObserver
    {
    public:
        TestObject() = default;
        ~TestObject() = default;
    };

    {
        TestObject obj1;
        EXPECT_EQ(obj1.getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj1.getCount(TestObject::e_refType::observer), 0);

        obj1 = obj1; // self-assignment
        EXPECT_TRUE(obj1.isAlive());
    }
}
*/

TEST(LifetimeObserverTest, TestSimpleCallback)
{
    class TestObject: public LifetimeObserver
    {
    public:
        TestObject() = default;
        ~TestObject() = default;
    };

    struct TestObjHolder
    {
        TestObjHolder(TestObject* p): obj(p), obj_lifetime(p->getObserver()) { }
        TestObjHolder() = delete;
        ~TestObjHolder() = default;

        bool testAlive(bool assumeAlive)
        {
            bool objIsAlive = false;

            // We're holding a weak reference to the object via obj_lifetime.
            // Check if the object is still alive before using it.
            if (obj_lifetime.isAlive())
            {
                // Object is still alive and valid. We can safely use it.
                objIsAlive = obj->isAlive();
            }

            EXPECT_EQ(objIsAlive, assumeAlive);
            return objIsAlive;
        }

    private:
        TestObject* obj;
        LifetimeObserver obj_lifetime;
    };

    {
        TestObject* obj = new TestObject();
        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 0);

        TestObjHolder holder(obj);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 1);

        EXPECT_TRUE(holder.testAlive(true));

        {
            LifetimeObserver observer = obj->getObserver(); // weak reference
            EXPECT_EQ(observer.getCount(TestObject::e_refType::owner), 1);
            EXPECT_EQ(observer.getCount(TestObject::e_refType::observer), 2);
            EXPECT_TRUE(observer.isAlive());
            EXPECT_TRUE(holder.testAlive(true));
        }
        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 1);

        EXPECT_TRUE(holder.testAlive(true));

        delete obj;

        // Emulate a callback on an interface that is holding a weak reference
        // to our object after the object has been deleted.
        EXPECT_FALSE(holder.testAlive(false));
    }
}


TEST(LifetimeObserverTest, TestDeferredCallback)
{
    class TestObject: public LifetimeObserver
    {
    public:
        TestObject() = default;
        ~TestObject() = default;

        int incValue() { return ++i; }

    private:
        int i = 0;
    };

    // a simple simulated event engine: registers a callback, and that
    // callback is invoked when an event happens
    class EventEngine
    {
    public:
        EventEngine() = default;
        ~EventEngine() = default;

        int event()
        {
            if (callback)
                return callback();
            return -2;
        }

        void registerEventCallback(std::function<int()> cb)
        {
            callback = cb;
        }

        // loop until an end condition is met, triggering a callback every 10ms
        void startEventLoop(int endWhen)
        {
            int count = 0;
            while (count >= 0 && count < endWhen)
            {
                std::this_thread::sleep_for(10ms);

                if (callback)
                    count = callback();
                else
                    return;
            }
        }

    private:
        std::function<int()> callback;
    };

    {
        TestObject* obj = new TestObject();
        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 0);

        // Create some long-running event engine
        EventEngine engine;

        // Register a callback that uses a weak reference to the object
        engine.registerEventCallback([&obj, alive = obj->getObserver()]() {
            if (alive)
                return obj->incValue();
            return -1;
        });

        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 1);

        // Invoke the event callback at some future point in time, while the object is alive
        int result = engine.event();
        EXPECT_EQ(result, 1);
        result = engine.event();
        EXPECT_EQ(result, 2);

        delete obj;

        // Invoke the event callback after the object has been deleted.
        // Because of the aliveness check, we will never call and object that
        // has been deleted asynchronously.
        result = engine.event();
        EXPECT_EQ(result, -1);
    }

    {
        TestObject* obj = new TestObject();
        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 0);

        // Create some long-running event engine
        EventEngine engine;

        // Register a callback that uses a weak reference to the object
        engine.registerEventCallback([&obj, alive = obj->getObserver()]() {
            if (alive)
                return obj->incValue();
            return -1;
        });

        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 1);

        engine.startEventLoop(5);
        EXPECT_EQ(obj->incValue(), 6);

        delete obj;

        // Invoke the event callback after the object has been deleted.
        // Because of the aliveness check, we will never call and object that
        // has been deleted asynchronously.

        // nothing should happen and should return with no work done;
        // no live object to call
        engine.startEventLoop(10);

        int result = engine.event();
        EXPECT_EQ(result, -1);
    }
}


TEST(LifetimeObserverTest, TestCallbackWithMultipleObservers)
{
    class TestObject: public LifetimeObserver
    {
    public:
        TestObject() = default;
        ~TestObject() = default;
    };
    {
        TestObject* obj = new TestObject();
        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 0);

        LifetimeObserver observer1 = obj->getObserver(); // weak reference
        LifetimeObserver observer2 = obj->getObserver(); // weak reference

        EXPECT_EQ(obj->getCount(TestObject::e_refType::owner), 1);
        EXPECT_EQ(obj->getCount(TestObject::e_refType::observer), 2);

        EXPECT_TRUE(observer1.isAlive());
        EXPECT_TRUE(observer2.isAlive());

        delete obj;

        EXPECT_FALSE(observer1.isAlive());
        EXPECT_FALSE(observer2.isAlive());
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
