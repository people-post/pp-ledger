#include "BinaryPack.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <sstream>

using namespace pp;

// Helper function to serialize using Archive
template<typename T>
std::string archivePack(const T& value) {
    std::ostringstream oss;
    OutputArchive ar(oss);
    ar & value;
    return oss.str();
}

// Helper function to deserialize using Archive
template<typename T>
bool archiveUnpack(const std::string& data, T& value) {
    std::istringstream iss(data);
    InputArchive ar(iss);
    ar & value;
    return !ar.failed();
}

// Test custom struct with template serialize method
struct TestStruct {
    int32_t id;
    std::string name;
    double value;
    
    template <typename Archive>
    void serialize(Archive& ar) {
        ar & id;
        ar & name;
        ar & value;
    }
};

// Test nested struct
struct NestedStruct {
    TestStruct inner;
    uint64_t count;
    
    template <typename Archive>
    void serialize(Archive& ar) {
        ar & inner;
        ar & count;
    }
};

// Test struct with containers
struct ContainerStruct {
    std::vector<int> numbers;
    std::map<std::string, int> keyValueMap;
    
    template <typename Archive>
    void serialize(Archive& ar) {
        ar & numbers;
        ar & keyValueMap;
    }
};

// Test fundamental types using Archive
TEST(ArchiveTest, FundamentalTypes) {
    // Test bool
    {
        bool original = true;
        std::string data = archivePack(original);
        bool deserialized = false;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test char
    {
        char original = 'A';
        std::string data = archivePack(original);
        char deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int8_t
    {
        int8_t original = -42;
        std::string data = archivePack(original);
        int8_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint8_t
    {
        uint8_t original = 200;
        std::string data = archivePack(original);
        uint8_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int16_t
    {
        int16_t original = -12345;
        std::string data = archivePack(original);
        int16_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint16_t
    {
        uint16_t original = 54321;
        std::string data = archivePack(original);
        uint16_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int32_t
    {
        int32_t original = -1234567890;
        std::string data = archivePack(original);
        int32_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint32_t
    {
        uint32_t original = 1234567890U;
        std::string data = archivePack(original);
        uint32_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int64_t
    {
        int64_t original = -9223372036854775807LL;
        std::string data = archivePack(original);
        int64_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint64_t
    {
        uint64_t original = 18446744073709551615ULL;
        std::string data = archivePack(original);
        uint64_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test float
    {
        float original = 3.14159f;
        std::string data = archivePack(original);
        float deserialized = 0.0f;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_FLOAT_EQ(original, deserialized);
    }
    
    // Test double
    {
        double original = 3.141592653589793;
        std::string data = archivePack(original);
        double deserialized = 0.0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_DOUBLE_EQ(original, deserialized);
    }
}

// Test strings using Archive
TEST(ArchiveTest, Strings) {
    // Test empty string
    {
        std::string original = "";
        std::string data = archivePack(original);
        std::string deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test normal string
    {
        std::string original = "Hello, World!";
        std::string data = archivePack(original);
        std::string deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test long string
    {
        std::string original(1000, 'A');
        std::string data = archivePack(original);
        std::string deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test string with special characters
    {
        std::string original = "Test\n\t\r\0\xFF";
        std::string data = archivePack(original);
        std::string deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test vectors using Archive
TEST(ArchiveTest, Vectors) {
    // Test empty vector
    {
        std::vector<int> original;
        std::string data = archivePack(original);
        std::vector<int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test vector of ints
    {
        std::vector<int> original = {1, 2, 3, 4, 5};
        std::string data = archivePack(original);
        std::vector<int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test vector of strings
    {
        std::vector<std::string> original = {"one", "two", "three"};
        std::string data = archivePack(original);
        std::vector<std::string> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test nested vector
    {
        std::vector<std::vector<int>> original = {{1, 2}, {3, 4, 5}, {6}};
        std::string data = archivePack(original);
        std::vector<std::vector<int>> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test arrays using Archive
TEST(ArchiveTest, Arrays) {
    // Test array of ints
    {
        std::array<int, 5> original = {1, 2, 3, 4, 5};
        std::string data = archivePack(original);
        std::array<int, 5> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test array of strings
    {
        std::array<std::string, 3> original = {"a", "b", "c"};
        std::string data = archivePack(original);
        std::array<std::string, 3> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test maps using Archive
TEST(ArchiveTest, Maps) {
    // Test empty map
    {
        std::map<std::string, int> original;
        std::string data = archivePack(original);
        std::map<std::string, int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test map with values
    {
        std::map<std::string, int> original = {
            {"one", 1},
            {"two", 2},
            {"three", 3}
        };
        std::string data = archivePack(original);
        std::map<std::string, int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test unordered_map
    {
        std::unordered_map<std::string, int> original = {
            {"one", 1},
            {"two", 2},
            {"three", 3}
        };
        std::string data = archivePack(original);
        std::unordered_map<std::string, int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original.size(), deserialized.size());
        for (const auto& pair : original) {
            EXPECT_EQ(deserialized[pair.first], pair.second);
        }
    }
}

// Test sets using Archive
TEST(ArchiveTest, Sets) {
    // Test empty set
    {
        std::set<int> original;
        std::string data = archivePack(original);
        std::set<int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test set with values
    {
        std::set<int> original = {3, 1, 4, 1, 5, 9, 2, 6};
        std::string data = archivePack(original);
        std::set<int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test unordered_set
    {
        std::unordered_set<std::string> original = {"apple", "banana", "cherry"};
        std::string data = archivePack(original);
        std::unordered_set<std::string> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original.size(), deserialized.size());
        for (const auto& item : original) {
            EXPECT_NE(deserialized.find(item), deserialized.end());
        }
    }
}

// Test custom structs with template serialize using Archive
TEST(ArchiveTest, CustomStructs) {
    // Test simple struct
    {
        TestStruct original;
        original.id = 42;
        original.name = "Test";
        original.value = 3.14;
        
        std::string data = archivePack(original);
        TestStruct deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        
        EXPECT_EQ(original.id, deserialized.id);
        EXPECT_EQ(original.name, deserialized.name);
        EXPECT_DOUBLE_EQ(original.value, deserialized.value);
    }
    
    // Test nested struct
    {
        NestedStruct original;
        original.inner.id = 100;
        original.inner.name = "Nested";
        original.inner.value = 2.718;
        original.count = 999;
        
        std::string data = archivePack(original);
        NestedStruct deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        
        EXPECT_EQ(original.inner.id, deserialized.inner.id);
        EXPECT_EQ(original.inner.name, deserialized.inner.name);
        EXPECT_DOUBLE_EQ(original.inner.value, deserialized.inner.value);
        EXPECT_EQ(original.count, deserialized.count);
    }
    
    // Test struct with containers
    {
        ContainerStruct original;
        original.numbers = {1, 2, 3, 4, 5};
        original.keyValueMap = {{"a", 1}, {"b", 2}, {"c", 3}};
        
        std::string data = archivePack(original);
        ContainerStruct deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        
        EXPECT_EQ(original.numbers, deserialized.numbers);
        EXPECT_EQ(original.keyValueMap, deserialized.keyValueMap);
    }
}

// Test stream-based Archive usage
TEST(ArchiveTest, StreamSerialization) {
    int32_t original = 12345;
    
    std::ostringstream oss;
    OutputArchive outAr(oss);
    outAr & original;
    std::string data = oss.str();
    
    std::istringstream iss(data);
    InputArchive inAr(iss);
    int32_t deserialized = 0;
    inAr & deserialized;
    ASSERT_FALSE(inAr.failed());
    EXPECT_EQ(original, deserialized);
}

// Test complex nested structures using Archive
TEST(ArchiveTest, ComplexNestedStructures) {
    // Test vector of custom structs
    {
        std::vector<TestStruct> original;
        for (int i = 0; i < 5; ++i) {
            TestStruct s;
            s.id = i;
            s.name = "Item" + std::to_string(i);
            s.value = i * 1.5;
            original.push_back(s);
        }
        
        std::string data = archivePack(original);
        std::vector<TestStruct> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        
        ASSERT_EQ(original.size(), deserialized.size());
        for (size_t i = 0; i < original.size(); ++i) {
            EXPECT_EQ(original[i].id, deserialized[i].id);
            EXPECT_EQ(original[i].name, deserialized[i].name);
            EXPECT_DOUBLE_EQ(original[i].value, deserialized[i].value);
        }
    }
    
    // Test map with custom struct values
    {
        std::map<std::string, TestStruct> original;
        for (int i = 0; i < 3; ++i) {
            TestStruct s;
            s.id = i;
            s.name = "Struct" + std::to_string(i);
            s.value = i * 2.5;
            original["key" + std::to_string(i)] = s;
        }
        
        std::string data = archivePack(original);
        std::map<std::string, TestStruct> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        
        ASSERT_EQ(original.size(), deserialized.size());
        for (const auto& pair : original) {
            ASSERT_NE(deserialized.find(pair.first), deserialized.end());
            const TestStruct& orig = pair.second;
            const TestStruct& deser = deserialized[pair.first];
            EXPECT_EQ(orig.id, deser.id);
            EXPECT_EQ(orig.name, deser.name);
            EXPECT_DOUBLE_EQ(orig.value, deser.value);
        }
    }
}

// Test edge cases using Archive
TEST(ArchiveTest, EdgeCases) {
    // Test zero values
    {
        int32_t original = 0;
        std::string data = archivePack(original);
        int32_t deserialized = -1;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(0, deserialized);
    }
    
    // Test maximum values
    {
        uint64_t original = UINT64_MAX;
        std::string data = archivePack(original);
        uint64_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(UINT64_MAX, deserialized);
    }
    
    // Test minimum values
    {
        int64_t original = INT64_MIN;
        std::string data = archivePack(original);
        int64_t deserialized = 0;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(INT64_MIN, deserialized);
    }
    
    // Test very large vector
    {
        std::vector<int> original(10000);
        for (size_t i = 0; i < original.size(); ++i) {
            original[i] = static_cast<int>(i);
        }
        std::string data = archivePack(original);
        std::vector<int> deserialized;
        ASSERT_TRUE(archiveUnpack(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test invalid deserialization (should fail gracefully) using Archive
TEST(ArchiveTest, InvalidDeserialization) {
    // Test empty data
    {
        std::string empty;
        int32_t value = 0;
        EXPECT_FALSE(archiveUnpack(empty, value));
    }
    
    // Test incomplete data (too short for int32_t which needs 4 bytes)
    {
        std::string incomplete(2, '\0');  // Only 2 bytes, need 4 for int32_t
        int32_t value = 0;
        EXPECT_FALSE(archiveUnpack(incomplete, value));
    }
    
    // Test incomplete data for string (missing size or data)
    {
        std::string incomplete(3, '\0');  // Not enough for string size (uint64_t = 8 bytes)
        std::string value;
        EXPECT_FALSE(archiveUnpack(incomplete, value));
    }
    
    // Test wrong type deserialization
    // Note: Binary formats may allow this, so we just verify it doesn't crash
    {
        int32_t original = 42;
        std::string data = archivePack(original);
        double deserialized = 0.0;
        // This might succeed but produce wrong value due to binary format differences
        // We just check it doesn't crash
        archiveUnpack(data, deserialized);
    }
}

// Test binaryPack and binaryUnpack functions
TEST(ArchiveTest, BinaryPackUnpack) {
    // Test simple type
    {
        int32_t original = 12345;
        std::string data = utl::binaryPack(original);
        auto result = utl::binaryUnpack<int32_t>(data);
        ASSERT_TRUE(result.isOk());
        EXPECT_EQ(original, result.value());
    }
    
    // Test custom struct
    {
        TestStruct original;
        original.id = 42;
        original.name = "BinaryPack Test";
        original.value = 99.9;
        
        std::string data = utl::binaryPack(original);
        auto result = utl::binaryUnpack<TestStruct>(data);
        ASSERT_TRUE(result.isOk());
        
        const auto& deserialized = result.value();
        EXPECT_EQ(original.id, deserialized.id);
        EXPECT_EQ(original.name, deserialized.name);
        EXPECT_DOUBLE_EQ(original.value, deserialized.value);
    }
    
    // Test error case
    {
        std::string badData = "x";  // Invalid data
        auto result = utl::binaryUnpack<int32_t>(badData);
        EXPECT_TRUE(result.isError());
    }
}
