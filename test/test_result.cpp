#include "ResultOrError.h"
#include "Logger.h"

#include <iostream>
#include <string>

// Example function that returns ResultOrError
pp::ResultOrError<int> divide(int a, int b) {
    if (b == 0) {
        return pp::ResultOrError<int>::error("Division by zero");
    }
    return a / b;
}

// Example function with void return
pp::ResultOrError<void> validatePositive(int value) {
    if (value <= 0) {
        return pp::ResultOrError<void>::error("Value must be positive");
    }
    return {};
}

// Example with custom error type
struct ErrorInfo {
    int code;
    std::string message;
};

pp::ResultOrError<std::string, ErrorInfo> processData(const std::string& data) {
    if (data.empty()) {
        return pp::ResultOrError<std::string, ErrorInfo>::error({1, "Data is empty"});
    }
    if (data.length() > 100) {
        return pp::ResultOrError<std::string, ErrorInfo>::error({2, "Data too long"});
    }
    return "Processed: " + data;
}

int main() {
    auto& logger = pp::logging::getLogger("result_test");
    
    std::cout << "=== Testing ResultOrError ===\n\n";
    
    // Test 1: Success case
    std::cout << "1. Testing success case:\n";
    auto result1 = divide(10, 2);
    if (result1.isOk()) {
        logger.info << "Division result: " << result1.value();
        std::cout << "  Result: " << result1.value() << "\n";
    } else {
        logger.error << "Error: " << result1.error();
        std::cout << "  Error: " << result1.error() << "\n";
    }
    
    // Test 2: Error case
    std::cout << "\n2. Testing error case:\n";
    auto result2 = divide(10, 0);
    if (result2.isOk()) {
        std::cout << "  Result: " << result2.value() << "\n";
    } else {
        logger.error << "Expected error: " << result2.error();
        std::cout << "  Error: " << result2.error() << "\n";
    }
    
    // Test 3: Using explicit bool conversion
    std::cout << "\n3. Testing bool conversion:\n";
    auto result3 = divide(20, 4);
    if (result3) {
        logger.info << "Division succeeded: " << *result3;
        std::cout << "  Success! Result: " << *result3 << "\n";
    } else {
        std::cout << "  Failed: " << result3.error() << "\n";
    }
    
    // Test 4: Using valueOr
    std::cout << "\n4. Testing valueOr:\n";
    auto result4 = divide(10, 0);
    int value = result4.valueOr(-1);
    logger.info << "Result with default: " << value;
    std::cout << "  Result (with default -1): " << value << "\n";
    
    // Test 5: Void return type
    std::cout << "\n5. Testing void return type:\n";
    auto result5 = validatePositive(10);
    if (result5.isOk()) {
        logger.info << "Validation passed";
        std::cout << "  Validation passed\n";
    } else {
        std::cout << "  Validation failed: " << result5.error() << "\n";
    }
    
    auto result6 = validatePositive(-5);
    if (result6.isOk()) {
        std::cout << "  Validation passed\n";
    } else {
        logger.error << "Validation failed: " << result6.error();
        std::cout << "  Validation failed: " << result6.error() << "\n";
    }
    
    // Test 6: Custom error type
    std::cout << "\n6. Testing custom error type:\n";
    auto result7 = processData("Hello, World!");
    if (result7.isOk()) {
        logger.info << "Processed: " << result7.value();
        std::cout << "  " << result7.value() << "\n";
    } else {
        std::cout << "  Error: " << result7.error().message << "\n";
    }
    
    auto result8 = processData("");
    if (result8.isOk()) {
        std::cout << "  " << result8.value() << "\n";
    } else {
        logger.error << "Error code " << result8.error().code << ": " << result8.error().message;
        std::cout << "  Error [" << result8.error().code << "]: " 
                  << result8.error().message << "\n";
    }
    
    // Test 7: Chaining operations
    std::cout << "\n7. Testing operation chaining:\n";
    auto computeAndLog = [&logger](int a, int b) {
        auto result = divide(a, b);
        if (result) {
            logger.info << "Computed: " << a << " / " << b << " = " << *result;
            return result;
        } else {
            logger.error << "Failed: " << a << " / " << b << " - " << result.error();
            return result;
        }
    };
    
    auto res1 = computeAndLog(100, 5);
    auto res2 = computeAndLog(50, 0);
    
    std::cout << "  First: " << (res1 ? "Success" : "Failed") << "\n";
    std::cout << "  Second: " << (res2 ? "Success" : "Failed") << "\n";
    
    logger.info << "Test complete";
    std::cout << "\n=== Test Complete ===\n";
    
    return 0;
}
