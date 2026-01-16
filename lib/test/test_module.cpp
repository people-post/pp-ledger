#include "Module.h"
#include "Server.h"
#include "Client.h"
#include <gtest/gtest.h>

// Test Module base class functionality
class TestModule : public pp::Module {
public:
    TestModule(const std::string& name) : pp::Module(name) {}
};

TEST(ModuleTest, LogReturnsLoggerReference) {
    TestModule module("test_module");
    
    // Test that log() returns a reference and works
    EXPECT_NO_THROW({
        module.log().info << "Test message";
        module.log().debug << "Debug message";
        module.log().warning << "Warning message";
    });
    
    // Verify logger name
    EXPECT_EQ(module.log().getName(), "test_module");
}

TEST(ModuleTest, LogIsConst) {
    const TestModule module("const_test");
    
    // Test that log() can be called on const objects
    EXPECT_NO_THROW({
        module.log().info << "Const test message";
    });
    
    EXPECT_EQ(module.log().getName(), "const_test");
}

TEST(ModuleTest, LoggerRedirect) {
    TestModule module("redirect_test");
    
    // Test initial state
    EXPECT_FALSE(module.log().hasRedirect());
    
    // Test redirect
    module.redirectLogger("target");
    EXPECT_TRUE(module.log().hasRedirect());
    EXPECT_EQ(module.log().getRedirectTarget(), "target");
    
    EXPECT_NO_THROW(module.log().info << "Message via redirect");
    
    // Test clear redirect
    module.clearLoggerRedirect();
    EXPECT_FALSE(module.log().hasRedirect());
    
    EXPECT_NO_THROW(module.log().info << "Message after clearing redirect");
}

TEST(ModuleTest, LoggerWorksForDerivedClasses) {
    pp::Server server;
    pp::Client client;
    
    // Test that derived classes can use logging
    EXPECT_NO_THROW({
        server.log().info << "Server message";
        client.log().info << "Client message";
    });
    
    // Verify logger names
    EXPECT_EQ(server.log().getName(), "server");
    EXPECT_EQ(client.log().getName(), "client");
}
