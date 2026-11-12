#include "ledger/consensus/EpochSeed.h"
#include "ledger/lib/common/Utilities.h"
#include <gtest/gtest.h>
#include <optional>
#include <string>

using namespace pp::consensus;

TEST(BeaconSeedChain, RevealForStepKHashesToTheAnchorInKSteps) {
  const std::string s(32, '\x01');
  const uint64_t n = 5;
  const std::string anchor = beaconSeedValue(s, n);
  for (uint64_t k = 1; k <= n; ++k) {
    const std::string reveal = beaconSeedValue(s, n - k);
    EXPECT_TRUE(verifyBeaconSeedReveal(anchor, k, reveal)) << "k=" << k;
    EXPECT_FALSE(verifyBeaconSeedReveal(anchor, k + 1, reveal)) << "k=" << k;
  }
}

TEST(BeaconSeedChain, ZeroStepsReturnsSecretAndStepUsesDomain) {
  const std::string s(32, '\x01');
  EXPECT_EQ(beaconSeedValue(s, 0), s);
  EXPECT_EQ(beaconSeedValue(s, 1), beaconSeedStep(s));
  EXPECT_EQ(beaconSeedValue(s, 2), beaconSeedStep(beaconSeedStep(s)));
  EXPECT_EQ(beaconSeedStep(s).size(), 32u);
}

TEST(BeaconSeedChain, RefusesShortRevealsAndZeroSteps) {
  const std::string s(32, '\x01');
  const std::string anchor = beaconSeedValue(s, 3);
  EXPECT_TRUE(verifyBeaconSeedReveal(anchor, 1, beaconSeedValue(s, 2)));
  EXPECT_FALSE(verifyBeaconSeedReveal(anchor, 1, beaconSeedValue(s, 2).substr(0, 31)));
  EXPECT_FALSE(verifyBeaconSeedReveal(anchor, 1, beaconSeedValue(s, 2) + "x"));
  EXPECT_FALSE(verifyBeaconSeedReveal(anchor, 1, ""));
  // steps == 0 would accept reveal == anchor, which is already public.
  EXPECT_FALSE(verifyBeaconSeedReveal(anchor, 0, anchor));
}

TEST(EpochSeedV2, DependsOnTheRevealAndItsAbsence) {
  const std::string prev(32, '\x07');
  const std::string r1(32, '\x11');
  const std::string r2(32, '\x22');
  const std::string a = deriveEpochSeedV2(5, "net", prev, r1);
  const std::string b = deriveEpochSeedV2(5, "net", prev, r2);
  const std::string c = deriveEpochSeedV2(5, "net", prev, std::nullopt);
  EXPECT_EQ(a.size(), 32u);
  EXPECT_EQ(b.size(), 32u);
  EXPECT_EQ(c.size(), 32u);
  EXPECT_NE(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(b, c);
}

TEST(EpochSeedV2, DependsOnEpochNetworkAndPrevSeed) {
  const std::string prev(32, '\x07');
  const auto base = deriveEpochSeedV2(5, "net", prev, std::nullopt);
  EXPECT_NE(base, deriveEpochSeedV2(6, "net", prev, std::nullopt));
  EXPECT_NE(base, deriveEpochSeedV2(5, "net2", prev, std::nullopt));
  EXPECT_NE(base, deriveEpochSeedV2(5, "net", std::string(32, '\x08'), std::nullopt));
}

TEST(EpochSeedV2, IsDeterministic) {
  const std::string prev(32, '\x07');
  const std::string r(32, '\x11');
  EXPECT_EQ(deriveEpochSeedV2(5, "net", prev, r), deriveEpochSeedV2(5, "net", prev, r));
  EXPECT_EQ(deriveEpochSeedV2(5, "net", prev, std::nullopt), deriveEpochSeedV2(5, "net", prev, std::nullopt));
}

// Known answers, computed once from this code and checked against an independent implementation
// (SHA-256 over the domain, big-endian u64 fields (OutputArchive) and a presence byte). Any change to a domain
// string or the preimage layout forks every network: it must fail here first.
TEST(EpochSeedKnownAnswers, BeaconSeedStepAndValue) {
  const std::string s(32, '\x01');
  EXPECT_EQ(pp::utl::hexEncode(beaconSeedStep(s)), "d79cda60036be7c3d9e5559a093005acf56d5a6e5dbb992e0439dc9429883a23");
  EXPECT_EQ(pp::utl::hexEncode(beaconSeedValue(s, 3)),
            "d05393c3797843af41e89ef2f224bfe576c2fe2fb195a3b0d5f81840a9d70ed1");
}

TEST(EpochSeedKnownAnswers, DeriveEpochSeedV2WithAndWithoutAReveal) {
  const std::string prev(32, '\x07');
  const std::string reveal(32, '\x11');
  EXPECT_EQ(pp::utl::hexEncode(deriveEpochSeedV2(5, "net", prev, reveal)),
            "1b2436190131f41e88749992e264afb3393cee282277cc06d641dcaf6f3ac5d8");
  EXPECT_EQ(pp::utl::hexEncode(deriveEpochSeedV2(5, "net", prev, std::nullopt)),
            "16bd388c421f741e45a0084386c93b9486bcd780a9486e72003aa7e77d5c8e7a");
}
