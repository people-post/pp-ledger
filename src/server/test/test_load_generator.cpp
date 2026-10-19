#include "client/LoadGenerator.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace pp;
using namespace pp::load;

namespace {

Client::Roe<std::string> serverError(const std::string &message) {
  return Client::Error(Client::E_SERVER_ERROR, message);
}

TEST(LoadGeneratorTest, ParsesAMix) {
  auto mix = parseMix("tx_add=9,tx_junk=1,status");
  ASSERT_TRUE(mix) << mix.error().message;
  ASSERT_EQ(mix.value().size(), 3u);
  EXPECT_EQ(mix.value()[0].first, Kind::TxAdd);
  EXPECT_EQ(mix.value()[0].second, 9u);
  EXPECT_EQ(mix.value()[2].second, 1u) << "no weight: 1";
  EXPECT_FALSE(parseMix("tx_add=9,nothing=1"));
  EXPECT_FALSE(parseMix("tx_add=x"));
  EXPECT_FALSE(parseMix("tx_add=0")) << "nothing left to send";
  EXPECT_EQ(kindFromName(kindName(Kind::TxByWallet)), Kind::TxByWallet);
}

// Every reply lands in one outcome, by what the node (or the transport) said.
TEST(LoadGeneratorTest, ClassifiesReplies) {
  EXPECT_EQ(classify(Kind::TxAdd, Client::Roe<std::string>("Transaction submitted")), Outcome::Taken);
  EXPECT_EQ(classify(Kind::TxAdd, Client::Roe<std::string>("Transaction already submitted")), Outcome::Duplicate);
  EXPECT_EQ(classify(Kind::Status, Client::Roe<std::string>("...")), Outcome::Served);
  EXPECT_EQ(classify(Kind::TxAdd, serverError("Transaction pool full, please retry later")), Outcome::PoolFull);
  EXPECT_EQ(classify(Kind::TxAdd, serverError("Server busy, please retry")), Outcome::Busy);
  EXPECT_EQ(classify(Kind::TxAdd, serverError("Request expired in server queue")), Outcome::Expired);
  EXPECT_EQ(classify(Kind::TxAdd, serverError("Request failed: AmpLedgerTransport: response timeout")),
            Outcome::Timeout)
      << "a relay's forward that timed out";
  EXPECT_EQ(classify(Kind::TxAdd, Client::Roe<std::string>(Client::Error(
                                      Client::E_REQUEST_FAILED, "Request failed: amp link: dial in backoff"))),
            Outcome::Unsent);
  auto transport = [](const char *message) {
    return Client::Roe<std::string>(Client::Error(Client::E_REQUEST_FAILED, message));
  };
  EXPECT_EQ(classify(Kind::Status, transport("Request failed: AmpLedgerTransport: response timeout")), Outcome::Timeout)
      << "sent, and not answered";
  EXPECT_EQ(classify(Kind::Status, transport("Request failed: amp link manager: dial timeout")), Outcome::Unsent)
      << "never sent: no link";
  EXPECT_EQ(classify(Kind::Status, transport("Request failed: AmpLedgerTransport: channel open timeout")),
            Outcome::Unsent);
  EXPECT_EQ(classify(Kind::TxAdd, serverError("Failed to deserialize transaction")), Outcome::Refused);
}

TEST(LoadGeneratorTest, Quantiles) {
  Latencies none;
  EXPECT_EQ(none.quantile(0.99), 0);
  Latencies l;
  for (int i = 1; i <= 100; ++i) {
    l.add(i);
  }
  EXPECT_EQ(l.quantile(0.0), 1);
  EXPECT_EQ(l.quantile(0.5), 51);
  EXPECT_EQ(l.quantile(0.99), 99);
  EXPECT_EQ(l.max(), 100);
}

TEST(LoadGeneratorTest, AccountsRoundTripAndJunkNeverVerifies) {
  auto keys = utl::mlDsaGenerate();
  ASSERT_TRUE(keys);
  const std::vector<Account> accounts = {{(1ULL << 30) + 7, keys->privateKey}};
  const std::string path = "pp-ledger-load-accounts-test.json";
  std::filesystem::remove(path);
  ASSERT_TRUE(writeAccounts(path, "net-1", accounts));
  std::string networkId;
  auto read = readAccounts(path, networkId);
  std::filesystem::remove(path);
  ASSERT_TRUE(read) << read.error().message;
  EXPECT_EQ(networkId, "net-1");
  ASSERT_EQ(read.value().size(), 1u);
  EXPECT_EQ(read.value()[0].id, accounts[0].id);
  EXPECT_EQ(read.value()[0].privateKey, accounts[0].privateKey);

  auto good = transfer(accounts[0], 42, 1, 1, "net-1");
  auto junk = transfer(accounts[0], 42, 1, 1, "net-1", /*junk=*/true);
  ASSERT_TRUE(good && junk);
  EXPECT_TRUE(junk.value().decode()) << "the pool takes what decodes";
  EXPECT_TRUE(utl::mlDsaVerify(keys->publicKey, good.value().signingMessage("net-1"), good.value().signatures[0]));
  EXPECT_FALSE(utl::mlDsaVerify(keys->publicKey, junk.value().signingMessage("net-1"), junk.value().signatures[0]));
  EXPECT_NE(utl::binaryPack(transfer(accounts[0], 42, 1, 1, "net-1").value()), utl::binaryPack(good.value()))
      << "each transfer is its own (idempotent id)";
}

} // namespace
