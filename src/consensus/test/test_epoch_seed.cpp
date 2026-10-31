#include "ledger/consensus/EpochSeed.h"
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
  EXPECT_EQ(deriveEpochSeedV2(5, "net", prev, std::nullopt),
            deriveEpochSeedV2(5, "net", prev, std::nullopt));
}
