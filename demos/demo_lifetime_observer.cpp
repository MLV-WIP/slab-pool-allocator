/*
 * Demo program showing LifetimeObserver usage
 */

#include <functional>
#include <mutex>
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
        println("   Safe to check liveness without dereferencing the raw pointer!\n");
    }

    // Example 3: Single-threaded / single-strand async callbacks
    //
    // Legitimate and common: one event thread (e.g. Asio io_context run on a
    // single thread, or a UI/event loop) posts both destruction and callbacks.
    // Sequencing is the protocol — destroy and the callback never overlap, so
    // isAlive()-then-use needs no mutex. See example 4 when that is not true.
    println("3. Single-threaded callback (e.g. Asio one event thread):");
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

    // Example 4: Cross-thread use — mutex covers check + use + destroy
    //
    // When another thread can destroy the object while a callback runs,
    // isAlive() alone is TOCTOU. Hold an external mutex across both the
    // check/use path and delete (member mutex unlocked before ~LifetimeObserver
    // is too late). Prefer weak_ptr::lock() if you need ownership upgrade.
    println("4. Cross-thread callback (mutex avoids TOCTOU):");
    {
        std::mutex access_mu;
        DataProcessor* proc = new DataProcessor(888);
        EventEngine engine;

        engine.registerCallback([&access_mu, &proc, alive = proc->getObserver()]() {
            std::lock_guard lock(access_mu);
            if (!alive)
            {
                println("   Callback: Object already destroyed, skipping");
                return -1;
            }
            println("   Callback: Processing data, result = {}", proc->process());
            return proc->process();
        });

        println("   Firing event while object is alive:");
        engine.fireEvent();

        println("   Deleting object under the access mutex...");
        {
            std::lock_guard lock(access_mu);
            delete proc;
            proc = nullptr;
        }

        println("   Firing event after object deleted:");
        engine.fireEvent();
    }
    println("");

    // Example 5: Multiple observers tracking same object
    println("5. Multiple observers tracking same object:");
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

    // Example 6: RAII with stack-based objects
    println("6. Stack-based object with observer:");
    {
        DataProcessor proc(555);
        LifetimeObserver observer = proc.getObserver();

        println("   Stack object alive: {}", observer.isAlive());
        println("   operator bool() also works: {}", (bool)observer);
    } // proc goes out of scope here automatically
    println("   Stack object destroyed by RAII\n");

    println("=== Demo completed ===");
    println("LifetimeObserver tracks liveness with atomic control-block counts.");
    println("Ideal for:");
    println("  - Single-strand event callbacks (Asio one thread, UI loops, …)");
    println("  - Cross-thread use only with a mutex/strand that also covers destroy");
    println("  - Breaking reference cycles without shared_ptr overhead");
    println("Not a substitute for weak_ptr::lock() when you need ownership upgrade.");

    return 0;
}
