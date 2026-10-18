#include "AccountBuffer.h"
#include "AccountStateTree.h"
#include "lib/common/Crypto.h"
#include "lib/common/Utilities.h"

#include <gtest/gtest.h>

using namespace pp;

TEST(AccountStateTreeTest, EmptyRootStable) {
  AccountStateTree a;
  AccountStateTree b;
  EXPECT_EQ(a.root(), b.root());
  EXPECT_EQ(a.root(), AccountStateTree::emptyRoot());
  EXPECT_EQ(a.root().size(), utl::SHA256_DIGEST_SIZE);
}

TEST(AccountStateTreeTest, SetAndClearLeafChangesRoot) {
  AccountStateTree tree;
  const std::string empty = tree.root();
  const std::string leaf = utl::sha256Raw("leaf-a");
  tree.setLeaf(42, leaf);
  EXPECT_NE(tree.root(), empty);
  AccountStateTree same;
  same.setLeaf(42, leaf);
  EXPECT_EQ(tree.root(), same.root());
  tree.clearLeaf(42);
  EXPECT_EQ(tree.root(), empty);
}

TEST(AccountStateTreeTest, CloneSharesUntilWrite) {
  AccountStateTree base;
  base.setLeaf(1, utl::sha256Raw("one"));
  AccountStateTree fork = base.clone();
  EXPECT_EQ(fork.root(), base.root());
  fork.setLeaf(2, utl::sha256Raw("two"));
  EXPECT_NE(fork.root(), base.root());
  // Base unchanged
  AccountStateTree expected;
  expected.setLeaf(1, utl::sha256Raw("one"));
  EXPECT_EQ(base.root(), expected.root());
}

TEST(AccountBufferStateRootTest, IncrementalRootMatchesIndependentReplay) {
  AccountBuffer a;
  AccountBuffer::Account fee;
  fee.id = AccountBuffer::ID_FEE;
  fee.wallet.keyType = Crypto::TK_ML_DSA_65;
  fee.wallet.publicKeys = {"pk"};
  fee.wallet.minSignatures = 1;
  ASSERT_TRUE(a.add(fee).isOk());

  AccountBuffer::Account user;
  user.id = AccountBuffer::ID_FIRST_USER;
  user.wallet.keyType = Crypto::TK_ML_DSA_65;
  user.wallet.publicKeys = {"pk2"};
  user.wallet.minSignatures = 1;
  user.wallet.mBalances[AccountBuffer::ID_GENESIS] = 100;
  ASSERT_TRUE(a.add(user).isOk());
  ASSERT_TRUE(a.depositBalance(AccountBuffer::ID_FIRST_USER,
                               AccountBuffer::ID_GENESIS, 50)
                  .isOk());

  AccountBuffer b;
  ASSERT_TRUE(b.add(fee).isOk());
  ASSERT_TRUE(b.add(user).isOk());
  ASSERT_TRUE(b.depositBalance(AccountBuffer::ID_FIRST_USER,
                               AccountBuffer::ID_GENESIS, 50)
                  .isOk());
  EXPECT_EQ(a.calculateStateRoot(), b.calculateStateRoot());
}

TEST(AccountBufferStateRootTest, RemoveClearsLeaf) {
  AccountBuffer buf;
  AccountBuffer::Account fee;
  fee.id = AccountBuffer::ID_FEE;
  fee.wallet.keyType = Crypto::TK_ML_DSA_65;
  fee.wallet.publicKeys = {"pk"};
  fee.wallet.minSignatures = 1;
  ASSERT_TRUE(buf.add(fee).isOk());
  const std::string withFee = buf.calculateStateRoot();
  EXPECT_NE(withFee, AccountStateTree::emptyRoot());
  buf.remove(AccountBuffer::ID_FEE);
  EXPECT_EQ(buf.calculateStateRoot(), AccountStateTree::emptyRoot());
  EXPECT_FALSE(buf.hasAccount(AccountBuffer::ID_FEE));
}

// Account proofs: a key's siblings and its leaf give the root, for keys held and keys not held.
TEST(AccountStateTreeTest, SiblingsAndALeafGiveTheRoot) {
  AccountStateTree tree;
  const std::vector<uint64_t> keys = {0, 1, 2, 3, (1ULL << 20) + 5, (1ULL << 30) + 7, ~0ULL};
  for (uint64_t key : keys) {
    tree.setLeaf(key, utl::sha256Raw("leaf " + std::to_string(key)));
  }
  for (uint64_t key : keys) {
    const auto siblings = tree.siblings(key);
    ASSERT_EQ(siblings.size(), 64u);
    EXPECT_EQ(AccountStateTree::rootFrom(key, utl::sha256Raw("leaf " + std::to_string(key)), siblings), tree.root());
    EXPECT_NE(AccountStateTree::rootFrom(key, utl::sha256Raw("other"), siblings), tree.root());
  }
  // A key nobody holds: the empty leaf on its path gives the root.
  const uint64_t absent = (1ULL << 30) + 8;
  EXPECT_EQ(AccountStateTree::rootFrom(absent, AccountStateTree::emptyLeaf(), tree.siblings(absent)), tree.root());
  // Most siblings of a sparse tree are empty subtrees: what proofs leave out.
  size_t empty = 0;
  const auto siblings = tree.siblings((1ULL << 30) + 7);
  for (int h = 0; h < 64; ++h) {
    empty += AccountStateTree::isEmptyAt(h, siblings[static_cast<size_t>(h)]) ? 1 : 0;
  }
  EXPECT_GT(empty, 55u);
}
