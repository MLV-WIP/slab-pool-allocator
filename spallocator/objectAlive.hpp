#ifndef OBJECT_ALIVE_HPP_
#define OBJECT_ALIVE_HPP_


namespace spallocator
{

    // Helper class to track if an object is alive (not yet destroyed)
    // Abstractly based on the Observer Pattern and Mediator Pattern
    // Explicitly inspired by std::shared_ptr and std::weak_ptr implementation
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
            ControlBlock(e_refType ref_type) { addRef(ref_type); }
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

        ControlBlock* control_block = nullptr;

    public: // methods
        // Check if the observed object is still alive
        bool isAlive() const;
        operator bool() const { return isAlive(); }

        // Use getObserver to obtain an observer object distinct from the
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
        // note about only use for copying observer and inherited
        // class explicitly using reset with ownership or some such
        LifetimeObserver(const LifetimeObserver& other);

        ~LifetimeObserver();

    protected: // methods
        // Default constructor - creates an owner reference
        // Used by inheriting class to create initial owner reference.
        // Optionally used to create a standalone object that will later be
        // assigned as an observer.
        LifetimeObserver();

        // Copy constructor and copy operator
        // Primary use case is when the inheriting object is copied, where
        // each copy get a separate ownership reference.
        // Secondary use case is to create an observer object by copy
        // assignment from an existing owner object; in this case, explicit
        // care must be taken to ensure the copy is a weak observer reference.
        // For this use case, the requirement is to use the getObserver()
        // method to obtain the observer.
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


    inline LifetimeObserver LifetimeObserver::getObserver() const
    {
        // make a weak reference copy and return it
        return LifetimeObserver(*this, e_refType::observer);
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
                runtime_assert(getCount(my_ownership) >= 0,
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

            runtime_assert(control_block->getCount(my_ownership) >= 0,
                "Reference count went negative in LifetimeObserver destructor");

            if (getCount(e_refType::owner) == 0 &&
                getCount(e_refType::observer) == 0)
            {
                delete control_block;
            }
        }
    }


}; // namespace spallocator


#endif // OBJECT_ALIVE_HPP_
