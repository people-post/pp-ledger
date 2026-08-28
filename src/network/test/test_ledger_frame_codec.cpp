#include "LedgerFrameCodec.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <cstring>

using pp::network::LedgerFrameCodec;

TEST(LedgerFrameCodecTest, EncodeDecodeRoundTrip) {
  const std::string payload = "hello ledger rpc";
  auto framed = LedgerFrameCodec::encode(payload);
  ASSERT_TRUE(framed.isOk());

  ASSERT_GE(framed.value().size(), sizeof(uint32_t));
  auto len = LedgerFrameCodec::decodeLengthPrefix(framed.value().data(), sizeof(uint32_t));
  ASSERT_TRUE(len.isOk());
  EXPECT_EQ(len.value(), payload.size());
  EXPECT_EQ(framed.value().substr(sizeof(uint32_t)), payload);
}

TEST(LedgerFrameCodecTest, RejectsOversizedPayload) {
  std::string huge(LedgerFrameCodec::MAX_PAYLOAD_SIZE + 1, 'x');
  auto framed = LedgerFrameCodec::encode(huge);
  EXPECT_FALSE(framed.isOk());
}

TEST(LedgerFrameCodecTest, RejectsOversizedLengthPrefix) {
  uint32_t netLen = 0;
  const uint32_t bad = LedgerFrameCodec::MAX_PAYLOAD_SIZE + 1;
  netLen = htonl(bad);
  auto len = LedgerFrameCodec::decodeLengthPrefix(&netLen, sizeof(netLen));
  EXPECT_FALSE(len.isOk());
}

TEST(LedgerFrameCodecTest, EmptyPayloadAllowed) {
  auto framed = LedgerFrameCodec::encode("");
  ASSERT_TRUE(framed.isOk());
  EXPECT_EQ(framed.value().size(), sizeof(uint32_t));

  auto len = LedgerFrameCodec::decodeLengthPrefix(framed.value().data(), sizeof(uint32_t));
  ASSERT_TRUE(len.isOk());
  EXPECT_EQ(len.value(), 0u);
}
