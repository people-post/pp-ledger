#include "server/UpstreamSet.h"

#include <gtest/gtest.h>

#include <map>

using namespace pp;

// beacon-authority bp2b: a relay or miner ranks its upstreams — the highest beacon generation it
// would follow, then the longest chain — so it moves to a new beacon even when nothing fails.

namespace {

std::string addr(const std::string &peer) { return "/ip4/127.0.0.1/udp/9000/adp/1.0.0/p2p/" + peer; }

Client::BeaconState stateAt(uint64_t generation, uint64_t nextBlockId, const std::string &hash = "") {
  Client::BeaconState state;
  state.nextBlockId = nextBlockId;
  if (generation > 0) {
    Client::SignedTip tip;
    tip.networkId = "test-net";
    tip.generation = generation;
    tip.height = nextBlockId - 1;
    tip.blockHash = hash.empty() ? "hash-" + std::to_string(tip.height) : hash;
    tip.signature = "sig";
    state.signedTip = tip;
  }
  return state;
}

using States = std::vector<std::optional<Client::BeaconState>>;
const auto kAny = [](const Client::SignedTip &) { return true; };

/** Answers STATUS when the test says so; records what the set did. */
struct Harness {
  std::map<size_t, UpstreamSet::StatusDone> pending;
  std::vector<std::string> followed;
  std::vector<std::string> learnt;
  std::vector<std::string> conflicts;
  std::function<bool(const Client::SignedTip &)> mayFollow = kAny;
  std::function<void(size_t, const Client::BeaconState &)> onLearn;
  std::set<std::string> refused;
  std::unique_ptr<UpstreamSet> set;

  explicit Harness(std::vector<std::string> configured) {
    UpstreamSet::Hooks hooks;
    hooks.fetchStatus = [this](size_t index, const std::string &, UpstreamSet::StatusDone done) {
      pending[index] = std::move(done);
    };
    hooks.admit = [](size_t, const Client::BeaconState &state) { return state.networkId != "other-net"; };
    hooks.learn = [this](size_t index, const Client::BeaconState &state) {
      learnt.push_back(std::to_string(index));
      if (onLearn) {
        onLearn(index, state);
      }
    };
    hooks.offerEquivocation = [this](const Client::Equivocation &, const std::string &from) {
      conflicts.push_back(from);
    };
    hooks.mayFollow = [this](const Client::SignedTip &tip) { return mayFollow(tip); };
    hooks.adopt = [this](const std::string &multiaddr) { return refused.count(multiaddr) == 0; };
    hooks.onFollow = [this](const std::string &multiaddr) { followed.push_back(multiaddr); };
    set = std::make_unique<UpstreamSet>(std::move(configured), std::move(hooks));
  }

  void answer(size_t index, Client::Roe<Client::BeaconState> status) {
    auto done = std::move(pending.at(index));
    pending.erase(index);
    done(std::move(status));
  }
  void answer(size_t index, const Client::BeaconState &state) { answer(index, Client::Roe<Client::BeaconState>(state)); }
  void fail(size_t index) {
    answer(index, Client::Roe<Client::BeaconState>(Client::Error(Client::E_NOT_CONNECTED, "down")));
  }
};

} // namespace

TEST(UpstreamSetChooseTest, TheHighestGenerationThenTheLongestChain) {
  const States states{stateAt(1, 50), stateAt(2, 10), stateAt(2, 12)};
  EXPECT_EQ(UpstreamSet::choose(states, kAny, {}, 0), std::make_optional<size_t>(2))
      << "a new beacon's chain is shorter, at first, than the old one's";
  EXPECT_EQ(UpstreamSet::choose({stateAt(1, 10), stateAt(1, 20)}, kAny, {}, 0), std::make_optional<size_t>(1));
  EXPECT_EQ(UpstreamSet::choose({stateAt(1, 10), stateAt(0, 30)}, kAny, {}, 0), std::make_optional<size_t>(0))
      << "no signed tip ranks below any";
}

TEST(UpstreamSetChooseTest, SkipsTheSilentTheAvoidedAndTipsNotWorthFollowing) {
  const States states{std::nullopt, stateAt(3, 10), stateAt(2, 40), stateAt(2, 30)};
  auto notGeneration3 = [](const Client::SignedTip &tip) { return tip.generation != 3; };
  EXPECT_EQ(UpstreamSet::choose(states, notGeneration3, {}, 0), std::make_optional<size_t>(2));
  EXPECT_EQ(UpstreamSet::choose(states, notGeneration3, {2}, 0), std::make_optional<size_t>(3));
  EXPECT_EQ(UpstreamSet::choose(states, notGeneration3, {2, 3}, 0), std::nullopt);
  EXPECT_EQ(UpstreamSet::choose({std::nullopt, std::nullopt}, kAny, {}, 0), std::nullopt);
}

TEST(UpstreamSetChooseTest, KeepsTheUpstreamFollowedUnlessFarBehindOrSuperseded) {
  const uint64_t lag = UpstreamSet::kLagTolerance;
  EXPECT_EQ(UpstreamSet::choose({stateAt(1, 10), stateAt(1, 10 + lag)}, kAny, {}, 0), std::make_optional<size_t>(0))
      << "two healthy relays a block apart: no flapping";
  EXPECT_EQ(UpstreamSet::choose({stateAt(1, 10), stateAt(1, 11 + lag)}, kAny, {}, 0), std::make_optional<size_t>(1));
  EXPECT_EQ(UpstreamSet::choose({stateAt(1, 10), stateAt(2, 3)}, kAny, {}, 0), std::make_optional<size_t>(1))
      << "a later generation wins whatever the height";
  EXPECT_EQ(UpstreamSet::choose({stateAt(1, 10), stateAt(1, 10)}, kAny, {0}, 0), std::make_optional<size_t>(1))
      << "not the avoided one";
}

TEST(UpstreamSetTest, RanksOnAllAnswersAndFollowsTheBest) {
  Harness h({addr("a"), addr("b"), addr("c")});
  EXPECT_EQ(h.set->active(), addr("a"));
  h.set->rerank("test");
  EXPECT_TRUE(h.set->ranking());
  ASSERT_EQ(h.pending.size(), 3u) << "STATUS from all of them at once";
  h.answer(0, stateAt(1, 20));
  h.fail(1);
  EXPECT_TRUE(h.set->ranking()) << "until every upstream answered or failed";
  h.answer(2, stateAt(2, 5));
  EXPECT_FALSE(h.set->ranking());
  EXPECT_EQ(h.set->active(), addr("c"));
  EXPECT_EQ(h.followed, std::vector<std::string>{addr("c")});
  EXPECT_EQ(h.set->chosen(), std::make_optional<size_t>(2));
  EXPECT_EQ(h.learnt, (std::vector<std::string>{"0", "2"})) << "what each answer carries is taken first";
  EXPECT_FALSE(h.set->lastStatus(1).has_value());
}

TEST(UpstreamSetTest, WhatOneUpstreamServesCanRuleOutAnother) {
  Harness h({addr("old-beacon"), addr("gateway")});
  bool cutoverHeld = false;
  // The gateway serves a certificate whose cutover rules out the old beacon's tip.
  h.onLearn = [&](size_t index, const Client::BeaconState &) { cutoverHeld = cutoverHeld || index == 1; };
  h.mayFollow = [&](const Client::SignedTip &tip) { return !cutoverHeld || tip.height <= 95; }; // cutover 95
  h.set->rerank("test");
  h.answer(0, stateAt(1, 100));
  h.answer(1, stateAt(1, 90));
  EXPECT_EQ(h.set->active(), addr("gateway")) << "ranked after both answers were taken in";
}

TEST(UpstreamSetTest, StatusForAnotherNetworkIsDisregarded) {
  Harness h({addr("a"), addr("b")});
  h.set->rerank("test");
  h.answer(0, stateAt(1, 10));
  auto foreign = stateAt(5, 100);
  foreign.networkId = "other-net";
  h.answer(1, foreign);
  EXPECT_EQ(h.set->active(), addr("a"));
  EXPECT_EQ(h.learnt, std::vector<std::string>{"0"});
}

TEST(UpstreamSetTest, NoneWorthFollowingKeepsTheCurrentOne) {
  Harness h({addr("a"), addr("b")});
  h.set->rerank("test");
  h.fail(0);
  h.fail(1);
  EXPECT_FALSE(h.set->chosen().has_value());
  EXPECT_EQ(h.set->active(), addr("a"));
  EXPECT_TRUE(h.followed.empty());
}

TEST(UpstreamSetTest, AskedAgainMidRoundRanksOnceMoreAfter) {
  Harness h({addr("a"), addr("b")});
  h.set->rerank("first");
  h.set->rerank("a new certificate");
  EXPECT_EQ(h.pending.size(), 2u) << "one round at a time";
  h.answer(0, stateAt(1, 10));
  h.answer(1, stateAt(1, 10));
  EXPECT_TRUE(h.set->ranking()) << "the second round started";
  EXPECT_EQ(h.pending.size(), 2u);
  h.answer(0, stateAt(1, 10));
  h.answer(1, stateAt(2, 3));
  EXPECT_FALSE(h.set->ranking());
  EXPECT_EQ(h.set->active(), addr("b"));
}

TEST(UpstreamSetTest, ALateAnswerFromAnEarlierRoundIsIgnored) {
  Harness h({addr("a"), addr("b")});
  h.set->rerank("first");
  auto stale = std::move(h.pending.at(1));
  h.pending.erase(1);
  h.answer(0, stateAt(1, 10));
  EXPECT_TRUE(h.set->ranking());
  stale(Client::Roe<Client::BeaconState>(Client::Error(Client::E_NOT_CONNECTED, "timed out")));
  EXPECT_FALSE(h.set->ranking());
  h.set->rerank("second");
  stale(Client::Roe<Client::BeaconState>(stateAt(9, 99))); // the first round's, arriving now
  EXPECT_TRUE(h.set->ranking());
  h.answer(0, stateAt(1, 10));
  h.answer(1, stateAt(1, 10));
  EXPECT_EQ(h.set->active(), addr("a"));
}

TEST(UpstreamSetTest, AFailedSyncMovesOnAndAvoidsThatUpstreamAWhile) {
  Harness h({addr("a"), addr("b"), addr("c")});
  h.set->noteFailed();
  EXPECT_EQ(h.set->active(), addr("b")) << "moved on at once";
  ASSERT_TRUE(h.set->ranking()) << "and ranks again";
  h.answer(0, stateAt(1, 99)); // the failed one claims the longest chain
  h.answer(1, stateAt(1, 10));
  h.answer(2, stateAt(1, 20));
  EXPECT_EQ(h.set->active(), addr("c")) << "the failed one is avoided";
}

TEST(UpstreamSetTest, OneUpstreamIsNeverRotatedNorRankedPeriodically) {
  Harness h({addr("a")});
  h.set->noteFailed();
  h.set->maintain();
  EXPECT_TRUE(h.pending.empty());
  h.set->rerank("start"); // asked for (at start): it runs, to learn what STATUS carries
  ASSERT_EQ(h.pending.size(), 1u);
  h.answer(0, stateAt(1, 10));
  EXPECT_EQ(h.set->chosen(), std::make_optional<size_t>(0));
}

TEST(UpstreamSetTest, TwoUpstreamsServingConflictingTipsAreEvidence) {
  Harness h({addr("a"), addr("b"), addr("c")});
  h.set->rerank("test");
  h.answer(0, stateAt(1, 10, "one"));
  h.answer(1, stateAt(1, 10, "two"));
  h.answer(2, stateAt(1, 10, "one"));
  EXPECT_EQ(h.conflicts, (std::vector<std::string>{"upstreams [0] and [1]", "upstreams [1] and [2]"}));
}

TEST(UpstreamSetTest, HintsAreAddedOnceAndOnlyWhenValidAndAdopted) {
  Harness h({addr("a")});
  h.refused.insert(addr("self"));
  EXPECT_EQ(h.set->addHints({addr("a"), addr("g1"), "not-a-multiaddr", addr("self"), addr("g1"), addr("g2")}), 2u);
  EXPECT_EQ(h.set->all(), (std::vector<std::string>{addr("a"), addr("g1"), addr("g2")}));
  EXPECT_EQ(h.set->addHints({addr("g2")}), 0u);

  std::vector<std::string> many;
  for (size_t i = 0; i < UpstreamSet::kMaxUpstreams + 5; ++i) {
    many.push_back(addr("h" + std::to_string(i)));
  }
  h.set->addHints(many);
  EXPECT_EQ(h.set->all().size(), UpstreamSet::kMaxUpstreams) << "a certificate cannot grow the list without bound";
}

TEST(UpstreamSetTest, AHintLearntMidRoundIsProbedInTheNextOne) {
  Harness h({addr("a"), addr("b")});
  h.onLearn = [&](size_t index, const Client::BeaconState &) {
    if (index == 1 && h.set->addHints({addr("gateway")}) > 0) {
      h.set->rerank("a new beacon certificate");
    }
  };
  h.set->rerank("test");
  h.answer(0, stateAt(1, 10));
  h.answer(1, stateAt(1, 10));
  ASSERT_TRUE(h.set->ranking());
  ASSERT_EQ(h.pending.size(), 3u) << "the next round includes the hint";
  h.answer(0, stateAt(1, 10));
  h.answer(1, stateAt(1, 10));
  h.answer(2, stateAt(2, 4));
  EXPECT_EQ(h.set->active(), addr("gateway"));
}

TEST(UpstreamSetTest, ARefusedBlockRanksAgainButNotOverAndOver) {
  Harness h({addr("old-side"), addr("gateway")});
  h.set->suspect("the upstream refused block 7");
  ASSERT_EQ(h.pending.size(), 2u) << "ranked at once, not at the next periodic round";
  h.answer(0, stateAt(1, 5));
  h.answer(1, stateAt(2, 8));
  EXPECT_EQ(h.set->active(), addr("gateway"));
  h.set->suspect("the upstream refused block 8");
  EXPECT_TRUE(h.pending.empty()) << "a round just ran: refusals in a burst rank once";

  Harness single({addr("only")});
  single.set->suspect("refused");
  EXPECT_TRUE(single.pending.empty()) << "nothing to choose from";
}
