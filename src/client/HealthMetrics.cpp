#include "HealthMetrics.h"

#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>

namespace pp {

namespace {

/** A Prometheus label value: backslash, quote and newline escaped. */
std::string label(const std::string &value) {
  std::string out;
  for (char c : value) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

/** A sample value: integers exactly (block ids pass a million), else three decimals. */
std::string number(double v) {
  std::ostringstream out;
  if (v == std::floor(v) && std::fabs(v) < 9e15) {
    out << static_cast<long long>(v);
  } else {
    out << std::fixed << std::setprecision(3) << v;
  }
  return out.str();
}

using Value = std::function<std::optional<double>(const HealthMetricsTarget &)>;

/** One gauge family: its header once, then a sample per node that has a value. */
void gauge(std::ostringstream &out, const std::vector<HealthMetricsTarget> &targets, const std::string &name,
           const std::string &help, const Value &value) {
  out << "# HELP " << name << " " << help << "\n# TYPE " << name << " gauge\n";
  for (const auto &target : targets) {
    if (const auto v = value(target)) {
      out << name << "{node=\"" << label(target.node) << "\"} " << number(*v) << "\n";
    }
  }
}

/** One counter family (Prometheus `_total`): its header once, then a sample per node that has a value. */
void counter(std::ostringstream &out, const std::vector<HealthMetricsTarget> &targets, const std::string &name,
             const std::string &help, const Value &value) {
  out << "# HELP " << name << " " << help << "\n# TYPE " << name << " counter\n";
  for (const auto &target : targets) {
    if (const auto v = value(target)) {
      out << name << "{node=\"" << label(target.node) << "\"} " << number(*v) << "\n";
    }
  }
}

using RequestValue = std::function<double(const Client::NodeHealth::RequestCount &)>;

/** A counter per node and request type (label `type`, Client::requestTypeName). */
void requestCounter(std::ostringstream &out, const std::vector<HealthMetricsTarget> &targets, const std::string &name,
                    const std::string &help, const RequestValue &value) {
  out << "# HELP " << name << " " << help << "\n# TYPE " << name << " counter\n";
  for (const auto &target : targets) {
    if (!target.health) {
      continue;
    }
    for (const auto &count : target.health->requests) {
      out << name << "{node=\"" << label(target.node) << "\",type=\"" << label(Client::requestTypeName(count.type))
          << "\"} " << number(value(count)) << "\n";
    }
  }
}

/** A gauge read from a node that answered. */
template <typename F> Value answered(F f) {
  return [f](const HealthMetricsTarget &t) -> std::optional<double> {
    if (!t.health) {
      return std::nullopt;
    }
    return f(*t.health);
  };
}

double flag(bool b) { return b ? 1 : 0; }

/** A value a node may not report (std::optional), from a node that answered. */
template <typename T, typename F> Value optionalField(F field) {
  return answered([field](const Client::NodeHealth &h) -> std::optional<double> {
    const std::optional<T> &v = field(h);
    if (!v) {
      return std::nullopt;
    }
    return static_cast<double>(*v);
  });
}

} // namespace

std::string renderHealthMetrics(const std::vector<HealthMetricsTarget> &targets) {
  using H = Client::NodeHealth;
  std::ostringstream out;
  out << "# HELP pp_ledger_info Node identity\n# TYPE pp_ledger_info gauge\n";
  for (const auto &t : targets) {
    if (t.health) {
      out << "pp_ledger_info{node=\"" << label(t.node) << "\",role=\"" << label(t.health->role) << "\",peer_id=\""
          << label(t.health->peerId) << "\",network=\"" << label(t.health->networkId) << "\"} 1\n";
    }
  }
  gauge(out, targets, "pp_ledger_up", "The node answered and its server thread is reporting",
        [](const HealthMetricsTarget &t) -> std::optional<double> { return flag(t.health && t.health->live()); });
  gauge(out, targets, "pp_ledger_ready", "Ready to serve (synced, not halted)",
        answered([](const H &h) -> std::optional<double> { return flag(h.readyNow()); }));
  gauge(out, targets, "pp_ledger_next_block_id", "Next block id of the local chain",
        answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.nextBlockId); }));
  gauge(out, targets, "pp_ledger_upstream_next_block_id", "Next block id the upstream last reported",
        answered([](const H &h) -> std::optional<double> {
          if (!h.upstreamNextBlockId) {
            return std::nullopt;
          }
          return static_cast<double>(*h.upstreamNextBlockId);
        }));
  gauge(out, targets, "pp_ledger_sync_lag_blocks", "Blocks behind the upstream",
        answered([](const H &h) -> std::optional<double> {
          if (!h.upstreamNextBlockId) {
            return std::nullopt;
          }
          return *h.upstreamNextBlockId > h.nextBlockId ? static_cast<double>(*h.upstreamNextBlockId - h.nextBlockId)
                                                         : 0;
        }));
  gauge(out, targets, "pp_ledger_last_sync_age_seconds", "Seconds since the last successful sync",
        answered([](const H &h) -> std::optional<double> {
          if (!h.lastSyncAgeSeconds) {
            return std::nullopt;
          }
          return static_cast<double>(*h.lastSyncAgeSeconds);
        }));
  gauge(out, targets, "pp_ledger_signed_tip_height", "Height of the beacon's latest signed tip held",
        answered([](const H &h) -> std::optional<double> {
          if (!h.signedTipHeight) {
            return std::nullopt;
          }
          return static_cast<double>(*h.signedTipHeight);
        }));
  gauge(out, targets, "pp_ledger_beacon_generation", "Beacon generation in force",
        answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.generation); }));
  gauge(out, targets, "pp_ledger_halted", "Halted on equivocation evidence",
        answered([](const H &h) -> std::optional<double> { return flag(h.halted); }));
  gauge(out, targets, "pp_ledger_recovery_pending", "A recovery of the admin set is pending",
        answered([](const H &h) -> std::optional<double> { return flag(h.recoveryPending); }));
  gauge(out, targets, "pp_ledger_standing_down", "Beacon stood down (another key is certified)",
        answered([](const H &h) -> std::optional<double> { return flag(h.standingDown); }));
  gauge(out, targets, "pp_ledger_request_queue_depth", "Requests waiting in the server queue",
        answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.requestQueueDepth); }));
  gauge(out, targets, "pp_ledger_report_age_seconds", "Seconds since the node's server thread last reported",
        answered([](const H &h) -> std::optional<double> {
          if (h.reportAgeMs < 0) {
            return std::nullopt;
          }
          return static_cast<double>(h.reportAgeMs) / 1000.0;
        }));

  // Load (load-resilience lr1).
  requestCounter(out, targets, "pp_ledger_requests_total", "Requests served, by type",
                 [](const H::RequestCount &c) { return static_cast<double>(c.served); });
  requestCounter(out, targets, "pp_ledger_request_wait_seconds_total", "Time requests waited in the queue, by type",
                 [](const H::RequestCount &c) { return static_cast<double>(c.waitMs) / 1000.0; });
  requestCounter(out, targets, "pp_ledger_request_service_seconds_total", "Time spent in request handlers, by type",
                 [](const H::RequestCount &c) { return static_cast<double>(c.serviceMs) / 1000.0; });
  counter(out, targets, "pp_ledger_requests_busy_total", "Requests answered busy before queueing",
          answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.requestsBusy); }));
  counter(out, targets, "pp_ledger_requests_refused_total", "Requests refused unserved (size, peer, form, type, direction)",
          answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.requestsRefused); }));
  counter(out, targets, "pp_ledger_requests_expired_total", "Requests that waited past their client and went unserved",
          answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.requestsExpired); }));
  counter(out, targets, "pp_ledger_loop_turns_slow_total", "Run-loop turns longer than a second",
          answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.loopTurnsSlow); }));
  gauge(out, targets, "pp_ledger_loop_turn_max_seconds", "Longest run-loop turn in the last 10 to 20 seconds",
        answered([](const H &h) -> std::optional<double> { return static_cast<double>(h.loopTurnMaxMs) / 1000.0; }));
  gauge(out, targets, "pp_ledger_resident_memory_bytes", "Resident memory of the node",
        optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.rssBytes; }));
  counter(out, targets, "pp_ledger_blocks_sealed_total", "Miner: blocks produced and broadcast",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.blocksSealed; }));
  counter(out, targets, "pp_ledger_blocks_committed_total", "Miner: own blocks committed on the beacon's signed tip",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.blocksCommitted; }));
  counter(out, targets, "pp_ledger_blocks_abandoned_total", "Miner: own blocks no upstream took",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.blocksAbandoned; }));
  gauge(out, targets, "pp_ledger_last_seal_delay_seconds", "Miner: the last seal, after its slot ended",
        answered([](const H &h) -> std::optional<double> {
          if (!h.lastSealDelayMs) {
            return std::nullopt;
          }
          return static_cast<double>(*h.lastSealDelayMs) / 1000.0;
        }));
  gauge(out, targets, "pp_ledger_tx_pool_records", "Beacon: transactions in the pool",
        optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolRecords; }));
  gauge(out, targets, "pp_ledger_tx_pool_bytes", "Beacon: bytes in the pool",
        optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolBytes; }));
  gauge(out, targets, "pp_ledger_tx_pool_oldest_age_seconds", "Beacon: how long the oldest pooled transaction has waited",
        optionalField<int64_t>([](const H &h) -> const std::optional<int64_t> & { return h.txPoolOldestAgeSeconds; }));
  counter(out, targets, "pp_ledger_tx_pool_added_total", "Beacon: transactions taken into the pool",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolAdded; }));
  counter(out, targets, "pp_ledger_tx_pool_duplicates_total", "Beacon: transactions already pooled",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolDuplicates; }));
  counter(out, targets, "pp_ledger_tx_pool_full_total", "Beacon: transactions refused, the pool full",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolFull; }));
  counter(out, targets, "pp_ledger_tx_pool_expired_total", "Beacon: transactions expired in the pool",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolExpired; }));
  counter(out, targets, "pp_ledger_tx_pool_included_total", "Beacon: transactions that left the pool in a block",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolIncluded; }));
  counter(out, targets, "pp_ledger_tx_pool_sender_full_total", "Beacon: transactions refused, their payer's share full",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolSenderFull; }));
  counter(out, targets, "pp_ledger_tx_pool_dropped_total", "Beacon: pooled transactions dropped, no longer applicable",
          optionalField<uint64_t>([](const H &h) -> const std::optional<uint64_t> & { return h.txPoolDropped; }));
  return out.str();
}

} // namespace pp
