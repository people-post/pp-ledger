#include "BulkWriter.h"
#include "platform/NetworkPlatform.h"
#include "SocketTestUtils.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace pp::network;
using pp::network::testutil::ensureNetworkPlatform;
using pp::network::testutil::fdIsOpen;
using pp::network::testutil::makeConnectedSocketPair;

static bool makeSocketPair(int &writer, int &reader) {
    return makeConnectedSocketPair(writer, reader);
}

// ============================================================================
// BulkWriter: fd closed on write error (no callback)
// ============================================================================

TEST(BulkWriterTest, FdClosedOnWriteErrorWithoutCallback) {
    ensureNetworkPlatform();
    int writer = -1, reader = -1;
    ASSERT_TRUE(makeSocketPair(writer, reader));

    socketClose(reader);
    reader = -1;

    BulkWriter bw;
    BulkWriter::Config cfg;
    cfg.errorCallback = nullptr;
    bw.setConfig(cfg);
    bw.start();

    auto result = bw.add(writer, "hello");
    ASSERT_TRUE(result.isOk()) << (result ? "" : result.error().message);

    // Give the write loop time to detect the broken pipe and close the fd
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bw.stop();

    // After BulkWriter processes the error it must have closed writer
    EXPECT_FALSE(fdIsOpen(writer)) << "fd should have been closed after write error";
}

// ============================================================================
// BulkWriter: fd closed on write error (with callback)
// ============================================================================

TEST(BulkWriterTest, FdClosedOnWriteErrorWithCallback) {
    ensureNetworkPlatform();
    int writer = -1, reader = -1;
    ASSERT_TRUE(makeSocketPair(writer, reader));

    socketClose(reader);
    reader = -1;

    std::atomic<bool> callbackCalled{false};

    BulkWriter bw;
    BulkWriter::Config cfg;
    cfg.errorCallback = [&](int /*fd*/, const BulkWriter::Error & /*e*/) {
        callbackCalled = true;
    };
    bw.setConfig(cfg);
    bw.start();

    auto result = bw.add(writer, "hello");
    ASSERT_TRUE(result.isOk()) << (result ? "" : result.error().message);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bw.stop();

    EXPECT_TRUE(callbackCalled) << "error callback should have been called";
    EXPECT_FALSE(fdIsOpen(writer)) << "fd should have been closed after write error";
}

// ============================================================================
// BulkWriter: fd closed on timeout (no callback)
// ============================================================================

TEST(BulkWriterTest, FdClosedOnTimeoutWithoutCallback) {
    ensureNetworkPlatform();
    int writer = -1, reader = -1;
    ASSERT_TRUE(makeSocketPair(writer, reader));

    // Fill the send buffer so that writes block but don't error.
    // We use a very small timeout so it expires quickly.
    int sndbuf = 1;
    ASSERT_EQ(setsockopt(writer, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)), 0);

    BulkWriter bw;
    BulkWriter::Config cfg;
    cfg.timeout.msBase  = 50;   // 50 ms total timeout
    cfg.timeout.msPerMb = 0;
    cfg.errorCallback   = nullptr;
    bw.setConfig(cfg);
    bw.start();

    // Large payload to keep the socket busy writing
    std::string payload(1024 * 1024, 'x');
    auto result = bw.add(writer, payload);
    ASSERT_TRUE(result.isOk());

    // Wait longer than the timeout so the job expires
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bw.stop();

    // Drain the reader so the test doesn't hang
    socketClose(reader);
    reader = -1;

    EXPECT_FALSE(fdIsOpen(writer)) << "fd should have been closed after timeout";
}

// ============================================================================
// BulkWriter: fd closed on timeout (with callback)
// ============================================================================

TEST(BulkWriterTest, FdClosedOnTimeoutWithCallback) {
    ensureNetworkPlatform();
    int writer = -1, reader = -1;
    ASSERT_TRUE(makeSocketPair(writer, reader));

    int sndbuf = 1;
    ASSERT_EQ(setsockopt(writer, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)), 0);

    std::atomic<bool> callbackCalled{false};

    BulkWriter bw;
    BulkWriter::Config cfg;
    cfg.timeout.msBase  = 50;
    cfg.timeout.msPerMb = 0;
    cfg.errorCallback   = [&](int /*fd*/, const BulkWriter::Error & /*e*/) {
        callbackCalled = true;
    };
    bw.setConfig(cfg);
    bw.start();

    std::string payload(1024 * 1024, 'x');
    auto result = bw.add(writer, payload);
    ASSERT_TRUE(result.isOk());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bw.stop();

    socketClose(reader);
    reader = -1;

    EXPECT_TRUE(callbackCalled) << "error callback should have been called on timeout";
    EXPECT_FALSE(fdIsOpen(writer)) << "fd should have been closed after timeout";
}

// ============================================================================
// BulkWriter: fd closed after successful write (sanity check)
// ============================================================================

TEST(BulkWriterTest, FdClosedOnSuccessfulWrite) {
    ensureNetworkPlatform();
    int writer = -1, reader = -1;
    ASSERT_TRUE(makeSocketPair(writer, reader));

    BulkWriter bw;
    bw.start();

    auto result = bw.add(writer, "hello");
    ASSERT_TRUE(result.isOk()) << (result ? "" : result.error().message);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    bw.stop();

    socketClose(reader);
    reader = -1;

    EXPECT_FALSE(fdIsOpen(writer)) << "fd should have been closed after successful write";
}
