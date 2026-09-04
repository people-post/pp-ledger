#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pp {

/**
 * Persistent sparse Merkle tree over uint64 keys (depth 64).
 *
 * - root() is O(1)
 * - setLeaf / clearLeaf are O(depth) with path copying (unchanged nodes shared)
 * - clone() is O(1) (shares root node)
 *
 * Used for account stateRoot: never requires hashing all leaves.
 */
class AccountStateTree {
public:
  static constexpr int kDepth = 64;

  AccountStateTree();

  /** O(1) — shares structure until the next divergent update. */
  AccountStateTree clone() const;

  /** Insert or replace leaf value (32-byte digest). */
  void setLeaf(uint64_t key, const std::string &leafHash);

  /** Remove leaf (revert to empty subtree). */
  void clearLeaf(uint64_t key);

  /** Current root digest (32 bytes). */
  const std::string &root() const { return rootHash_; }

  /** Empty-tree root (same for all fresh trees). */
  static const std::string &emptyRoot();

private:
  struct Node {
    std::string hash;
    std::shared_ptr<Node> left;
    std::shared_ptr<Node> right;
  };

  std::shared_ptr<Node> root_;
  std::string rootHash_;

  static const std::string &emptyAt(int height);
  static std::string hashInternal(const std::string &left,
                                  const std::string &right);
  static std::shared_ptr<Node>
  setRec(const std::shared_ptr<Node> &node, uint64_t key, int height,
         const std::string &leafHash, bool clear);
  static std::string nodeHash(const std::shared_ptr<Node> &node, int height);
};

} // namespace pp
