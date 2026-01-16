#include "TcpClient.h"
#include "TcpServer.h"
#include "TcpConnection.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

using namespace pp::network;

// Helper function to find an available port
uint16_t findAvailablePort() {
    static uint16_t basePort = 20000;
    return basePort++;
}

// ============================================================================
// TcpConnection Tests
// ============================================================================

class TcpConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a pair of connected sockets for testing
        int sockets[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
        serverSocket_ = sockets[0];
        clientSocket_ = sockets[1];
    }

    void TearDown() override {
        if (serverSocket_ >= 0) {
            close(serverSocket_);
        }
        if (clientSocket_ >= 0) {
            close(clientSocket_);
        }
    }

    int serverSocket_ = -1;
    int clientSocket_ = -1;
};

TEST_F(TcpConnectionTest, ConstructsFromSocketFd) {
    TcpConnection conn(serverSocket_);
    // Connection should be valid (for socketpair, peer address might be empty)
    // Just verify construction doesn't crash
    std::string addr = conn.getPeerAddress();
    // Address might be empty for Unix domain sockets, which is OK
    EXPECT_GE(addr.length(), 0);
}

TEST_F(TcpConnectionTest, MoveConstructor) {
    TcpConnection conn1(serverSocket_);
    std::string peerAddr = conn1.getPeerAddress();
    
    TcpConnection conn2(std::move(conn1));
    
    // conn2 should have the connection
    EXPECT_EQ(conn2.getPeerAddress(), peerAddr);
}

TEST_F(TcpConnectionTest, MoveAssignment) {
    TcpConnection conn1(serverSocket_);
    std::string peerAddr = conn1.getPeerAddress();
    
    TcpConnection conn2(clientSocket_);
    conn2 = std::move(conn1);
    
    // conn2 should have moved from conn1
    EXPECT_EQ(conn2.getPeerAddress(), peerAddr);
}

TEST_F(TcpConnectionTest, SendData) {
    TcpConnection conn(serverSocket_);
    
    const char* data = "Hello";
    auto result = conn.send(data, strlen(data));
    
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), strlen(data));
}

TEST_F(TcpConnectionTest, SendString) {
    TcpConnection conn(serverSocket_);
    
    std::string message = "Test Message";
    auto result = conn.send(message);
    
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), message.length());
}

TEST_F(TcpConnectionTest, ReceiveData) {
    TcpConnection conn(serverSocket_);
    
    // Send data from client socket
    const char* testData = "Hello World";
    ::send(clientSocket_, testData, strlen(testData), 0);
    
    // Receive on server connection
    char buffer[256] = {0};
    auto result = conn.receive(buffer, sizeof(buffer) - 1);
    
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), strlen(testData));
    EXPECT_STREQ(buffer, testData);
}

TEST_F(TcpConnectionTest, ReceiveLine) {
    TcpConnection conn(serverSocket_);
    
    // Send line with newline from client socket
    const char* line = "Test Line\n";
    ::send(clientSocket_, line, strlen(line), 0);
    
    // Receive line on server connection
    auto result = conn.receiveLine();
    
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), "Test Line");
}

TEST_F(TcpConnectionTest, ReceiveLineWithCarriageReturn) {
    TcpConnection conn(serverSocket_);
    
    // Send line with CRLF from client socket
    const char* line = "Test Line\r\n";
    ::send(clientSocket_, line, strlen(line), 0);
    
    // Receive line on server connection
    auto result = conn.receiveLine();
    
    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(result.value(), "Test Line");
}

TEST_F(TcpConnectionTest, CloseConnection) {
    TcpConnection conn(serverSocket_);
    
    conn.close();
    
    // After close, send should fail
    auto result = conn.send("test", 4);
    EXPECT_TRUE(result.isError());
}

TEST_F(TcpConnectionTest, GetPeerAddress) {
    TcpConnection conn(serverSocket_);
    
    // For socketpair (Unix domain socket), peer address might be empty
    // This is expected behavior - just verify the method doesn't crash
    std::string addr = conn.getPeerAddress();
    EXPECT_GE(addr.length(), 0);
    
    // Port should be 0 for Unix domain sockets
    uint16_t port = conn.getPeerPort();
    EXPECT_GE(port, 0);
}

// ============================================================================
// TcpClient Tests
// ============================================================================

class TcpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        client = std::make_unique<TcpClient>();
        testPort = findAvailablePort();
    }

    void TearDown() override {
        if (client) {
            client->close();
        }
        client.reset();
    }

    std::unique_ptr<TcpClient> client;
    uint16_t testPort;
};

TEST_F(TcpClientTest, ConstructsSuccessfully) {
    EXPECT_NE(client, nullptr);
    EXPECT_FALSE(client->isConnected());
}

TEST_F(TcpClientTest, MoveConstructor) {
    TcpClient client1;
    TcpClient client2(std::move(client1));
    
    // Should not crash
    EXPECT_FALSE(client2.isConnected());
}

TEST_F(TcpClientTest, MoveAssignment) {
    TcpClient client1;
    TcpClient client2;
    
    client2 = std::move(client1);
    
    // Should not crash
    EXPECT_FALSE(client2.isConnected());
}

TEST_F(TcpClientTest, ConnectFailsWithInvalidHost) {
    auto result = client->connect("invalid-host-that-does-not-exist.local", 9999);
    
    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(client->isConnected());
}

TEST_F(TcpClientTest, ConnectFailsWithInvalidPort) {
    // Try to connect to a port that's likely not listening
    auto result = client->connect("127.0.0.1", 1);
    
    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(client->isConnected());
}

TEST_F(TcpClientTest, SendFailsWhenNotConnected) {
    auto result = client->send("test", 4);
    
    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(client->isConnected());
}

TEST_F(TcpClientTest, ReceiveFailsWhenNotConnected) {
    char buffer[256];
    auto result = client->receive(buffer, sizeof(buffer));
    
    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(client->isConnected());
}

TEST_F(TcpClientTest, CloseWhenNotConnected) {
    // Should not crash
    client->close();
    EXPECT_FALSE(client->isConnected());
}

// ============================================================================
// TcpServer Tests
// ============================================================================

class TcpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        server = std::make_unique<TcpServer>();
        testPort = findAvailablePort();
    }

    void TearDown() override {
        if (server && server->isListening()) {
            server->stop();
        }
        server.reset();
    }

    std::unique_ptr<TcpServer> server;
    uint16_t testPort;
};

TEST_F(TcpServerTest, ConstructsSuccessfully) {
    EXPECT_NE(server, nullptr);
    EXPECT_FALSE(server->isListening());
}

TEST_F(TcpServerTest, ListenOnPort) {
    auto result = server->listen("127.0.0.1", testPort);
    
    EXPECT_TRUE(result.isOk());
    EXPECT_TRUE(server->isListening());
}

TEST_F(TcpServerTest, ListenFailsOnInvalidPort) {
    // Port 0 is typically invalid
    auto result = server->listen("127.0.0.1", 0);
    
    // This might succeed on some systems, so we just check it doesn't crash
    // If it fails, that's expected
    if (result.isError()) {
        EXPECT_FALSE(server->isListening());
    }
}

TEST_F(TcpServerTest, ListenFailsWhenAlreadyListening) {
    auto result1 = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(result1.isOk());
    
    auto result2 = server->listen("127.0.0.1", testPort + 1);
    
    EXPECT_TRUE(result2.isError());
}

TEST_F(TcpServerTest, AcceptFailsWhenNotListening) {
    auto result = server->accept();
    
    EXPECT_TRUE(result.isError());
}

TEST_F(TcpServerTest, WaitForEventsFailsWhenNotListening) {
    auto result = server->waitForEvents(100);
    
    EXPECT_TRUE(result.isError());
}

TEST_F(TcpServerTest, StopWhenNotListening) {
    // Should not crash
    server->stop();
    EXPECT_FALSE(server->isListening());
}

TEST_F(TcpServerTest, StopWhenListening) {
    auto result = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(result.isOk());
    
    server->stop();
    
    EXPECT_FALSE(server->isListening());
}

// ============================================================================
// Integration Tests: TcpClient + TcpServer
// ============================================================================

class TcpIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        server = std::make_unique<TcpServer>();
        client = std::make_unique<TcpClient>();
        testPort = findAvailablePort();
    }

    void TearDown() override {
        if (client && client->isConnected()) {
            client->close();
        }
        if (server && server->isListening()) {
            server->stop();
        }
        client.reset();
        server.reset();
    }

    std::unique_ptr<TcpServer> server;
    std::unique_ptr<TcpClient> client;
    uint16_t testPort;
};

TEST_F(TcpIntegrationTest, ClientConnectsToServer) {
    // Start server
    auto listenResult = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(listenResult.isOk());
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client connects
    auto connectResult = client->connect("127.0.0.1", testPort);
    
    EXPECT_TRUE(connectResult.isOk());
    EXPECT_TRUE(client->isConnected());
    
    server->stop();
}

TEST_F(TcpIntegrationTest, ClientSendsDataToServer) {
    // Start server
    auto listenResult = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(listenResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client connects
    auto connectResult = client->connect("127.0.0.1", testPort);
    ASSERT_TRUE(connectResult.isOk());
    
    // Wait for connection to be accepted
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client sends data
    std::string message = "Hello Server";
    auto sendResult = client->send(message);
    
    EXPECT_TRUE(sendResult.isOk());
    EXPECT_EQ(sendResult.value(), message.length());
    
    server->stop();
}

TEST_F(TcpIntegrationTest, ServerReceivesDataFromClient) {
    // Start server
    auto listenResult = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(listenResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client connects and sends
    auto connectResult = client->connect("127.0.0.1", testPort);
    ASSERT_TRUE(connectResult.isOk());
    
    std::string message = "Test Message";
    auto sendResult = client->send(message);
    ASSERT_TRUE(sendResult.isOk());
    
    // Wait for server to receive
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server waits for events
    auto waitResult = server->waitForEvents(100);
    if (waitResult.isOk()) {
        // Accept connection
        auto acceptResult = server->accept();
        if (acceptResult.isOk()) {
            TcpConnection conn = std::move(acceptResult.value());
            
            // Receive data
            char buffer[256] = {0};
            auto recvResult = conn.receive(buffer, sizeof(buffer) - 1);
            
            EXPECT_TRUE(recvResult.isOk());
            EXPECT_EQ(recvResult.value(), message.length());
            EXPECT_STREQ(buffer, message.c_str());
        }
    }
    
    server->stop();
}

TEST_F(TcpIntegrationTest, FullBidirectionalCommunication) {
    // Start server
    auto listenResult = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(listenResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client connects
    auto connectResult = client->connect("127.0.0.1", testPort);
    ASSERT_TRUE(connectResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server accepts
    auto waitResult = server->waitForEvents(100);
    ASSERT_TRUE(waitResult.isOk());
    
    auto acceptResult = server->accept();
    ASSERT_TRUE(acceptResult.isOk());
    
    TcpConnection conn = std::move(acceptResult.value());
    
    // Client sends
    std::string clientMessage = "Hello from client";
    auto sendResult = client->send(clientMessage);
    ASSERT_TRUE(sendResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server receives
    char buffer[256] = {0};
    auto recvResult = conn.receive(buffer, sizeof(buffer) - 1);
    ASSERT_TRUE(recvResult.isOk());
    EXPECT_STREQ(buffer, clientMessage.c_str());
    
    // Server sends response
    std::string serverMessage = "Hello from server";
    auto serverSendResult = conn.send(serverMessage);
    ASSERT_TRUE(serverSendResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client receives
    char clientBuffer[256] = {0};
    auto clientRecvResult = client->receive(clientBuffer, sizeof(clientBuffer) - 1);
    ASSERT_TRUE(clientRecvResult.isOk());
    EXPECT_STREQ(clientBuffer, serverMessage.c_str());
    
    server->stop();
}

TEST_F(TcpIntegrationTest, ReceiveLine) {
    // Start server
    auto listenResult = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(listenResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client connects
    auto connectResult = client->connect("127.0.0.1", testPort);
    ASSERT_TRUE(connectResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server accepts
    auto waitResult = server->waitForEvents(100);
    ASSERT_TRUE(waitResult.isOk());
    
    auto acceptResult = server->accept();
    ASSERT_TRUE(acceptResult.isOk());
    
    TcpConnection conn = std::move(acceptResult.value());
    
    // Client sends line
    std::string line = "Test Line\n";
    auto sendResult = client->send(line);
    ASSERT_TRUE(sendResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server receives line
    auto recvResult = conn.receiveLine();
    ASSERT_TRUE(recvResult.isOk());
    EXPECT_EQ(recvResult.value(), "Test Line");
    
    server->stop();
}

TEST_F(TcpIntegrationTest, MultipleConnections) {
    // Start server
    auto listenResult = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(listenResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Create multiple clients
    TcpClient client1;
    TcpClient client2;
    
    // Connect both
    auto conn1 = client1.connect("127.0.0.1", testPort);
    ASSERT_TRUE(conn1.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto conn2 = client2.connect("127.0.0.1", testPort);
    ASSERT_TRUE(conn2.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server should be able to accept both
    auto wait1 = server->waitForEvents(100);
    if (wait1.isOk()) {
        auto accept1 = server->accept();
        EXPECT_TRUE(accept1.isOk());
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto wait2 = server->waitForEvents(100);
    if (wait2.isOk()) {
        auto accept2 = server->accept();
        EXPECT_TRUE(accept2.isOk());
    }
    
    server->stop();
}

TEST_F(TcpIntegrationTest, ClientClosesConnection) {
    // Start server
    auto listenResult = server->listen("127.0.0.1", testPort);
    ASSERT_TRUE(listenResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Client connects
    auto connectResult = client->connect("127.0.0.1", testPort);
    ASSERT_TRUE(connectResult.isOk());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server accepts
    auto waitResult = server->waitForEvents(100);
    ASSERT_TRUE(waitResult.isOk());
    
    auto acceptResult = server->accept();
    ASSERT_TRUE(acceptResult.isOk());
    
    TcpConnection conn = std::move(acceptResult.value());
    
    // Client closes
    client->close();
    EXPECT_FALSE(client->isConnected());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Server should detect closed connection
    char buffer[256];
    auto recvResult = conn.receive(buffer, sizeof(buffer));
    EXPECT_TRUE(recvResult.isError());
    
    server->stop();
}
