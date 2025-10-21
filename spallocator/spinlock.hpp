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

#ifndef SPINLOCK_HPP_
#define SPINLOCK_HPP_

#include <atomic>
#include <thread>
#include <random>
#include <chrono>


// ThreadSanitizer annotations for custom synchronization primitives
// These tell TSan about happens-before relationships in our SpinLock
#ifdef __has_feature
  #if __has_feature(thread_sanitizer)
    #define TSAN_ENABLED 1
  #endif
#endif

#ifdef TSAN_ENABLED
extern "C" {
    void __tsan_acquire(void *addr);
    void __tsan_release(void *addr);
}
#define TSAN_ANNOTATE_HAPPENS_BEFORE(addr) __tsan_release(addr)
#define TSAN_ANNOTATE_HAPPENS_AFTER(addr)  __tsan_acquire(addr)
#else
#define TSAN_ANNOTATE_HAPPENS_BEFORE(addr)
#define TSAN_ANNOTATE_HAPPENS_AFTER(addr)
#endif


class SpinLock
{
public:
    SpinLock() = default;
    ~SpinLock() = default;

    void lock()
    {
        // Single static instance per thread; efficient and thread safe
        // without requiring additional costly synchronization
        thread_local static std::minstd_rand gen{std::random_device{}()};
        thread_local static std::uniform_int_distribution<int> dist{1, 100};

        // Incrementing time to wait if we get contention. This is an
        // escalating backoff strategy to avoid the thundering herd problem
        // when the resource becomes available. This doesn't have to be highly
        // random, and it doesn't even have to be highly accurate on the wait;
        // the more important attribute is that it differentiates the wait
        // times between different threads to encourage ordered acquisition
        // attempts.
        std::chrono::nanoseconds wait_time{dist(gen)};

        int backoff_count = 0;

        // Test-and-Test-and-set (TTAS) spin lock with escalating backoff
        while (true)
        {
            // First, spin while the lock appears to be held
            for (int i = 0; i < 100; ++i)
            {
                if (!lock_flag.test(std::memory_order_relaxed))
                {
                    break; // Lock appears free, try to acquire
                }
                std::this_thread::yield();
            }

            // Then, attempt to acquire the lock
            if (!lock_flag.test_and_set(std::memory_order_acquire))
            {
                TSAN_ANNOTATE_HAPPENS_AFTER(this);
                return; // Lock acquired
            }

            // Failed to acquire - use escalating backoff
            if (backoff_count < 10)
            {
                std::this_thread::sleep_for(wait_time);
                wait_time += wait_time;
                ++backoff_count;
            }
            else
            {
                // After 10 attempts, use efficient blocking
                lock_flag.wait(true, std::memory_order_relaxed);
                // Loop back to try acquiring again
            }
        }
    }

    bool try_lock()
    {
        if (lock_flag.test(std::memory_order_relaxed))
        {
            return false; // Lock is already held elsewhere
        }

        // Attempt to acquire the lock without blocking
        if (!lock_flag.test_and_set(std::memory_order_acquire))
        {
            TSAN_ANNOTATE_HAPPENS_AFTER(this);
            return true; // Lock acquired
        }
        // Lock not acquired; return immediately
        return false;
    }

    void unlock()
    {
        TSAN_ANNOTATE_HAPPENS_BEFORE(this);
        lock_flag.clear(std::memory_order_release);
        lock_flag.notify_one();
    }

private: // methods
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

private: // data members
    std::atomic_flag lock_flag = ATOMIC_FLAG_INIT;
};


#endif // SPINLOCK_HPP_
