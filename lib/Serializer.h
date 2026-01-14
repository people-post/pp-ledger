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

/**
 * Serializer class for common serialization capabilities
 * Does NOT support pointers - only value types
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
        serializeValue(oss, value);
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
        return deserializeValue(iss, value);
    }

    /**
     * Serialize a value to a stream
     * @param os Output stream
     * @param value The value to serialize
     */
    template<typename T>
    static void serializeToStream(std::ostream& os, const T& value) {
        serializeValue(os, value);
    }

    /**
     * Deserialize a value from a stream
     * @param is Input stream
     * @param value Output parameter for deserialized value
     * @return true if successful, false otherwise
     */
    template<typename T>
    static bool deserializeFromStream(std::istream& is, T& value) {
        return deserializeValue(is, value);
    }

    // Friend declarations for Archive classes
    friend class OutputArchive;
    friend class InputArchive;

private:
    // Helper to check if type is a pointer (should not be serialized)
    template<typename T>
    static constexpr bool is_pointer_v = std::is_pointer_v<T>;
    
    // Helper to check if type is long double (not supported)
    template<typename T>
    static constexpr bool is_long_double_v = std::is_same_v<T, long double>;

    // Endian conversion helpers - convert to/from big endian (network byte order)
    // These ensure machine-independent serialization
    static uint16_t toBigEndian(uint16_t value);
    static uint32_t toBigEndian(uint32_t value);
    static uint64_t toBigEndian(uint64_t value);
    static uint16_t fromBigEndian(uint16_t value);
    static uint32_t fromBigEndian(uint32_t value);
    static uint64_t fromBigEndian(uint64_t value);
    
    // Floating point conversion helpers - convert to/from IEEE 754 big endian format
    static void floatToBigEndian(float value, uint8_t* bytes);
    static float floatFromBigEndian(const uint8_t* bytes);
    static void doubleToBigEndian(double value, uint8_t* bytes);
    static double doubleFromBigEndian(const uint8_t* bytes);

    // Serialization for fundamental types
    static void serializeValue(std::ostream& os, bool value);
    static void serializeValue(std::ostream& os, char value);
    static void serializeValue(std::ostream& os, int8_t value);
    static void serializeValue(std::ostream& os, uint8_t value);
    static void serializeValue(std::ostream& os, int16_t value);
    static void serializeValue(std::ostream& os, uint16_t value);
    static void serializeValue(std::ostream& os, int32_t value);
    static void serializeValue(std::ostream& os, uint32_t value);
    static void serializeValue(std::ostream& os, int64_t value);
    static void serializeValue(std::ostream& os, uint64_t value);
    static void serializeValue(std::ostream& os, float value);
    static void serializeValue(std::ostream& os, double value);

    // Serialization for strings
    static void serializeValue(std::ostream& os, const std::string& value);
    static void serializeValue(std::ostream& os, const char* value);

    // Serialization for containers
    template<typename T>
    static void serializeValue(std::ostream& os, const std::vector<T>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size = value.size();
        serializeValue(os, size);
        for (const auto& item : value) {
            serializeValue(os, item);
        }
    }

    template<typename T, size_t N>
    static void serializeValue(std::ostream& os, const std::array<T, N>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        for (const auto& item : value) {
            serializeValue(os, item);
        }
    }

    template<typename K, typename V>
    static void serializeValue(std::ostream& os, const std::map<K, V>& value) {
        static_assert(!is_pointer_v<K> && !is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<K> && !is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size = value.size();
        serializeValue(os, size);
        for (const auto& pair : value) {
            serializeValue(os, pair.first);
            serializeValue(os, pair.second);
        }
    }

    template<typename K, typename V>
    static void serializeValue(std::ostream& os, const std::unordered_map<K, V>& value) {
        static_assert(!is_pointer_v<K> && !is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<K> && !is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size = value.size();
        serializeValue(os, size);
        for (const auto& pair : value) {
            serializeValue(os, pair.first);
            serializeValue(os, pair.second);
        }
    }

    template<typename T>
    static void serializeValue(std::ostream& os, const std::set<T>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size = value.size();
        serializeValue(os, size);
        for (const auto& item : value) {
            serializeValue(os, item);
        }
    }

    template<typename T>
    static void serializeValue(std::ostream& os, const std::unordered_set<T>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size = value.size();
        serializeValue(os, size);
        for (const auto& item : value) {
            serializeValue(os, item);
        }
    }

    // Deserialization for fundamental types
    static bool deserializeValue(std::istream& is, bool& value);
    static bool deserializeValue(std::istream& is, char& value);
    static bool deserializeValue(std::istream& is, int8_t& value);
    static bool deserializeValue(std::istream& is, uint8_t& value);
    static bool deserializeValue(std::istream& is, int16_t& value);
    static bool deserializeValue(std::istream& is, uint16_t& value);
    static bool deserializeValue(std::istream& is, int32_t& value);
    static bool deserializeValue(std::istream& is, uint32_t& value);
    static bool deserializeValue(std::istream& is, int64_t& value);
    static bool deserializeValue(std::istream& is, uint64_t& value);
    static bool deserializeValue(std::istream& is, float& value);
    static bool deserializeValue(std::istream& is, double& value);

    // Deserialization for strings
    static bool deserializeValue(std::istream& is, std::string& value);

    // Deserialization for containers
    template<typename T>
    static bool deserializeValue(std::istream& is, std::vector<T>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size;
        if (!deserializeValue(is, size)) {
            return false;
        }
        value.clear();
        value.reserve(size);
        for (uint64_t i = 0; i < size; ++i) {
            T item;
            if (!deserializeValue(is, item)) {
                return false;
            }
            value.push_back(item);
        }
        return true;
    }

    template<typename T, size_t N>
    static bool deserializeValue(std::istream& is, std::array<T, N>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        for (size_t i = 0; i < N; ++i) {
            if (!deserializeValue(is, value[i])) {
                return false;
            }
        }
        return true;
    }

    template<typename K, typename V>
    static bool deserializeValue(std::istream& is, std::map<K, V>& value) {
        static_assert(!is_pointer_v<K> && !is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<K> && !is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size;
        if (!deserializeValue(is, size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            K key;
            V val;
            if (!deserializeValue(is, key) || !deserializeValue(is, val)) {
                return false;
            }
            value[key] = val;
        }
        return true;
    }

    template<typename K, typename V>
    static bool deserializeValue(std::istream& is, std::unordered_map<K, V>& value) {
        static_assert(!is_pointer_v<K> && !is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<K> && !is_long_double_v<V>, "Serializer does not support long double");
        uint64_t size;
        if (!deserializeValue(is, size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            K key;
            V val;
            if (!deserializeValue(is, key) || !deserializeValue(is, val)) {
                return false;
            }
            value[key] = val;
        }
        return true;
    }

    template<typename T>
    static bool deserializeValue(std::istream& is, std::set<T>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size;
        if (!deserializeValue(is, size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            T item;
            if (!deserializeValue(is, item)) {
                return false;
            }
            value.insert(item);
        }
        return true;
    }

    template<typename T>
    static bool deserializeValue(std::istream& is, std::unordered_set<T>& value) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        uint64_t size;
        if (!deserializeValue(is, size)) {
            return false;
        }
        value.clear();
        for (uint64_t i = 0; i < size; ++i) {
            T item;
            if (!deserializeValue(is, item)) {
                return false;
            }
            value.insert(item);
        }
        return true;
    }

    // For custom types that provide serialize/deserialize methods (non-template)
    template<typename T>
    static auto serializeValue(std::ostream& os, const T& value) 
        -> decltype(value.serialize(os), void()) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        value.serialize(os);
    }

    template<typename T>
    static auto deserializeValue(std::istream& is, T& value)
        -> decltype(value.deserialize(is), bool()) {
        static_assert(!is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!is_long_double_v<T>, "Serializer does not support long double");
        return value.deserialize(is);
    }

    // For custom types that provide template serialize(Archive&) method
    // This handles the pattern: template<typename Archive> void serialize(Archive& ar) { ar & member; }
    // Implementation is provided after Archive classes are defined (see end of file)
    template<typename T>
    static auto serializeValue(std::ostream& os, const T& value)
        -> decltype(const_cast<T&>(value).template serialize<OutputArchive>(std::declval<OutputArchive&>()), void());
    
    template<typename T>
    static auto deserializeValue(std::istream& is, T& value)
        -> decltype(value.template serialize<InputArchive>(std::declval<InputArchive&>()), bool());
};

/**
 * OutputArchive for serialization (writing)
 * Supports the & operator pattern used by custom structs
 */
class OutputArchive {
public:
    explicit OutputArchive(std::ostream& os) : os_(os) {}

    // Operator & for fundamental types
    OutputArchive& operator&(bool value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(char value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(int8_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(uint8_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(int16_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(uint16_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(int32_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(uint32_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(int64_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(uint64_t value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(float value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(double value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(const std::string& value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    OutputArchive& operator&(const char* value) {
        Serializer::serializeValue(os_, value);
        return *this;
    }

    // Operator & for containers
    template<typename T>
    OutputArchive& operator&(const std::vector<T>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        Serializer::serializeValue(os_, value);
        return *this;
    }

    template<typename T, size_t N>
    OutputArchive& operator&(const std::array<T, N>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        Serializer::serializeValue(os_, value);
        return *this;
    }

    template<typename K, typename V>
    OutputArchive& operator&(const std::map<K, V>& value) {
        static_assert(!std::is_pointer_v<K> && !std::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<K, long double> && !std::is_same_v<V, long double>, "Serializer does not support long double");
        Serializer::serializeValue(os_, value);
        return *this;
    }

    template<typename K, typename V>
    OutputArchive& operator&(const std::unordered_map<K, V>& value) {
        static_assert(!std::is_pointer_v<K> && !std::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<K, long double> && !std::is_same_v<V, long double>, "Serializer does not support long double");
        Serializer::serializeValue(os_, value);
        return *this;
    }

    template<typename T>
    OutputArchive& operator&(const std::set<T>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        Serializer::serializeValue(os_, value);
        return *this;
    }

    template<typename T>
    OutputArchive& operator&(const std::unordered_set<T>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        Serializer::serializeValue(os_, value);
        return *this;
    }

    // Operator & for custom types (const reference)
    template<typename T>
    auto operator&(const T& value) -> decltype(value.template serialize<OutputArchive>(*this), *this) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        const_cast<T&>(value).template serialize<OutputArchive>(*this);
        return *this;
    }
    
    // Operator & for custom types (non-const reference for nested serialization)
    template<typename T>
    auto operator&(T& value) -> decltype(value.template serialize<OutputArchive>(*this), *this) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        value.template serialize<OutputArchive>(*this);
        return *this;
    }

private:
    std::ostream& os_;
};

/**
 * InputArchive for deserialization (reading)
 * Supports the & operator pattern used by custom structs
 */
class InputArchive {
public:
    explicit InputArchive(std::istream& is) : is_(is) {}

    // Operator & for fundamental types
    InputArchive& operator&(bool& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(char& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(int8_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(uint8_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(int16_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(uint16_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(int32_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(uint32_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(int64_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(uint64_t& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(float& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(double& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    InputArchive& operator&(std::string& value) {
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    // Operator & for containers
    template<typename T>
    InputArchive& operator&(std::vector<T>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    template<typename T, size_t N>
    InputArchive& operator&(std::array<T, N>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    template<typename K, typename V>
    InputArchive& operator&(std::map<K, V>& value) {
        static_assert(!std::is_pointer_v<K> && !std::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<K, long double> && !std::is_same_v<V, long double>, "Serializer does not support long double");
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    template<typename K, typename V>
    InputArchive& operator&(std::unordered_map<K, V>& value) {
        static_assert(!std::is_pointer_v<K> && !std::is_pointer_v<V>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<K, long double> && !std::is_same_v<V, long double>, "Serializer does not support long double");
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    template<typename T>
    InputArchive& operator&(std::set<T>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    template<typename T>
    InputArchive& operator&(std::unordered_set<T>& value) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        if (!Serializer::deserializeValue(is_, value)) {
            failed_ = true;
        }
        return *this;
    }

    // Operator & for custom types
    template<typename T>
    auto operator&(T& value) -> decltype(value.template serialize<InputArchive>(*this), *this) {
        static_assert(!std::is_pointer_v<T>, "Serializer does not support pointers");
        static_assert(!std::is_same_v<T, long double>, "Serializer does not support long double");
        value.template serialize<InputArchive>(*this);
        return *this;
    }

    bool failed() const { return failed_; }

private:
    std::istream& is_;
    bool failed_ = false;
};

// Implementation of Serializer methods that use Archive (must be after Archive is defined)
template<typename T>
auto Serializer::serializeValue(std::ostream& os, const T& value)
    -> decltype(const_cast<T&>(value).template serialize<OutputArchive>(std::declval<OutputArchive&>()), void()) {
    static_assert(!Serializer::is_pointer_v<T>, "Serializer does not support pointers");
    static_assert(!Serializer::is_long_double_v<T>, "Serializer does not support long double");
    OutputArchive ar(os);
    const_cast<T&>(value).template serialize<OutputArchive>(ar);
}

template<typename T>
auto Serializer::deserializeValue(std::istream& is, T& value)
    -> decltype(value.template serialize<InputArchive>(std::declval<InputArchive&>()), bool()) {
    static_assert(!Serializer::is_pointer_v<T>, "Serializer does not support pointers");
    static_assert(!Serializer::is_long_double_v<T>, "Serializer does not support long double");
    InputArchive ar(is);
    value.template serialize<InputArchive>(ar);
    return !ar.failed();
}

} // namespace pp

#endif // PP_LEDGER_SERIALIZER_H
