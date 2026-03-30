#pragma once

#include "ITxHandler.h"
#include "../ledger/Ledger.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace pp {

/** RecordHandler owns the per-record-type tx handlers. */
class RecordHandler final {
public:
  static constexpr std::size_t kNumTxTypes = 7;

  RecordHandler();
  ~RecordHandler() = default;

  RecordHandler(const RecordHandler &) = delete;
  RecordHandler &operator=(const RecordHandler &) = delete;
  RecordHandler(RecordHandler &&) = delete;
  RecordHandler &operator=(RecordHandler &&) = delete;

  /** Get handler for a ledger record type id (0..6). */
  ITxHandler *get(std::size_t type);
  const ITxHandler *get(std::size_t type) const;

  /** Set per-handler logger names (optional). */
  void redirectLoggers(const std::string &baseName);

private:
  std::array<std::unique_ptr<ITxHandler>, kNumTxTypes> handlers_{};
};

} // namespace pp

