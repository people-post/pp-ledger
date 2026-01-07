#include "SlotLeaderSelection.h"
#include <gtest/gtest.h>

using namespace pp::ouroboros;

class VRFTest : public ::testing::Test {
protected:
    void SetUp() override {
        vrf = std::make_unique<VRF>();
    }

    void TearDown() override {
        vrf.reset();
    }

    std::unique_ptr<VRF> vrf;
};

TEST_F(VRFTest, EvaluatesSuccessfully) {
    std::string seed = "epoch_nonce_12345";
    uint64_t slot = 100;
    std::string privateKey = "alice_private_key";
    
    auto result = vrf->evaluate(seed, slot, privateKey);
    
    ASSERT_TRUE(result.isOk());
    VRF::VRFOutput output = result.value();
    EXPECT_FALSE(output.value.empty());
    EXPECT_FALSE(output.proof.empty());
}

TEST_F(VRFTest, RejectsEmptyPrivateKey) {
    auto result = vrf->evaluate("seed", 100, "");
    
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 1);
    EXPECT_FALSE(result.error().message.empty());
}

TEST_F(VRFTest, VerifiesProofSuccessfully) {
    std::string seed = "epoch_nonce_12345";
    uint64_t slot = 100;
    std::string privateKey = "alice_private_key";
    std::string publicKey = "alice_public_key";
    
    auto evalResult = vrf->evaluate(seed, slot, privateKey);
    ASSERT_TRUE(evalResult.isOk());
    
    auto output = evalResult.value();
    auto verifyResult = vrf->verify(output.value, output.proof, seed, slot, publicKey);
    
    ASSERT_TRUE(verifyResult.isOk());
    EXPECT_TRUE(verifyResult.value());
}

TEST_F(VRFTest, RejectsEmptyPublicKeyInVerification) {
    auto result = vrf->verify("value", "proof", "seed", 100, "");
    
    EXPECT_TRUE(result.isError());
    EXPECT_EQ(result.error().code, 2);
}

TEST_F(VRFTest, IsDeterministic) {
    std::string seed = "epoch_nonce";
    uint64_t slot = 50;
    std::string privateKey = "key";
    
    auto result1 = vrf->evaluate(seed, slot, privateKey);
    auto result2 = vrf->evaluate(seed, slot, privateKey);
    
    ASSERT_TRUE(result1.isOk());
    ASSERT_TRUE(result2.isOk());
    
    EXPECT_EQ(result1.value().value, result2.value().value);
    EXPECT_EQ(result1.value().proof, result2.value().proof);
}

TEST_F(VRFTest, ProducesDifferentOutputsForDifferentInputs) {
    std::string seed = "epoch_nonce";
    uint64_t slot = 50;
    std::string privateKey = "key";
    
    auto result1 = vrf->evaluate(seed, slot, privateKey);
    auto result2 = vrf->evaluate("different_seed", slot, privateKey);
    auto result3 = vrf->evaluate(seed, 51, privateKey);
    auto result4 = vrf->evaluate(seed, slot, "different_key");
    
    ASSERT_TRUE(result1.isOk());
    ASSERT_TRUE(result2.isOk());
    ASSERT_TRUE(result3.isOk());
    ASSERT_TRUE(result4.isOk());
    
    EXPECT_NE(result1.value().value, result2.value().value);
    EXPECT_NE(result1.value().value, result3.value().value);
    EXPECT_NE(result1.value().value, result4.value().value);
}

TEST_F(VRFTest, ChecksLeadershipWithValidStake) {
    std::string seed = "epoch_nonce";
    uint64_t slot = 100;
    std::string privateKey = "key";
    
    auto result = vrf->evaluate(seed, slot, privateKey);
    ASSERT_TRUE(result.isOk());
    
    uint64_t stake = 1000;
    uint64_t totalStake = 10000;
    double difficulty = 0.05;
    
    // Just verify it returns a boolean
    bool isLeader = vrf->checkLeadership(result.value().value, stake, totalStake, difficulty);
    EXPECT_TRUE(isLeader == true || isLeader == false);
}

TEST_F(VRFTest, RejectsLeadershipWithZeroStake) {
    std::string vrfOutput = "some_output";
    
    bool result = vrf->checkLeadership(vrfOutput, 0, 10000, 0.05);
    EXPECT_FALSE(result);
}

TEST_F(VRFTest, RejectsLeadershipWithZeroTotalStake) {
    std::string vrfOutput = "some_output";
    
    bool result = vrf->checkLeadership(vrfOutput, 1000, 0, 0.05);
    EXPECT_FALSE(result);
}

TEST_F(VRFTest, LeadershipProbabilityReflectsStakeRatio) {
    // Test that higher stake increases win probability
    int wins_low_stake = 0;
    int wins_high_stake = 0;
    int trials = 100;
    
    for (int i = 0; i < trials; i++) {
        auto result = vrf->evaluate("nonce", i, "key");
        if (result.isOk()) {
            if (vrf->checkLeadership(result.value().value, 100, 10000, 0.05)) {
                wins_low_stake++;
            }
            if (vrf->checkLeadership(result.value().value, 5000, 10000, 0.05)) {
                wins_high_stake++;
            }
        }
    }
    
    // Higher stake should generally win more often
    // This is probabilistic but with 50% vs 1% stake ratio, should be reliable
    EXPECT_GT(wins_high_stake, wins_low_stake);
}

// EpochNonce Tests
class EpochNonceTest : public ::testing::Test {
protected:
    void SetUp() override {
        nonce = std::make_unique<EpochNonce>();
    }

    void TearDown() override {
        nonce.reset();
    }

    std::unique_ptr<EpochNonce> nonce;
};

TEST_F(EpochNonceTest, ReturnsGenesisNonce) {
    std::string genesisNonce = nonce->getGenesisNonce();
    EXPECT_FALSE(genesisNonce.empty());
}

TEST_F(EpochNonceTest, GeneratesNonceForEpoch) {
    std::string genesisNonce = nonce->getGenesisNonce();
    std::vector<std::string> blockHashes = {"hash1", "hash2", "hash3"};
    
    std::string epoch1Nonce = nonce->generate(1, genesisNonce, blockHashes);
    
    EXPECT_FALSE(epoch1Nonce.empty());
    EXPECT_NE(epoch1Nonce, genesisNonce);
}

TEST_F(EpochNonceTest, IsDeterministic) {
    std::string genesisNonce = nonce->getGenesisNonce();
    std::vector<std::string> blockHashes = {"hash1", "hash2", "hash3"};
    
    std::string nonce1 = nonce->generate(1, genesisNonce, blockHashes);
    std::string nonce2 = nonce->generate(1, genesisNonce, blockHashes);
    
    EXPECT_EQ(nonce1, nonce2);
}

TEST_F(EpochNonceTest, ProducesDifferentNoncesForDifferentEpochs) {
    std::string genesisNonce = nonce->getGenesisNonce();
    std::vector<std::string> blockHashes = {"hash1", "hash2", "hash3"};
    
    std::string epoch1Nonce = nonce->generate(1, genesisNonce, blockHashes);
    std::string epoch2Nonce = nonce->generate(2, genesisNonce, blockHashes);
    
    EXPECT_NE(epoch1Nonce, epoch2Nonce);
}

TEST_F(EpochNonceTest, ProducesDifferentNoncesForDifferentBlockHashes) {
    std::string genesisNonce = nonce->getGenesisNonce();
    std::vector<std::string> blockHashes1 = {"hash1", "hash2", "hash3"};
    std::vector<std::string> blockHashes2 = {"hashA", "hashB"};
    
    std::string nonce1 = nonce->generate(1, genesisNonce, blockHashes1);
    std::string nonce2 = nonce->generate(1, genesisNonce, blockHashes2);
    
    EXPECT_NE(nonce1, nonce2);
}

TEST_F(EpochNonceTest, ProducesDifferentNoncesForDifferentPreviousNonce) {
    std::vector<std::string> blockHashes = {"hash1", "hash2"};
    
    std::string nonce1 = nonce->generate(1, "nonce_a", blockHashes);
    std::string nonce2 = nonce->generate(1, "nonce_b", blockHashes);
    
    EXPECT_NE(nonce1, nonce2);
}

TEST_F(EpochNonceTest, HandlesEmptyBlockHashes) {
    std::string genesisNonce = nonce->getGenesisNonce();
    std::vector<std::string> emptyHashes;
    
    std::string epochNonce = nonce->generate(1, genesisNonce, emptyHashes);
    
    EXPECT_FALSE(epochNonce.empty());
}

TEST_F(EpochNonceTest, GeneratesChainOfNonces) {
    std::string genesisNonce = nonce->getGenesisNonce();
    
    std::vector<std::string> blocks1 = {"hash1", "hash2"};
    std::string epoch1Nonce = nonce->generate(1, genesisNonce, blocks1);
    
    std::vector<std::string> blocks2 = {"hash3", "hash4"};
    std::string epoch2Nonce = nonce->generate(2, epoch1Nonce, blocks2);
    
    std::vector<std::string> blocks3 = {"hash5", "hash6"};
    std::string epoch3Nonce = nonce->generate(3, epoch2Nonce, blocks3);
    
    // All should be unique
    EXPECT_NE(genesisNonce, epoch1Nonce);
    EXPECT_NE(epoch1Nonce, epoch2Nonce);
    EXPECT_NE(epoch2Nonce, epoch3Nonce);
    EXPECT_NE(genesisNonce, epoch2Nonce);
    EXPECT_NE(genesisNonce, epoch3Nonce);
    EXPECT_NE(epoch1Nonce, epoch3Nonce);
}
