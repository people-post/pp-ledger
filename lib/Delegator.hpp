#pragma once

#include <memory>

namespace pp {

/**
 * @class Delegator
 * @brief Provides delegation pattern support with type-safe casting
 * 
 * Holds a non-owning raw pointer to a delegate object and provides type-safe retrieval.
 * The delegate lifetime must be managed by the owner. Typical usage: delegator.setDelegate(this).
 * 
 * @warning The delegate must remain valid for the lifetime of this Delegator.
 */
class Delegator {
public:
    /**
     * @struct Delegate
     * @brief Base class for all delegates
     */
    struct Delegate {
        virtual ~Delegate() = default;
    };

    /**
     * @brief Construct a Delegator with no delegate
     */
    Delegator() = default;

    /**
     * @brief Destructor
     */
    ~Delegator() = default;

    /**
     * @brief Set the delegate from a raw pointer
     * 
     * Stores a non-owning reference to the delegate. The caller is responsible
     * for ensuring the delegate remains valid. Typical usage: setDelegate(this).
     * 
     * @param delegate Raw pointer to delegate (ownership NOT transferred)
     */
    void setDelegate(Delegate* delegate) {
        pDelegate_ = delegate;
    }

protected:
    /**
     * @brief Get the delegate as a derived type
     * 
     * Performs a dynamic_cast to the specified type and returns a raw pointer.
     * Returns nullptr if the cast fails (delegate is null or not of type T).
     * 
     * @tparam T The derived delegate type
     * @return T* Casted delegate pointer, or nullptr if cast fails
     */
    template<typename T>
    T* getDelegate() const {
        if (!pDelegate_) {
            return nullptr;
        }
        return dynamic_cast<T*>(pDelegate_);
    }

    /**
     * @brief Check if a delegate is set
     * 
     * @return bool True if a delegate is set, false otherwise
     */
    bool hasDelegate() const {
        return pDelegate_ != nullptr;
    }

private:
    Delegate* pDelegate_{nullptr};
};

} // namespace pp
