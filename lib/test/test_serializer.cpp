#include "Serializer.h"
#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>

using namespace pp;

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

// Test fundamental types
TEST(SerializerTest, FundamentalTypes) {
    // Test bool
    {
        bool original = true;
        std::string data = Serializer::serialize(original);
        bool deserialized = false;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test char
    {
        char original = 'A';
        std::string data = Serializer::serialize(original);
        char deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int8_t
    {
        int8_t original = -42;
        std::string data = Serializer::serialize(original);
        int8_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint8_t
    {
        uint8_t original = 200;
        std::string data = Serializer::serialize(original);
        uint8_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int16_t
    {
        int16_t original = -12345;
        std::string data = Serializer::serialize(original);
        int16_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint16_t
    {
        uint16_t original = 54321;
        std::string data = Serializer::serialize(original);
        uint16_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int32_t
    {
        int32_t original = -1234567890;
        std::string data = Serializer::serialize(original);
        int32_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint32_t
    {
        uint32_t original = 1234567890U;
        std::string data = Serializer::serialize(original);
        uint32_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test int64_t
    {
        int64_t original = -9223372036854775807LL;
        std::string data = Serializer::serialize(original);
        int64_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test uint64_t
    {
        uint64_t original = 18446744073709551615ULL;
        std::string data = Serializer::serialize(original);
        uint64_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test float
    {
        float original = 3.14159f;
        std::string data = Serializer::serialize(original);
        float deserialized = 0.0f;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_FLOAT_EQ(original, deserialized);
    }
    
    // Test double
    {
        double original = 3.141592653589793;
        std::string data = Serializer::serialize(original);
        double deserialized = 0.0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_DOUBLE_EQ(original, deserialized);
    }
}

// Test strings
TEST(SerializerTest, Strings) {
    // Test empty string
    {
        std::string original = "";
        std::string data = Serializer::serialize(original);
        std::string deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test normal string
    {
        std::string original = "Hello, World!";
        std::string data = Serializer::serialize(original);
        std::string deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test long string
    {
        std::string original(1000, 'A');
        std::string data = Serializer::serialize(original);
        std::string deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test string with special characters
    {
        std::string original = "Test\n\t\r\0\xFF";
        std::string data = Serializer::serialize(original);
        std::string deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test vectors
TEST(SerializerTest, Vectors) {
    // Test empty vector
    {
        std::vector<int> original;
        std::string data = Serializer::serialize(original);
        std::vector<int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test vector of ints
    {
        std::vector<int> original = {1, 2, 3, 4, 5};
        std::string data = Serializer::serialize(original);
        std::vector<int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test vector of strings
    {
        std::vector<std::string> original = {"one", "two", "three"};
        std::string data = Serializer::serialize(original);
        std::vector<std::string> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test nested vector
    {
        std::vector<std::vector<int>> original = {{1, 2}, {3, 4, 5}, {6}};
        std::string data = Serializer::serialize(original);
        std::vector<std::vector<int>> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test arrays
TEST(SerializerTest, Arrays) {
    // Test array of ints
    {
        std::array<int, 5> original = {1, 2, 3, 4, 5};
        std::string data = Serializer::serialize(original);
        std::array<int, 5> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test array of strings
    {
        std::array<std::string, 3> original = {"a", "b", "c"};
        std::string data = Serializer::serialize(original);
        std::array<std::string, 3> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test maps
TEST(SerializerTest, Maps) {
    // Test empty map
    {
        std::map<std::string, int> original;
        std::string data = Serializer::serialize(original);
        std::map<std::string, int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test map with values
    {
        std::map<std::string, int> original = {
            {"one", 1},
            {"two", 2},
            {"three", 3}
        };
        std::string data = Serializer::serialize(original);
        std::map<std::string, int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test unordered_map
    {
        std::unordered_map<std::string, int> original = {
            {"one", 1},
            {"two", 2},
            {"three", 3}
        };
        std::string data = Serializer::serialize(original);
        std::unordered_map<std::string, int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original.size(), deserialized.size());
        for (const auto& pair : original) {
            EXPECT_EQ(deserialized[pair.first], pair.second);
        }
    }
}

// Test sets
TEST(SerializerTest, Sets) {
    // Test empty set
    {
        std::set<int> original;
        std::string data = Serializer::serialize(original);
        std::set<int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test set with values
    {
        std::set<int> original = {3, 1, 4, 1, 5, 9, 2, 6};
        std::string data = Serializer::serialize(original);
        std::set<int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
    
    // Test unordered_set
    {
        std::unordered_set<std::string> original = {"apple", "banana", "cherry"};
        std::string data = Serializer::serialize(original);
        std::unordered_set<std::string> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original.size(), deserialized.size());
        for (const auto& item : original) {
            EXPECT_NE(deserialized.find(item), deserialized.end());
        }
    }
}

// Test custom structs with template serialize
TEST(SerializerTest, CustomStructs) {
    // Test simple struct
    {
        TestStruct original;
        original.id = 42;
        original.name = "Test";
        original.value = 3.14;
        
        std::string data = Serializer::serialize(original);
        TestStruct deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        
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
        
        std::string data = Serializer::serialize(original);
        NestedStruct deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        
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
        
        std::string data = Serializer::serialize(original);
        ContainerStruct deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        
        EXPECT_EQ(original.numbers, deserialized.numbers);
        EXPECT_EQ(original.keyValueMap, deserialized.keyValueMap);
    }
}

// Test stream-based serialization
TEST(SerializerTest, StreamSerialization) {
    int32_t original = 12345;
    
    std::ostringstream oss;
    Serializer::serializeToStream(oss, original);
    std::string data = oss.str();
    
    std::istringstream iss(data);
    int32_t deserialized = 0;
    ASSERT_TRUE(Serializer::deserializeFromStream(iss, deserialized));
    EXPECT_EQ(original, deserialized);
}

// Test complex nested structures
TEST(SerializerTest, ComplexNestedStructures) {
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
        
        std::string data = Serializer::serialize(original);
        std::vector<TestStruct> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        
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
        
        std::string data = Serializer::serialize(original);
        std::map<std::string, TestStruct> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        
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

// Test edge cases
TEST(SerializerTest, EdgeCases) {
    // Test zero values
    {
        int32_t original = 0;
        std::string data = Serializer::serialize(original);
        int32_t deserialized = -1;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(0, deserialized);
    }
    
    // Test maximum values
    {
        uint64_t original = UINT64_MAX;
        std::string data = Serializer::serialize(original);
        uint64_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(UINT64_MAX, deserialized);
    }
    
    // Test minimum values
    {
        int64_t original = INT64_MIN;
        std::string data = Serializer::serialize(original);
        int64_t deserialized = 0;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(INT64_MIN, deserialized);
    }
    
    // Test very large vector
    {
        std::vector<int> original(10000);
        for (size_t i = 0; i < original.size(); ++i) {
            original[i] = static_cast<int>(i);
        }
        std::string data = Serializer::serialize(original);
        std::vector<int> deserialized;
        ASSERT_TRUE(Serializer::deserialize(data, deserialized));
        EXPECT_EQ(original, deserialized);
    }
}

// Test invalid deserialization (should fail gracefully)
TEST(SerializerTest, InvalidDeserialization) {
    // Test empty data
    {
        std::string empty;
        int32_t value = 0;
        EXPECT_FALSE(Serializer::deserialize(empty, value));
    }
    
    // Test incomplete data (too short for int32_t which needs 4 bytes)
    {
        std::string incomplete(2, '\0');  // Only 2 bytes, need 4 for int32_t
        int32_t value = 0;
        EXPECT_FALSE(Serializer::deserialize(incomplete, value));
    }
    
    // Test incomplete data for string (missing size or data)
    {
        std::string incomplete(3, '\0');  // Not enough for string size (uint64_t = 8 bytes)
        std::string value;
        EXPECT_FALSE(Serializer::deserialize(incomplete, value));
    }
    
    // Test wrong type deserialization
    // Note: Binary formats may allow this, so we just verify it doesn't crash
    {
        int32_t original = 42;
        std::string data = Serializer::serialize(original);
        double deserialized = 0.0;
        // This might succeed but produce wrong value due to binary format differences
        // We just check it doesn't crash
        Serializer::deserialize(data, deserialized);
    }
}
