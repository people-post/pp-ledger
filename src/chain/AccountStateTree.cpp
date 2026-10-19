#include "ledger/chain/AccountStateTree.h"

#include "ledger/lib/common/Utilities.h"

#include <array>

namespace pp {
namespace {

std::string hashPair(const std::string &left, const std::string &right) {
  return utl::sha256Raw(std::string("pp-ledger/smt/v1/node") + left + right);
}

const std::array<std::string, AccountStateTree::kDepth + 1> &emptyHashes() {
  static const std::array<std::string, AccountStateTree::kDepth + 1> kEmpty =
      [] {
        std::array<std::string, AccountStateTree::kDepth + 1> out{};
        out[0] = utl::sha256Raw(std::string("pp-ledger/smt/v1/empty-leaf"));
        for (int i = 1; i <= AccountStateTree::kDepth; ++i) {
          out[i] = hashPair(out[static_cast<size_t>(i - 1)],
                            out[static_cast<size_t>(i - 1)]);
        }
        return out;
      }();
  return kEmpty;
}

} // namespace

AccountStateTree::AccountStateTree() {
  rootHash_ = emptyAt(kDepth);
}

AccountStateTree AccountStateTree::clone() const {
  AccountStateTree out;
  out.root_ = root_;
  out.rootHash_ = rootHash_;
  return out;
}

const std::string &AccountStateTree::emptyRoot() { return emptyAt(kDepth); }

const std::string &AccountStateTree::emptyAt(int height) {
  return emptyHashes().at(static_cast<size_t>(height));
}

std::string AccountStateTree::hashInternal(const std::string &left,
                                           const std::string &right) {
  return hashPair(left, right);
}

std::string
AccountStateTree::nodeHash(const std::shared_ptr<Node> &node, int height) {
  if (!node) {
    return emptyAt(height);
  }
  return node->hash;
}

std::shared_ptr<AccountStateTree::Node>
AccountStateTree::setRec(const std::shared_ptr<Node> &node, uint64_t key,
                         int height, const std::string &leafHash, bool clear) {
  if (height == 0) {
    if (clear) {
      return nullptr;
    }
    auto out = std::make_shared<Node>();
    out->hash = leafHash;
    return out;
  }

  const bool goRight = ((key >> static_cast<uint64_t>(height - 1)) & 1ULL) != 0;
  auto left = node ? node->left : nullptr;
  auto right = node ? node->right : nullptr;
  if (goRight) {
    right = setRec(right, key, height - 1, leafHash, clear);
  } else {
    left = setRec(left, key, height - 1, leafHash, clear);
  }

  if (!left && !right) {
    return nullptr;
  }

  auto out = std::make_shared<Node>();
  out->left = std::move(left);
  out->right = std::move(right);
  out->hash = hashInternal(nodeHash(out->left, height - 1),
                           nodeHash(out->right, height - 1));
  return out;
}

std::vector<std::string> AccountStateTree::siblings(uint64_t key) const {
  std::vector<std::string> out(kDepth);
  std::shared_ptr<Node> node = root_;
  // From the root down: at height h, the sibling is the other child's subtree (height h - 1).
  for (int height = kDepth; height > 0; --height) {
    const bool goRight = ((key >> static_cast<uint64_t>(height - 1)) & 1ULL) != 0;
    const std::shared_ptr<Node> other = node ? (goRight ? node->left : node->right) : nullptr;
    out[static_cast<size_t>(height - 1)] = nodeHash(other, height - 1);
    node = node ? (goRight ? node->right : node->left) : nullptr;
  }
  return out;
}

std::string AccountStateTree::rootFrom(uint64_t key, const std::string &leafHash,
                                       const std::vector<std::string> &siblings) {
  if (siblings.size() != static_cast<size_t>(kDepth)) {
    return {};
  }
  std::string hash = leafHash;
  for (int height = 1; height <= kDepth; ++height) {
    const bool right = ((key >> static_cast<uint64_t>(height - 1)) & 1ULL) != 0;
    const std::string &sibling = siblings[static_cast<size_t>(height - 1)];
    hash = right ? hashInternal(sibling, hash) : hashInternal(hash, sibling);
  }
  return hash;
}

void AccountStateTree::setLeaf(uint64_t key, const std::string &leafHash) {
  root_ = setRec(root_, key, kDepth, leafHash, false);
  rootHash_ = nodeHash(root_, kDepth);
}

void AccountStateTree::clearLeaf(uint64_t key) {
  root_ = setRec(root_, key, kDepth, {}, true);
  rootHash_ = nodeHash(root_, kDepth);
}

} // namespace pp
