#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

  /**
   * The siblings along `key`'s path, from the leaf's up to the root's children (kDepth of them):
   * with the leaf, they give the root (rootFrom). An absent key's leaf is emptyLeaf().
   */
  std::vector<std::string> siblings(uint64_t key) const;
  /** The root `leafHash` at `key` and its `siblings` (leaf level first) make. */
  static std::string rootFrom(uint64_t key, const std::string &leafHash, const std::vector<std::string> &siblings);
  /** The leaf of a key nobody holds. */
  static const std::string &emptyLeaf() { return emptyAt(0); }
  /** Whether `sibling` at `height` (0: the leaf's) is the empty subtree's: proofs leave those out. */
  static bool isEmptyAt(int height, const std::string &sibling) { return sibling == emptyAt(height); }
  /** The empty subtree of `height`, to put back what a proof left out. */
  static const std::string &emptySubtree(int height) { return emptyAt(height); }

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
