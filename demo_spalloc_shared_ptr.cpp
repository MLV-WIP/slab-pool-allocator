/*
 * Demo program showing make_pool_shared usage
 */

#include <vector>
#include "spallocator/helper.hpp"
#include "spallocator/spinlock.hpp"
#include "spallocator/slab.hpp"
#include "spallocator/pool.hpp"
#include "spallocator/spallocator.hpp"

using namespace spallocator;

struct MyClass {
    int value;

    MyClass(int v) : value(v) {
        println("MyClass({}) constructed", value);
    }

    ~MyClass() {
        println("MyClass({}) destroyed", value);
    }
};

int main() {
    Pool pool;

    println("=== Demonstration of make_pool_shared ===\n");

    // 1. Single object with shared ownership
    println("1. Creating shared_ptr to single object:");
    {
        auto obj1 = make_pool_shared<MyClass>(pool, 42);
        println("   obj1 value: {}, use_count: {}", obj1->value, obj1.use_count());

        auto obj2 = obj1;  // Share ownership
        println("   After sharing, use_count: {}", obj1.use_count());

        println("   Exiting scope, both will release reference...");
    }
    println("   Object destroyed when last reference released\n");

    // 2. Array with default initialization
    println("2. Creating shared_ptr to array (default init):");
    {
        auto arr = make_pool_shared<int[]>(pool, 5);
        println("   Array created with 5 elements");

        for (int i = 0; i < 5; ++i) {
            arr[i] = i * 10;
        }

        print("   Array values: ");
        for (int i = 0; i < 5; ++i) {
            print("{} ", arr[i]);
        }
        println("");
    }
    println("   Array destroyed\n");

    // 3. Array with value initialization
    println("3. Creating shared_ptr to array (value init):");
    {
        auto arr = make_pool_shared<int[]>(pool, 5, 99);
        print("   Array values (all initialized to 99): ");
        for (int i = 0; i < 5; ++i) {
            print("{} ", arr[i]);
        }
        println("");
    }
    println("   Array destroyed\n");

    // 4. Shared pointers in containers
    println("4. Using shared_ptr in containers:");
    {
        std::vector<std::shared_ptr<MyClass>> vec;

        for (int i = 0; i < 3; ++i) {
            vec.push_back(make_pool_shared<MyClass>(pool, i));
        }

        println("   Container has {} objects", vec.size());
        println("   Clearing container...");
        vec.clear();
    }
    println("   All objects destroyed\n");

    // 5. Comparing with make_pool_unique
    println("5. Comparison with unique_ptr:");
    {
        println("   unique_ptr (exclusive ownership):");
        auto unique_obj = make_pool_unique<MyClass>(pool, 100);
        // auto copy = unique_obj;  // ERROR: cannot copy unique_ptr
        auto moved = std::move(unique_obj);  // OK: can move
        println("   After move, moved value: {}", moved->value);

        println("\n   shared_ptr (shared ownership):");
        auto shared_obj = make_pool_shared<MyClass>(pool, 200);
        auto copy = shared_obj;  // OK: can copy, shares ownership
        println("   After copy, use_count: {}", shared_obj.use_count());
        println("   Both pointers point to value: {}", shared_obj->value);
    }
    println("   Objects destroyed\n");

    println("=== Demo completed ===");
    println("All memory allocated from pool and returned to pool!");

    return 0;
}
