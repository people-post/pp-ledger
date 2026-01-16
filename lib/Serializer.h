#ifndef PP_LEDGER_SERIALIZER_H
#define PP_LEDGER_SERIALIZER_H

#include <string>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <cstdint>
#include <type_traits>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

namespace pp {

// Forward declarations
class OutputArchive;
class InputArchive;

namespace detail {

// Helper to check if type is a pointer (should not be serialized)
template<typename T>
static constexpr bool is_pointer_v = std::is_pointer_v<T>;

// Helper to check if type is long double (not supported)
template<typename T>
static constexpr bool is_long_double_v = std::is_same_v<T, long double>;

// Cache endianness detection result - initialized once on first use
inline bool isLittleEndian() {
    static const bool cached = []() {
        const uint16_t test = 0x0102;
        return reinterpret_cast<const uint8_t*>(&test)[0] == 0x02;
    }();
    return cached;
}

// Swap bytes for a value
template<typename T>
T swapBytes(T value) {
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&value);
    constexpr size_t size = sizeof(T);
    for (size_t i = 0; i < size / 2; ++i) {
        std::swap(bytes[i], bytes[size - 1 - i]);
    }
    return value;
}

// Endian conversion helpers - convert to/from big endian (network byte order)
// These ensure machine-independent serialization
inline uint16_t toBigEndian(uint16_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

inline uint32_t toBigEndian(uint32_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

inline uint64_t toBigEndian(uint64_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

inline uint16_t fromBigEndian(uint16_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

inline uint32_t fromBigEndian(uint32_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

inline uint64_t fromBigEndian(uint64_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

// Floating point conversion helpers - convert to/from IEEE 754 big endian format
inline void floatToBigEndian(float value, uint8_t* bytes) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(float));
    bits = toBigEndian(bits);
    std::memcpy(bytes, &bits, sizeof(float));
}

inline float floatFromBigEndian(const uint8_t* bytes) {
    uint32_t bits;
    std::memcpy(&bits, bytes, sizeof(float));
    bits = fromBigEndian(bits);
    float value;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
}

inline void doubleToBigEndian(double value, uint8_t* bytes) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));
    bits = toBigEndian(bits);
    std::memcpy(bytes, &bits, sizeof(double));
}

inline double doubleFromBigEndian(const uint8_t* bytes) {
    uint64_t bits;
    std::memcpy(&bits, bytes, sizeof(double));
    bits = fromBigEndian(bits);
    double value;
    std::memcpy(&value, &bits, sizeof(double));
    return value;
}

} // namespace detail

/**
 * OutputArchive for serialization (writing)
 * Supports the & operator pattern used by custom structs
 * This is the primary class for serialization - Serializer delegates to this
 */
class OutputArchive {
public:
    explicit OutputArchive(std::ostream& os) : os_(os) {}

    // Serialize fundamental types
    void write(bool value) {
        uint8_t byte = value ? 1 : 0;
        os_.write(reinterpret_cast<const char*>(&byte), sizeof(byte));
    }

    void write(char value) {
        os_.write(&value, sizeof(value));
    }

    void write(int8_t value) {
        os_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void write(uint8_t value) {
        os_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void write(int16_t value) {
        uint16_t uvalue = static_cast<uint16_t>(value);
        uvalue = detail::toBigEndian(uvalue);
        os_.write(reinterpret_cast<const char*>(&uvalue), sizeof(uvalue));
    }

    void write(uint16_t value) {
        value = detail::toBigEndian(value);
        os_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void write(int32_t value) {
        uint32_t uvalue = static_cast<uint32_t>(value);
        uvalue = detail::toBigEndian(uvalue);
        os_.write(reinterpret_cast<const char*>(&uvalue), sizeof(uvalue));
    }

    void write(uint32_t value) {
        value = detail::toBigEndian(value);
        os_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void write(int64_t value) {
        uint64_t uvalue = static_cast<uint64_t>(value);
        uvalue = detail::toBigEndian(uvalue);
        os_.write(reinterpret_cast<const char*>(&uvalue), sizeof(uvalue));
    }

    void write(uint64_t value) {
        value = detail::toBigEndian(value);
        os_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void write(float value) {
        uint8_t bytes[sizeof(float)];
        detail::floatToBigEndian(value, bytes);
        os_.write(reinterpret_cast<const char*>(bytes), sizeof(float));
    }

    void write(double value) {
        uint8_t bytes[sizeof(double)];
        detail::doubleToBigEndian(value, bytes);
        os_.write(reinterpret_cast<const char*>(bytes), sizeof(double));
    }

    void write(const std::string& value) {
        uint64_t size = value.size();
        write(size);
        if (size > 0) {
            os_.write(value.data(), size);
        }
    }

    void write(const char* value) {
        if (value == nullptr) {
            uint64_t size = 0;
            write(size);
            return;
        }
        std::string str(value);
        write(str);
    }

    // Write containers
    template<typename T>
    void write(const std::vector<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size = value.size();
        write(size);
        for (const auto& item : value) {
            (*this) & item;
        }
    }

    template<typename T, size_t N>
    void write(const std::array<T, N>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        for (const auto& item : value) {
            (*this) & item;
        }
    }

    template<typename K, typename V>
    void write(const std::map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size = value.size();
        write(size);
        for (const auto& pair : value) {
            (*this) & pair.first;
            (*this) & pair.second;
        }
    }

    template<typename K, typename V>
    void write(const std::unordered_map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size = value.size();
        write(size);
        for (const auto& pair : value) {
            (*this) & pair.first;
            (*this) & pair.second;
        }
    }

    template<typename T>
    void write(const std::set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size = value.size();
        write(size);
        for (const auto& item : value) {
            (*this) & item;
        }
    }

    template<typename T>
    void write(const std::unordered_set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size = value.size();
        write(size);
        for (const auto& item : value) {
            (*this) & item;
        }
    }

    // Operator & for fundamental types
    OutputArchive& operator&(bool value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(char value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(int8_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(uint8_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(int16_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(uint16_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(int32_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(uint32_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(int64_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(uint64_t value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(float value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(double value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(const std::string& value) {
        write(value);
        return *this;
    }

    OutputArchive& operator&(const char* value) {
        write(value);
        return *this;
    }

    // Operator & for containers
    template<typename T>
    OutputArchive& operator&(const std::vector<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        write(value);
        return *this;
    }

    template<typename T, size_t N>
    OutputArchive& operator&(const std::array<T, N>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        write(value);
        return *this;
    }

    template<typename K, typename V>
    OutputArchive& operator&(const std::map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        write(value);
        return *this;
    }

    template<typename K, typename V>
    OutputArchive& operator&(const std::unordered_map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        write(value);
        return *this;
    }

    template<typename T>
    OutputArchive& operator&(const std::set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        write(value);
        return *this;
    }

    template<typename T>
    OutputArchive& operator&(const std::unordered_set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        write(value);
        return *this;
    }

    // Operator & for custom types (const reference)
    // Uses const_cast because serialize() is non-const but we're reading (not modifying)
    template<typename T>
    auto operator&(const T& value) -> decltype(std::declval<T&>().template serialize<OutputArchive>(std::declval<OutputArchive&>()), *this) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        const_cast<T&>(value).template serialize<OutputArchive>(*this);
        return *this;
    }
    
    // Operator & for custom types (non-const reference for nested serialization)
    template<typename T>
    auto operator&(T& value) -> decltype(value.template serialize<OutputArchive>(*this), *this) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        value.template serialize<OutputArchive>(*this);
        return *this;
    }

private:
    std::ostream& os_;
};

/**
 * InputArchive for deserialization (reading)
 * Supports the & operator pattern used by custom structs
 * This is the primary class for deserialization - Serializer delegates to this
 */
class InputArchive {
public:
    explicit InputArchive(std::istream& is) : is_(is) {}

    // Read fundamental types
    bool read(bool& value) {
        uint8_t byte;
        if (!is_.read(reinterpret_cast<char*>(&byte), sizeof(byte))) {
            failed_ = true;
            return false;
        }
        value = (byte != 0);
        return true;
    }

    bool read(char& value) {
        if (!is_.read(&value, sizeof(value))) {
            failed_ = true;
            return false;
        }
        return true;
    }

    bool read(int8_t& value) {
        if (!is_.read(reinterpret_cast<char*>(&value), sizeof(value))) {
            failed_ = true;
            return false;
        }
        return true;
    }

    bool read(uint8_t& value) {
        if (!is_.read(reinterpret_cast<char*>(&value), sizeof(value))) {
            failed_ = true;
            return false;
        }
        return true;
    }

    bool read(int16_t& value) {
        uint16_t uvalue;
        if (!is_.read(reinterpret_cast<char*>(&uvalue), sizeof(uvalue))) {
            failed_ = true;
            return false;
        }
        uvalue = detail::fromBigEndian(uvalue);
        value = static_cast<int16_t>(uvalue);
        return true;
    }

    bool read(uint16_t& value) {
        if (!is_.read(reinterpret_cast<char*>(&value), sizeof(value))) {
            failed_ = true;
            return false;
        }
        value = detail::fromBigEndian(value);
        return true;
    }

    bool read(int32_t& value) {
        uint32_t uvalue;
        if (!is_.read(reinterpret_cast<char*>(&uvalue), sizeof(uvalue))) {
            failed_ = true;
            return false;
        }
        uvalue = detail::fromBigEndian(uvalue);
        value = static_cast<int32_t>(uvalue);
        return true;
    }

    bool read(uint32_t& value) {
        if (!is_.read(reinterpret_cast<char*>(&value), sizeof(value))) {
            failed_ = true;
            return false;
        }
        value = detail::fromBigEndian(value);
        return true;
    }

    bool read(int64_t& value) {
        uint64_t uvalue;
        if (!is_.read(reinterpret_cast<char*>(&uvalue), sizeof(uvalue))) {
            failed_ = true;
            return false;
        }
        uvalue = detail::fromBigEndian(uvalue);
        value = static_cast<int64_t>(uvalue);
        return true;
    }

    bool read(uint64_t& value) {
        if (!is_.read(reinterpret_cast<char*>(&value), sizeof(value))) {
            failed_ = true;
            return false;
        }
        value = detail::fromBigEndian(value);
        return true;
    }

    bool read(float& value) {
        uint8_t bytes[sizeof(float)];
        if (!is_.read(reinterpret_cast<char*>(bytes), sizeof(float))) {
            failed_ = true;
            return false;
        }
        value = detail::floatFromBigEndian(bytes);
        return true;
    }

    bool read(double& value) {
        uint8_t bytes[sizeof(double)];
        if (!is_.read(reinterpret_cast<char*>(bytes), sizeof(double))) {
            failed_ = true;
            return false;
        }
        value = detail::doubleFromBigEndian(bytes);
        return true;
    }

    bool read(std::string& value) {
        uint64_t size;
        if (!read(size)) {
            return false;
        }
        
        value.resize(size);
        if (size > 0) {
            if (!is_.read(&value[0], size)) {
                failed_ = true;
                return false;
            }
        }
        return true;
    }

    // Read containers
    template<typename T>
    bool read(std::vector<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size;
        if (!read(size)) {
            return false;
        }
        value.clear();
        value.reserve(size);
        for (uint64_t i = 0; i < size; ++i) {
            T item;
            (*this) & item;
            if (failed_) {
                return false;
            }
            value.push_back(std::move(item));
        }
        return true;
    }

    template<typename T, size_t N>
    bool read(std::array<T, N>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        for (size_t i = 0; i < N; ++i) {
            (*this) & value[i];
            if (failed_) {
                return false;
            }
        }
        return true;
    }

    template<typename K, typename V>
    bool read(std::map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size;
        if (!read(size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            K key;
            V val;
            (*this) & key;
            (*this) & val;
            if (failed_) {
                return false;
            }
            value[std::move(key)] = std::move(val);
        }
        return true;
    }

    template<typename K, typename V>
    bool read(std::unordered_map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size;
        if (!read(size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            K key;
            V val;
            (*this) & key;
            (*this) & val;
            if (failed_) {
                return false;
            }
            value[std::move(key)] = std::move(val);
        }
        return true;
    }

    template<typename T>
    bool read(std::set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size;
        if (!read(size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            T item;
            (*this) & item;
            if (failed_) {
                return false;
            }
            value.insert(std::move(item));
        }
        return true;
    }

    template<typename T>
    bool read(std::unordered_set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size;
        if (!read(size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            T item;
            (*this) & item;
            if (failed_) {
                return false;
            }
            value.insert(std::move(item));
        }
        return true;
    }

    // Operator & for fundamental types
    InputArchive& operator&(bool& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(char& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(int8_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(uint8_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(int16_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(uint16_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(int32_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(uint32_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(int64_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(uint64_t& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(float& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(double& value) {
        read(value);
        return *this;
    }

    InputArchive& operator&(std::string& value) {
        read(value);
        return *this;
    }

    // Operator & for containers
    template<typename T>
    InputArchive& operator&(std::vector<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        read(value);
        return *this;
    }

    template<typename T, size_t N>
    InputArchive& operator&(std::array<T, N>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        read(value);
        return *this;
    }

    template<typename K, typename V>
    InputArchive& operator&(std::map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        read(value);
        return *this;
    }

    template<typename K, typename V>
    InputArchive& operator&(std::unordered_map<K, V>& value) {
        static_assert(!detail::is_pointer_v<K> && !detail::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<K> && !detail::is_long_double_v<V>, "Serializer does not support long double");
        read(value);
        return *this;
    }

    template<typename T>
    InputArchive& operator&(std::set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        read(value);
        return *this;
    }

    template<typename T>
    InputArchive& operator&(std::unordered_set<T>& value) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        read(value);
        return *this;
    }

    // Operator & for custom types
    template<typename T>
    auto operator&(T& value) -> decltype(value.template serialize<InputArchive>(*this), *this) {
        static_assert(!detail::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!detail::is_long_double_v<T>, "Serializer does not support long double");
        value.template serialize<InputArchive>(*this);
        return *this;
    }

    bool failed() const { return failed_; }

private:
    std::istream& is_;
    bool failed_ = false;
};

/**
 * Serializer class - provides a convenient static API for serialization
 * This class is now a thin wrapper around OutputArchive and InputArchive
 * 
 * @deprecated Prefer using OutputArchive and InputArchive directly for new code
 */
class Serializer {
public:
    /**
     * Serialize a value to binary format
     * @param value The value to serialize
     * @return Binary string representation
     */
    template<typename T>
    static std::string serialize(const T& value) {
        std::ostringstream oss;
        OutputArchive ar(oss);
        ar & value;
        return oss.str();
    }

    /**
     * Deserialize a value from binary format
     * @param data Binary string data
     * @param value Output parameter for deserialized value
     * @return true if successful, false otherwise
     */
    template<typename T>
    static bool deserialize(const std::string& data, T& value) {
        std::istringstream iss(data);
        InputArchive ar(iss);
        ar & value;
        return !ar.failed();
    }

    /**
     * Serialize a value to a stream
     * @param os Output stream
     * @param value The value to serialize
     */
    template<typename T>
    static void serializeToStream(std::ostream& os, const T& value) {
        OutputArchive ar(os);
        ar & value;
    }

    /**
     * Deserialize a value from a stream
     * @param is Input stream
     * @param value Output parameter for deserialized value
     * @return true if successful, false otherwise
     */
    template<typename T>
    static bool deserializeFromStream(std::istream& is, T& value) {
        InputArchive ar(is);
        ar & value;
        return !ar.failed();
    }
};

} // namespace pp

#endif // PP_LEDGER_SERIALIZER_H
