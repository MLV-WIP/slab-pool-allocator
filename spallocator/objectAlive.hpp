#ifndef OBJECT_ALIVE_HPP_
#define OBJECT_ALIVE_HPP_


namespace spallocator
{

    // Helper class to track if an object is alive (not yet destroyed)
    template<typename T>
    class LifetimeObserver 
    {
    public: // types
        enum class e_refType
        {
            owner,
            observer
        };

    private: // encapsulated types
        struct ControlBlock
        {
            int64_t addRef(e_refType ref_type);
            int64_t releaseRef(e_refType ref_type);

            int64_t getCount(e_refType ref_type) const;

            ControlBlock() = default;
            ControlBlock(e_refType ref_type) { addRef(ref_type); }
            ~ControlBlock() = default;
;
        private: // data members
            int64_t owner_count = 0;
            int64_t observer_count = 0;
        };

        ControlBlock* control_block = nullptr;

    public: // methods
        // Check if the object is still alive
        bool isAlive() const;
        operator bool() const { return isAlive(); }

        int64_t getCount(e_refType ref_type) const;

        LifetimeObserver(e_refType ref_type = e_refType::owner);

        LifetimeObserver(const LifetimeObserver& other);
        LifetimeObserver& operator=(const LifetimeObserver& other);

        LifetimeObserver(LifetimeObserver&& other) noexcept;
        LifetimeObserver& operator=(LifetimeObserver&& other) noexcept;

        ~LifetimeObserver();

    private:
        e_refType my_ref = e_refType::owner;
    };


    template<typename T>
    inline int64_t LifetimeObserver<T>::ControlBlock::addRef(e_refType ref_type)
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

    template<typename T>
    inline int64_t LifetimeObserver<T>::ControlBlock::releaseRef(e_refType ref_type)
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

    template<typename T>
    inline int64_t LifetimeObserver<T>::ControlBlock::getCount(e_refType ref_type) const
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


    template<typename T>
    inline bool LifetimeObserver<T>::isAlive() const
    {
        return control_block->getCount(e_refType::owner) > 0;
    }


    template<typename T>
    inline int64_t LifetimeObserver<T>::getCount(e_refType ref_type) const
    {
        return control_block->getCount(ref_type);
    }


    template<typename T>
    inline LifetimeObserver<T>::LifetimeObserver(e_refType ref_type /* = e_refType::owner */):
        control_block(new ControlBlock(ref_type)),
        my_ref(ref_type)
    {
    }

    template<typename T>
    inline LifetimeObserver<T>::LifetimeObserver(const LifetimeObserver& other):
        control_block(other.control_block),
        my_ref(e_refType::observer)
    {
        control_block->addRef(my_ref);
    }

    template<typename T>
    inline LifetimeObserver<T>& LifetimeObserver<T>::operator=(const LifetimeObserver& other)
    {
        if (this != &other)
        {
            delete control_block;
            control_block = other.control_block;
            my_ref = e_refType::observer;
            control_block->addRef(my_ref);
        }
        return *this;
    }

    template<typename T>
    inline LifetimeObserver<T>::LifetimeObserver(LifetimeObserver&& other) noexcept:
        control_block(other.control_block),
        my_ref(other.my_ref)
    {
        other.my_ref = e_refType::owner;
        other.control_block = new ControlBlock(other.my_ref);
    }

    template<typename T>
    inline LifetimeObserver<T>& LifetimeObserver<T>::operator=(LifetimeObserver&& other) noexcept
    {
        if (this != &other)
        {
            my_ref = other.my_ref;
            delete control_block;
            control_block = other.control_block;
            other.my_ref = e_refType::owner;
            other.control_block = new ControlBlock(other.my_ref);
        }
        return *this;
    }


    template<typename T>
    inline LifetimeObserver<T>::~LifetimeObserver()
    {
        if (control_block)
        {
            control_block->releaseRef(my_ref);

            runtime_assert(control_block->getCount(my_ref) >= 0,
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
