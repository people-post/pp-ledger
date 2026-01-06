#ifndef PP_LEDGER_RESULT_OR_ERROR_H
#define PP_LEDGER_RESULT_OR_ERROR_H

#include <string>
#include <utility>
#include <stdexcept>
#include <type_traits>

namespace pp {

template<typename T, typename E = std::string>
class ResultOrError {
public:
    // Constructors for success case
    ResultOrError(const T& value) : hasValue_(true) {
        new (&storage_) T(value);
    }
    
    ResultOrError(T&& value) : hasValue_(true) {
        new (&storage_) T(std::move(value));
    }
    
    // Constructors for error case
    static ResultOrError error(const E& err) {
        ResultOrError result;
        result.hasValue_ = false;
        new (&result.storage_) E(err);
        return result;
    }
    
    static ResultOrError error(E&& err) {
        ResultOrError result;
        result.hasValue_ = false;
        new (&result.storage_) E(std::move(err));
        return result;
    }
    
    // Copy constructor
    ResultOrError(const ResultOrError& other) : hasValue_(other.hasValue_) {
        if (hasValue_) {
            new (&storage_) T(*reinterpret_cast<const T*>(&other.storage_));
        } else {
            new (&storage_) E(*reinterpret_cast<const E*>(&other.storage_));
        }
    }
    
    // Move constructor
    ResultOrError(ResultOrError&& other) noexcept : hasValue_(other.hasValue_) {
        if (hasValue_) {
            new (&storage_) T(std::move(*reinterpret_cast<T*>(&other.storage_)));
        } else {
            new (&storage_) E(std::move(*reinterpret_cast<E*>(&other.storage_)));
        }
    }
    
    // Destructor
    ~ResultOrError() {
        destroy();
    }
    
    // Copy assignment
    ResultOrError& operator=(const ResultOrError& other) {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (hasValue_) {
                new (&storage_) T(*reinterpret_cast<const T*>(&other.storage_));
            } else {
                new (&storage_) E(*reinterpret_cast<const E*>(&other.storage_));
            }
        }
        return *this;
    }
    
    // Move assignment
    ResultOrError& operator=(ResultOrError&& other) noexcept {
        if (this != &other) {
            destroy();
            hasValue_ = other.hasValue_;
            if (hasValue_) {
                new (&storage_) T(std::move(*reinterpret_cast<T*>(&other.storage_)));
            } else {
                new (&storage_) E(std::move(*reinterpret_cast<E*>(&other.storage_)));
            }
        }
        return *this;
    }
    
    // Check if contains value
    bool isOk() const { return hasValue_; }
    bool isError() const { return !hasValue_; }
    
    // Explicit conversion to bool (true if has value)
    explicit operator bool() const { return hasValue_; }
    
    // Access value (throws if error)
    const T& value() const {
        if (!hasValue_) {
            throw std::runtime_error("Attempting to access value of error result");
        }
        return *reinterpret_cast<const T*>(&storage_);
    }
    
    T& value() {
        if (!hasValue_) {
            throw std::runtime_error("Attempting to access value of error result");
        }
        return *reinterpret_cast<T*>(&storage_);
    }
    
    // Access value with default if error
    T valueOr(const T& defaultValue) const {
        return hasValue_ ? value() : defaultValue;
    }
    
    // Access error (throws if value)
    const E& error() const {
        if (hasValue_) {
            throw std::runtime_error("Attempting to access error of success result");
        }
        return *reinterpret_cast<const E*>(&storage_);
    }
    
    E& error() {
        if (hasValue_) {
            throw std::runtime_error("Attempting to access error of success result");
        }
        return *reinterpret_cast<E*>(&storage_);
    }
    
    // Dereference operators for convenience
    const T& operator*() const { return value(); }
    T& operator*() { return value(); }
    
    const T* operator->() const { return &value(); }
    T* operator->() { return &value(); }

private:
    // Private default constructor for error case
    ResultOrError() : hasValue_(false) {}
    
    void destroy() {
        if (hasValue_) {
            reinterpret_cast<T*>(&storage_)->~T();
        } else {
            reinterpret_cast<E*>(&storage_)->~E();
        }
    }
    
    bool hasValue_;
    typename std::aligned_union<0, T, E>::type storage_;
};

// Specialization for void return type
template<typename E>
class ResultOrError<void, E> {
public:
    // Constructor for success case
    ResultOrError() : hasValue_(true) {}
    
    // Constructor for error case
    static ResultOrError error(const E& err) {
        ResultOrError result;
        result.hasValue_ = false;
        new (&result.storage_) E(err);
        return result;
    }
    
    static ResultOrError error(E&& err) {
        ResultOrError result;
        result.hasValue_ = false;
        new (&result.storage_) E(std::move(err));
        return result;
    }
    
    // Copy constructor
    ResultOrError(const ResultOrError& other) : hasValue_(other.hasValue_) {
        if (!hasValue_) {
            new (&storage_) E(*reinterpret_cast<const E*>(&other.storage_));
        }
    }
    
    // Move constructor
    ResultOrError(ResultOrError&& other) noexcept : hasValue_(other.hasValue_) {
        if (!hasValue_) {
            new (&storage_) E(std::move(*reinterpret_cast<E*>(&other.storage_)));
        }
    }
    
    // Destructor
    ~ResultOrError() {
        if (!hasValue_) {
            reinterpret_cast<E*>(&storage_)->~E();
        }
    }
    
    // Check if success
    bool isOk() const { return hasValue_; }
    bool isError() const { return !hasValue_; }
    explicit operator bool() const { return hasValue_; }
    
    // Access error
    const E& error() const {
        if (hasValue_) {
            throw std::runtime_error("Attempting to access error of success result");
        }
        return *reinterpret_cast<const E*>(&storage_);
    }
    
    E& error() {
        if (hasValue_) {
            throw std::runtime_error("Attempting to access error of success result");
        }
        return *reinterpret_cast<E*>(&storage_);
    }

private:
    bool hasValue_;
    typename std::aligned_union<0, E>::type storage_;
};

} // namespace pp

#endif // PP_LEDGER_RESULT_OR_ERROR_H
