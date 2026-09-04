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
