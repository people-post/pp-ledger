#include "Serializer.h"
#include <cstring>

namespace pp {

// Endian conversion helpers
// We use portable byte-by-byte conversion to ensure machine independence
// All values are stored in big endian (network byte order) format

namespace {
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
}

uint16_t Serializer::toBigEndian(uint16_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

uint32_t Serializer::toBigEndian(uint32_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

uint64_t Serializer::toBigEndian(uint64_t value) {
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

uint16_t Serializer::fromBigEndian(uint16_t value) {
    // Read bytes as if they're in big endian format, convert to host byte order
    // This is the same as toBigEndian (swap if little endian)
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

uint32_t Serializer::fromBigEndian(uint32_t value) {
    // Read bytes as if they're in big endian format, convert to host byte order
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

uint64_t Serializer::fromBigEndian(uint64_t value) {
    // Read bytes as if they're in big endian format, convert to host byte order
    if (isLittleEndian()) {
        return swapBytes(value);
    } else {
        return value;  // Already big endian
    }
}

void Serializer::floatToBigEndian(float value, uint8_t* bytes) {
    // Convert float to IEEE 754 representation in big endian
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(float));
    bits = toBigEndian(bits);
    std::memcpy(bytes, &bits, sizeof(float));
}

float Serializer::floatFromBigEndian(const uint8_t* bytes) {
    // Convert from IEEE 754 big endian representation
    uint32_t bits;
    std::memcpy(&bits, bytes, sizeof(float));
    bits = fromBigEndian(bits);
    float value;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
}

void Serializer::doubleToBigEndian(double value, uint8_t* bytes) {
    // Convert double to IEEE 754 representation in big endian
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(double));
    bits = toBigEndian(bits);
    std::memcpy(bytes, &bits, sizeof(double));
}

double Serializer::doubleFromBigEndian(const uint8_t* bytes) {
    // Convert from IEEE 754 big endian representation
    uint64_t bits;
    std::memcpy(&bits, bytes, sizeof(double));
    bits = fromBigEndian(bits);
    double value;
    std::memcpy(&value, &bits, sizeof(double));
    return value;
}

// Serialization implementations for fundamental types
void Serializer::serializeValue(std::ostream& os, bool value) {
    uint8_t byte = value ? 1 : 0;
    os.write(reinterpret_cast<const char*>(&byte), sizeof(byte));
}

void Serializer::serializeValue(std::ostream& os, char value) {
    os.write(&value, sizeof(value));
}

void Serializer::serializeValue(std::ostream& os, int8_t value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void Serializer::serializeValue(std::ostream& os, uint8_t value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void Serializer::serializeValue(std::ostream& os, int16_t value) {
    // Convert to big endian for machine independence
    uint16_t uvalue = static_cast<uint16_t>(value);
    uvalue = toBigEndian(uvalue);
    os.write(reinterpret_cast<const char*>(&uvalue), sizeof(uvalue));
}

void Serializer::serializeValue(std::ostream& os, uint16_t value) {
    value = toBigEndian(value);
    os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void Serializer::serializeValue(std::ostream& os, int32_t value) {
    // Convert to big endian for machine independence
    uint32_t uvalue = static_cast<uint32_t>(value);
    uvalue = toBigEndian(uvalue);
    os.write(reinterpret_cast<const char*>(&uvalue), sizeof(uvalue));
}

void Serializer::serializeValue(std::ostream& os, uint32_t value) {
    value = toBigEndian(value);
    os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void Serializer::serializeValue(std::ostream& os, int64_t value) {
    // Convert to big endian for machine independence
    uint64_t uvalue = static_cast<uint64_t>(value);
    uvalue = toBigEndian(uvalue);
    os.write(reinterpret_cast<const char*>(&uvalue), sizeof(uvalue));
}

void Serializer::serializeValue(std::ostream& os, uint64_t value) {
    value = toBigEndian(value);
    os.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void Serializer::serializeValue(std::ostream& os, float value) {
    // Convert to IEEE 754 big endian format
    uint8_t bytes[sizeof(float)];
    floatToBigEndian(value, bytes);
    os.write(reinterpret_cast<const char*>(bytes), sizeof(float));
}

void Serializer::serializeValue(std::ostream& os, double value) {
    // Convert to IEEE 754 big endian format
    uint8_t bytes[sizeof(double)];
    doubleToBigEndian(value, bytes);
    os.write(reinterpret_cast<const char*>(bytes), sizeof(double));
}

// Serialization for strings
void Serializer::serializeValue(std::ostream& os, const std::string& value) {
    uint64_t size = value.size();
    serializeValue(os, size);
    if (size > 0) {
        os.write(value.data(), size);
    }
}

void Serializer::serializeValue(std::ostream& os, const char* value) {
    if (value == nullptr) {
        uint64_t size = 0;
        serializeValue(os, size);
        return;
    }
    std::string str(value);
    serializeValue(os, str);
}

// Deserialization implementations for fundamental types
bool Serializer::deserializeValue(std::istream& is, bool& value) {
    uint8_t byte;
    if (!is.read(reinterpret_cast<char*>(&byte), sizeof(byte))) {
        return false;
    }
    value = (byte != 0);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, char& value) {
    if (!is.read(&value, sizeof(value))) {
        return false;
    }
    return true;
}

bool Serializer::deserializeValue(std::istream& is, int8_t& value) {
    if (!is.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        return false;
    }
    return true;
}

bool Serializer::deserializeValue(std::istream& is, uint8_t& value) {
    if (!is.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        return false;
    }
    return true;
}

bool Serializer::deserializeValue(std::istream& is, int16_t& value) {
    uint16_t uvalue;
    if (!is.read(reinterpret_cast<char*>(&uvalue), sizeof(uvalue))) {
        return false;
    }
    uvalue = fromBigEndian(uvalue);
    value = static_cast<int16_t>(uvalue);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, uint16_t& value) {
    if (!is.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        return false;
    }
    value = fromBigEndian(value);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, int32_t& value) {
    uint32_t uvalue;
    if (!is.read(reinterpret_cast<char*>(&uvalue), sizeof(uvalue))) {
        return false;
    }
    uvalue = fromBigEndian(uvalue);
    value = static_cast<int32_t>(uvalue);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, uint32_t& value) {
    if (!is.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        return false;
    }
    value = fromBigEndian(value);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, int64_t& value) {
    uint64_t uvalue;
    if (!is.read(reinterpret_cast<char*>(&uvalue), sizeof(uvalue))) {
        return false;
    }
    uvalue = fromBigEndian(uvalue);
    value = static_cast<int64_t>(uvalue);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, uint64_t& value) {
    if (!is.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        return false;
    }
    value = fromBigEndian(value);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, float& value) {
    uint8_t bytes[sizeof(float)];
    if (!is.read(reinterpret_cast<char*>(bytes), sizeof(float))) {
        return false;
    }
    value = floatFromBigEndian(bytes);
    return true;
}

bool Serializer::deserializeValue(std::istream& is, double& value) {
    uint8_t bytes[sizeof(double)];
    if (!is.read(reinterpret_cast<char*>(bytes), sizeof(double))) {
        return false;
    }
    value = doubleFromBigEndian(bytes);
    return true;
}

// Deserialization for strings
bool Serializer::deserializeValue(std::istream& is, std::string& value) {
    uint64_t size;
    if (!deserializeValue(is, size)) {
        return false;
    }
    
    value.resize(size);
    if (size > 0) {
        if (!is.read(&value[0], size)) {
            return false;
        }
    }
    return true;
}

} // namespace pp
