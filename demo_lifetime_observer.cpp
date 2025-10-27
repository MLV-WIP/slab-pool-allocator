/*
 * Demo program showing LifetimeObserver usage
 */

#include <functional>
#include <thread>
#include "spallocator/helper.hpp"
#include "spallocator/lifetimeobserver.hpp"

using namespace spallocator;


// Example 1: Simple object with lifetime tracking
class DataProcessor : public LifetimeObserver
{
public:
    DataProcessor(int newid) : id(newid)
    {
        println("DataProcessor({}) created", id);
    }

    ~DataProcessor()
    {
        println("DataProcessor({}) destroyed", id);
    }

    int process()
    {
        return id * 100;
    }

private:
    int id;
};


// Example 2: Event engine that holds weak references to objects
class EventEngine
{
public:
    EventEngine() = default;

    void registerCallback(std::function<int()> cb)
    {
        callback = cb;
    }

    int fireEvent()
    {
        if (callback)
        {
            return callback();
        }
        return -1;
    }

private:
    std::function<int()> callback;
};


int main()
{
    println("=== Demonstration of LifetimeObserver ===\n");

    // Example 1: Basic ownership tracking
    println("1. Basic object lifetime tracking:");
    {
        DataProcessor proc(42);
        println("   proc.isAlive(): {}", proc.isAlive());
        println("   Owner count: {}", proc.getCount(LifetimeObserver::e_refType::owner));
        println("   Observer count: {}", proc.getCount(LifetimeObserver::e_refType::observer));

        {
            LifetimeObserver observer = proc.getObserver();
            println("   Created weak observer");
            println("   proc.isAlive(): {}", proc.isAlive());
            println("   observer.isAlive(): {}", observer.isAlive());
            println("   Observer count now: {}",
                    proc.getCount(LifetimeObserver::e_refType::observer));
        }
        println("   Observer out of scope, count: {}",
                proc.getCount(LifetimeObserver::e_refType::observer));
    }
    println("   Object destroyed\n");

    // Example 2: Weak reference survives object deletion
    println("2. Weak reference after object deletion:");
    {
        DataProcessor* proc = new DataProcessor(99);
        LifetimeObserver observer = proc->getObserver();

        println("   Before deletion - observer.isAlive(): {}", observer.isAlive());

        delete proc;

        println("   After deletion - observer.isAlive(): {}", observer.isAlive());
        println("   Safe to check without dereferencing pointer!\n");
    }

    // Example 3: Asynchronous callback scenario
    println("3. Asynchronous callback with event engine:");
    {
        DataProcessor* proc = new DataProcessor(777);
        EventEngine engine;

        // Register callback with weak reference to the processor
        engine.registerCallback([&proc, alive = proc->getObserver()]() {
            if (alive)
            {
                println("   Callback: Processing data, result = {}", proc->process());
                return proc->process();
            }
            else
            {
                println("   Callback: Object already destroyed, skipping");
                return -1;
            }
        });

        println("   Firing event while object is alive:");
        engine.fireEvent();

        println("   Deleting object...");
        delete proc;

        println("   Firing event after object deleted:");
        engine.fireEvent();
    }
    println("");

    // Example 4: Multiple observers tracking same object
    println("4. Multiple observers tracking same object:");
    {
        DataProcessor* proc = new DataProcessor(123);

        LifetimeObserver observer1 = proc->getObserver();
        LifetimeObserver observer2 = proc->getObserver();
        LifetimeObserver observer3 = proc->getObserver();

        println("   Created 3 observers");
        println("   Observer count: {}",
                proc->getCount(LifetimeObserver::e_refType::observer));

        println("   All observers alive: {} {} {}",
                observer1.isAlive(), observer2.isAlive(), observer3.isAlive());

        delete proc;

        println("   After deletion, all observers report: {} {} {}",
                observer1.isAlive(), observer2.isAlive(), observer3.isAlive());
    }
    println("");

    // Example 5: RAII with stack-based objects
    println("5. Stack-based object with observer:");
    {
        DataProcessor proc(555);
        LifetimeObserver observer = proc.getObserver();

        println("   Stack object alive: {}", observer.isAlive());
        println("   operator bool() also works: {}", (bool)observer);
    } // proc goes out of scope here automatically
    println("   Stack object destroyed by RAII\n");

    println("=== Demo completed ===");
    println("LifetimeObserver safely tracks object lifetimes!");
    println("Ideal for:");
    println("  - Event callbacks that may fire after object deletion");
    println("  - Asynchronous operations with uncertain lifetimes");
    println("  - Breaking reference cycles without shared_ptr overhead");

    return 0;
}
