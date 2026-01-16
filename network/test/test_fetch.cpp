#include "FetchClient.h"
#include "FetchServer.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <chrono>

using namespace pp::network;

class FetchClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        client = std::make_unique<FetchClient>();
    }

    void TearDown() override {
        client.reset();
    }

    std::unique_ptr<FetchClient> client;
};

TEST_F(FetchClientTest, CreatesSuccessfully) {
    EXPECT_NE(client, nullptr);
}

TEST_F(FetchClientTest, FetchSyncFailsWithInvalidHost) {
    auto result = client->fetchSync("invalid-host-that-does-not-exist.local", 9999, "Hello");
    EXPECT_FALSE(result.isOk());
}

class FetchServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        server = std::make_unique<FetchServer>();
    }

    void TearDown() override {
        if (server && server->isRunning()) {
            server->stop();
        }
        server.reset();
    }

    std::unique_ptr<FetchServer> server;
};

TEST_F(FetchServerTest, CreatesSuccessfully) {
    EXPECT_NE(server, nullptr);
    EXPECT_FALSE(server->isRunning());
}

TEST_F(FetchServerTest, StartsAndStops) {
    FetchServer::Config config;
    config.host = "127.0.0.1";
    config.port = 18880;
    config.handler = [](const std::string& req) {
        return "Echo: " + req;
    };
    bool started = server->start(config);
    
    EXPECT_TRUE(started);
    EXPECT_TRUE(server->isRunning());
    EXPECT_EQ(server->getPort(), 18880);
    
    server->stop();
    EXPECT_FALSE(server->isRunning());
}

TEST_F(FetchServerTest, FailsToStartOnSamePortTwice) {
    FetchServer::Config config;
    config.host = "127.0.0.1";
    config.port = 18881;
    config.handler = [](const std::string& req) {
        return "Echo: " + req;
    };
    bool started1 = server->start(config);
    EXPECT_TRUE(started1);
    
    // Create second server
    FetchServer server2;
    FetchServer::Config config2;
    config2.host = "127.0.0.1";
    config2.port = 18881;
    config2.handler = [](const std::string& req) {
        return "Echo: " + req;
    };
    bool started2 = server2.start(config2);
    EXPECT_FALSE(started2);
    
    server->stop();
    server2.stop();
}

// Integration test
class FetchIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        server = std::make_unique<FetchServer>();
        client = std::make_unique<FetchClient>();
    }

    void TearDown() override {
        if (server && server->isRunning()) {
            server->stop();
        }
        server.reset();
        client.reset();
    }

    std::unique_ptr<FetchServer> server;
    std::unique_ptr<FetchClient> client;
};

TEST_F(FetchIntegrationTest, ClientServerCommunication) {
    // Start server
    FetchServer::Config config;
    config.host = "127.0.0.1";
    config.port = 18882;
    config.handler = [](const std::string& req) {
        return "Echo: " + req;
    };
    bool started = server->start(config);
    ASSERT_TRUE(started);
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Fetch from server
    auto result = client->fetchSync("127.0.0.1", 18882, "Hello World");
    
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), "Echo: Hello World");
    
    server->stop();
}

TEST_F(FetchIntegrationTest, MultipleRequests) {
    // Start server
    FetchServer::Config config;
    config.host = "127.0.0.1";
    config.port = 18883;
    config.handler = [](const std::string& req) {
        return "Response: " + req;
    };
    bool started = server->start(config);
    ASSERT_TRUE(started);
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send multiple requests
    for (int i = 0; i < 5; i++) {
        auto result = client->fetchSync("127.0.0.1", 18883, "Request " + std::to_string(i));
        EXPECT_TRUE(result.isOk());
        EXPECT_EQ(result.value(), "Response: Request " + std::to_string(i));
    }
    
    server->stop();
}

TEST_F(FetchIntegrationTest, AsyncFetch) {
    // Start server
    FetchServer::Config config;
    config.host = "127.0.0.1";
    config.port = 18884;
    config.handler = [](const std::string& req) {
        return "Async: " + req;
    };
    bool started = server->start(config);
    ASSERT_TRUE(started);
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Async fetch with callback
    bool callbackCalled = false;
    std::string receivedResponse;
    
    client->fetch("127.0.0.1", 18884, "Hello Async",
        [&callbackCalled, &receivedResponse](const auto& result) {
            callbackCalled = true;
            if (result.isOk()) {
                receivedResponse = result.value();
            }
        });
    
    // Wait for callback
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(receivedResponse, "Async: Hello Async");
    
    server->stop();
}
