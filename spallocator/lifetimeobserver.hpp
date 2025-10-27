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

#ifndef LIFETIME_OBSERVER_HPP_
#define LIFETIME_OBSERVER_HPP_

#include <cstddef>

#include "helper.hpp"


//
// LifetimeObserver is a helper class to track whether an object is alive
// (not yet destroyed). This is useful for asynchronous callbacks or event
// handlers that may be invoked after the object they reference has been
// destroyed.
//
// A common use case goes something like this:
// - An object A derives from LifetimeObserver.
// - Object A registers a callback or event handler with some external
//   system (e.g., an event loop, a timer, a network handler).
// - The callback captures a weak reference to A's LifetimeObserver via
//   getObserver().
// - The callback is used while the object A is alive.
// - When the callback is invoked, it first checks if the object
//   is still alive by calling isAlive() on the captured LifetimeObserver.
// - If isAlive() returns true, the callback proceeds to use object A.
// - At some point, A is destroyed but the callback handler still exists.
// - If isAlive() returns false, the callback safely exits without
//   accessing the destroyed object.
//
// This implementation is abstractly based on the Observer Pattern, with
// a specialization based on the Mediator Pattern to manage both strong
// (owner aka "subject") and weak (observer) references to the object's
// lifetime.
// More explicitly, it is inspired by the implementation of
// std::shared_ptr and std::weak_ptr in the C++ Standard Library.
//
class LifetimeObserver
{
public: // types
    enum class e_refType
    {
        owner, // "subject" in observer pattern
        observer
    };

private: // encapsulated types
    // ControlBlock is the "mediator" in the mediator pattern
    struct ControlBlock
    {
        int64_t addRef(e_refType ref_type);
        int64_t releaseRef(e_refType ref_type);

        int64_t getCount(e_refType ref_type) const;

        ControlBlock() = default;
        ControlBlock(e_refType ref_type);
        ~ControlBlock() = default;
;
    private: // methods
        ControlBlock(const ControlBlock&) = delete;
        ControlBlock& operator=(const ControlBlock&) = delete;
        ControlBlock(ControlBlock&&) = delete;
        ControlBlock& operator=(ControlBlock&&) = delete;

    private: // data members
        int64_t owner_count = 0;
        int64_t observer_count = 0;
    };

    // control blocks are explicitly sharable between owner and observer references
    ControlBlock* control_block = nullptr;

public: // methods
    // Check if the observed object is still alive
    bool isAlive() const;
    operator bool() const { return isAlive(); }

    // Use getObserver() to obtain an observer object distinct from the
    // object for which liveness is being observed.
    // Example: Object A is an object, derived from LifetimeObserver,
    // which has an indeterminate lifetime. getObserver() returns a
    // separate and unique LifetimeObserver object (B) that tracks the
    // liveness of object A, even after its destruction. B has a separate
    // lifetime from A, and can be safely used to check if A is still
    // alive.
    LifetimeObserver getObserver() const;

    // Useful for diagnostics
    int64_t getCount(e_refType ref_type) const;

    // Discard old state and copy in new state from other object
    LifetimeObserver& reset(const LifetimeObserver& other,
                            e_refType ref_type);

    // Copy constructor
    // Note: This constructor is only for copying observer; the inheriting
    // class should not use this constructor directly to create owner
    // copies, but use the ref_type constructor or reset() method instead.
    explicit LifetimeObserver(const LifetimeObserver& other);

    ~LifetimeObserver();

protected: // methods
    // Default constructor - creates an owner reference
    // Used by inheriting class to create initial owner reference.
    // Optionally used to create a standalone object that will later be
    // assigned as an observer.
    LifetimeObserver();

    // Copy constructor and copy operator
    // - Primary use case is when the inheriting object is copied, where
    //   each copy gets a separate ownership reference.
    // - Secondary use case (explicit LifetimeObserver parameter) is to create
    //   an observer object by copy assignment from an existing owner object;
    //   in this case, explicit care must be taken to ensure the copy is a
    //   weak observer reference. For this use case, the requirement is to use
    //   the getObserver() method to obtain the observer.
    LifetimeObserver(const LifetimeObserver& other, e_refType ref_type);
    LifetimeObserver& operator=(const LifetimeObserver& other);

    // Move constructor and move operator
    // Used to transfer ownership of the inheriting object along with
    // ownership state.
    LifetimeObserver(LifetimeObserver&& other) noexcept;
    LifetimeObserver& operator=(LifetimeObserver&& other) noexcept;

private:
    e_refType my_ownership = e_refType::owner;
};


inline LifetimeObserver::ControlBlock::ControlBlock(e_refType ref_type)
{
    addRef(ref_type);
}


inline int64_t LifetimeObserver::ControlBlock::addRef(e_refType ref_type)
{
    if (ref_type == e_refType::owner)
    {
        return ++owner_count;
    }
    else
    {
        return ++observer_count;
    }
}

inline int64_t LifetimeObserver::ControlBlock::releaseRef(e_refType ref_type)
{
    if (ref_type == e_refType::owner)
    {
        return --owner_count;
    }
    else
    {
        return --observer_count;
    }
}

inline int64_t LifetimeObserver::ControlBlock::getCount(e_refType ref_type) const
{
    if (ref_type == e_refType::owner)
    {
        return owner_count;
    }
    else
    {
        return observer_count;
    }
}


inline bool LifetimeObserver::isAlive() const
{
    return control_block->getCount(e_refType::owner) > 0;
}


inline LifetimeObserver LifetimeObserver::getObserver() const
{
    // make a weak reference copy and return it
    return LifetimeObserver(*this, e_refType::observer);
}


inline int64_t LifetimeObserver::getCount(e_refType ref_type) const
{
    return control_block->getCount(ref_type);
}


// default constructor - creates an owner reference
inline LifetimeObserver::LifetimeObserver():
    control_block(new ControlBlock(e_refType::owner)),
    my_ownership(e_refType::owner)
{
}

// copy constructor
inline LifetimeObserver::LifetimeObserver(const LifetimeObserver& other):
    control_block(other.control_block),
    my_ownership(e_refType::observer)
{
    // We are an observer copy
    control_block->addRef(e_refType::observer);
}

inline LifetimeObserver::LifetimeObserver(const LifetimeObserver& other, e_refType ref_type):
    my_ownership(ref_type)
{
    if (my_ownership == e_refType::owner)
    {
        // We own a separate copy. Make a new control block separate from
        // the original object.
        control_block = new ControlBlock(my_ownership);
    }
    else
    {
        // We are an observer copy
        control_block = other.control_block;
        control_block->addRef(e_refType::observer);
    }
}

// copy assignment operator
inline LifetimeObserver& LifetimeObserver::operator=(const LifetimeObserver& other)
{
    // Assumed use-case: creating a new owned copy of an inherited object.
    // If creating an obeserver copy, use getObserver() method instead.
    return reset(other, e_refType::owner);
}

inline LifetimeObserver& LifetimeObserver::reset(
    const LifetimeObserver& other,
    e_refType ref_type)
{
    if (this != &other)
    {
        // are we changing which object we are observing?
        if (control_block != other.control_block)
        {
            control_block->releaseRef(my_ownership);
            spallocator::runtime_assert(getCount(my_ownership) >= 0,
                "Reference count went negative in LifetimeObserver copy assignment");
            if (getCount(e_refType::owner) == 0 &&
                getCount(e_refType::observer) == 0)
            {
                delete control_block;
            }

            my_ownership = ref_type;
            if (my_ownership == e_refType::owner)
            {
                // We own a separate copy. Make a new control block separate from
                // the original object.
                control_block = new ControlBlock(my_ownership);
            }
            else
            {
                // We are an observer copy
                control_block = other.control_block;
                control_block->addRef(e_refType::observer);
            }
        }
    }
    return *this;
}

// move constructor
inline LifetimeObserver::LifetimeObserver(LifetimeObserver&& other) noexcept:
    control_block(other.control_block),
    my_ownership(other.my_ownership)
{
    other.my_ownership = e_refType::owner;
    other.control_block = new ControlBlock(other.my_ownership);
}

// move assignment operator
inline LifetimeObserver& LifetimeObserver::operator=(LifetimeObserver&& other) noexcept
{
    if (this != &other)
    {
        my_ownership = other.my_ownership;
        delete control_block;
        control_block = other.control_block;
        other.my_ownership = e_refType::owner;
        other.control_block = new ControlBlock(other.my_ownership);
    }
    return *this;
}


inline LifetimeObserver::~LifetimeObserver()
{
    if (control_block)
    {
        control_block->releaseRef(my_ownership);

        spallocator::runtime_assert(control_block->getCount(my_ownership) >= 0,
            "Reference count went negative in LifetimeObserver destructor");

        if (getCount(e_refType::owner) == 0 &&
            getCount(e_refType::observer) == 0)
        {
            delete control_block;
        }
    }
}


#endif // LIFETIME_OBSERVER_HPP_
