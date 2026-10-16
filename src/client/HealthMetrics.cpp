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
  return out.str();
}

} // namespace pp
