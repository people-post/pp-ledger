#include "AccountStateTree.h"

#include "lib/common/Utilities.h"

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

void AccountStateTree::setLeaf(uint64_t key, const std::string &leafHash) {
  root_ = setRec(root_, key, kDepth, leafHash, false);
  rootHash_ = nodeHash(root_, kDepth);
}

void AccountStateTree::clearLeaf(uint64_t key) {
  root_ = setRec(root_, key, kDepth, {}, true);
  rootHash_ = nodeHash(root_, kDepth);
}

} // namespace pp
