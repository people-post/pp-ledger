#include "BulkWriter.h"
#include "platform/NetworkPlatform.h"
#include "SocketTestUtils.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

using namespace pp::network;
using pp::network::testutil::ensureNetworkPlatform;
using pp::network::testutil::fdIsOpen;
using pp::network::testutil::makeConnectedSocketPair;

static bool makeSocketPair(int &writer, int &reader) {
    return makeConnectedSocketPair(writer, reader);
}

// On TCP, a tiny write after peer-close often succeeds into the local send
// buffer (Complete path). Use a large payload so the kernel surfaces EPIPE /
// ECONNRESET (or we hit the job timeout) and exercise the error path.
static std::string largePayload() {
    return std::string(256 * 1024, 'x');
}

// Shrink the writer send buffer and abort the peer so send() fails (or the
// job times out) instead of Completing into a large kernel buffer — especially
// important on Windows where closesocket() is graceful by default.
static void prepareWriteErrorScenario(int writer, int &reader) {
    int sndbuf = 1024;
    (void)setsockopt(writer, SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char *>(&sndbuf), sizeof(sndbuf));
#if defined(_WIN32)
    LINGER linger {};
    linger.l_onoff = 1;
    linger.l_linger = 0;
    (void)setsockopt(reader, SOL_SOCKET, SO_LINGER,
                     reinterpret_cast<const char *>(&linger), sizeof(linger));
#endif
    socketClose(reader);
    reader = -1;

    // Ensure subsequent sends fail even if the kernel still accepts a large
    // buffered write after a graceful peer close (common on Windows).
#if defined(_WIN32)
    (void)::shutdown(static_cast<SOCKET>(writer), SD_BOTH);
#else
    (void)::shutdown(writer, SHUT_RDWR);
#endif
}

// ============================================================================
// BulkWriter: fd closed on write error (no callback)
// ============================================================================

TEST(BulkWriterTest, FdClosedOnWriteErrorWithoutCallback) {
    ensureNetworkPlatform();
    int writer = -1, reader = -1;
    ASSERT_TRUE(makeSocketPair(writer, reader));
    prepareWriteErrorScenario(writer, reader);

    BulkWriter bw;
    BulkWriter::Config cfg;
    cfg.timeout.msBase = 200;
    cfg.timeout.msPerMb = 0;
    cfg.errorCallback = nullptr;
    bw.setConfig(cfg);
    bw.start();

    auto result = bw.add(writer, largePayload());
    ASSERT_TRUE(result.isOk()) << (result ? "" : result.error().message);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    bw.stop();

    EXPECT_FALSE(fdIsOpen(writer)) << "fd should have been closed after write error";
}

// ============================================================================
// BulkWriter: fd closed on write error (with callback)
// ============================================================================

TEST(BulkWriterTest, FdClosedOnWriteErrorWithCallback) {
    ensureNetworkPlatform();
    int writer = -1, reader = -1;
    ASSERT_TRUE(makeSocketPair(writer, reader));
    prepareWriteErrorScenario(writer, reader);

    std::atomic<bool> callbackCalled{false};

    BulkWriter bw;
    BulkWriter::Config cfg;
    cfg.timeout.msBase = 200;
    cfg.timeout.msPerMb = 0;
    cfg.errorCallback = [&](int /*fd*/, const BulkWriter::Error & /*e*/) {
        callbackCalled = true;
    };
    bw.setConfig(cfg);
    bw.start();

    auto result = bw.add(writer, largePayload());
    ASSERT_TRUE(result.isOk()) << (result ? "" : result.error().message);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    bw.stop();

    EXPECT_TRUE(callbackCalled) << "error/timeout callback should have been called";
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
    ASSERT_EQ(setsockopt(writer, SOL_SOCKET, SO_SNDBUF,
                         reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf)), 0);

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
    ASSERT_EQ(setsockopt(writer, SOL_SOCKET, SO_SNDBUF,
                         reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf)), 0);

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
