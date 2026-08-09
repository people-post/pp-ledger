#include "ResultOrError.hpp"
#include <gtest/gtest.h>
#include <cmath>
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

// Example using RoeErrorBase
struct AppError : pp::RoeErrorBase {
    using pp::RoeErrorBase::RoeErrorBase;
};

template <typename T>
using AppRoe = pp::ResultOrError<T, AppError>;

AppRoe<double> safeSqrt(double value) {
    if (value < 0) {
        return AppRoe<double>::error(AppError(100, "Cannot compute square root of negative number"));
    }
    return std::sqrt(value);
}

AppRoe<void> validateRange(int value, int min, int max) {
    if (value < min || value > max) {
        return AppRoe<void>::error(AppError(200, "Value out of range"));
    }
    return {};
}

TEST(ResultOrErrorTest, SuccessCase) {
    auto result = divide(10, 2);
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), 5);
}

TEST(ResultOrErrorTest, ErrorCase) {
    auto result = divide(10, 0);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), "Division by zero");
}

TEST(ResultOrErrorTest, BoolConversion) {
    auto result = divide(20, 4);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 5);
}

TEST(ResultOrErrorTest, ValueOr) {
    auto successResult = divide(10, 2);
    EXPECT_EQ(successResult.valueOr(-1), 5);
    
    auto errorResult = divide(10, 0);
    EXPECT_EQ(errorResult.valueOr(-1), -1);
}

TEST(ResultOrErrorTest, VoidReturnSuccess) {
    auto result = validatePositive(10);
    EXPECT_TRUE(result.isOk());
}

TEST(ResultOrErrorTest, VoidReturnError) {
    auto result = validatePositive(-5);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error(), "Value must be positive");
}

TEST(ResultOrErrorTest, CustomErrorTypeSuccess) {
    auto result = processData("Hello, World!");
    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), "Processed: Hello, World!");
}

TEST(ResultOrErrorTest, CustomErrorTypeEmpty) {
    auto result = processData("");
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 1);
    EXPECT_EQ(result.error().message, "Data is empty");
}

TEST(ResultOrErrorTest, CustomErrorTypeTooLong) {
    std::string longData(101, 'x');
    auto result = processData(longData);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 2);
    EXPECT_EQ(result.error().message, "Data too long");
}

TEST(ResultOrErrorTest, RoeErrorBaseSuccess) {
    auto result = safeSqrt(16.0);
    ASSERT_TRUE(result.isOk());
    EXPECT_DOUBLE_EQ(result.value(), 4.0);
}

TEST(ResultOrErrorTest, RoeErrorBaseError) {
    auto result = safeSqrt(-4.0);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 100);
    EXPECT_EQ(result.error().message, "Cannot compute square root of negative number");
}

TEST(ResultOrErrorTest, ValidateRangeSuccess) {
    auto result = validateRange(50, 0, 100);
    EXPECT_TRUE(result.isOk());
}

TEST(ResultOrErrorTest, ValidateRangeError) {
    auto result = validateRange(150, 0, 100);
    ASSERT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 200);
    EXPECT_EQ(result.error().message, "Value out of range");
}
