#include "BlockWaitList.h"

namespace pp {

std::vector<BlockWaitList::Reply> BlockWaitList::park(const std::string &peerId, uint64_t knownNext,
                                                      Clock::time_point deadline, Reply reply, uint64_t tip) {
  std::vector<Reply> due;
  if (auto it = waits_.find(peerId); it != waits_.end()) {
    due.push_back(std::move(it->second.reply));
    waits_.erase(it);
  }
  if (tip > knownNext || waits_.size() >= capacity_) {
    due.push_back(std::move(reply));
    return due;
  }
  waits_.emplace(peerId, Wait{knownNext, deadline, std::move(reply)});
  return due;
}

std::vector<BlockWaitList::Reply> BlockWaitList::poll(uint64_t tip, Clock::time_point now) {
  std::vector<Reply> due;
  for (auto it = waits_.begin(); it != waits_.end();) {
    if (tip > it->second.knownNext || now >= it->second.deadline) {
      due.push_back(std::move(it->second.reply));
      it = waits_.erase(it);
    } else {
      ++it;
    }
  }
  return due;
}

std::vector<BlockWaitList::Reply> BlockWaitList::takeAll() {
  std::vector<Reply> all;
  for (auto &[_, wait] : waits_) {
    all.push_back(std::move(wait.reply));
  }
  waits_.clear();
  return all;
}

} // namespace pp
