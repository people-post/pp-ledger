#include "HealthEndpoint.h"

#include "httplib.h"

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

void gauge(std::ostringstream &out, const std::string &name, const std::string &help, double value) {
  out << "# HELP " << name << " " << help << "\n# TYPE " << name << " gauge\n" << name << " " << value << "\n";
}

} // namespace

HealthEndpoint::HealthEndpoint() { redirectLogger("HealthEndpoint"); }

HealthEndpoint::~HealthEndpoint() { stop(); }

HealthEndpoint::Roe<void> HealthEndpoint::start(const std::string &bind, uint16_t port) {
  if (http_) {
    return Error(1, "Health endpoint already started");
  }
  http_ = std::make_unique<httplib::Server>();
  http_->Get("/healthz", [this](const httplib::Request &, httplib::Response &res) {
    const auto [snapshot, fresh] = current();
    res.status = fresh ? 200 : 503;
    res.set_content(fresh ? "ok\n" : "server thread not reporting\n", "text/plain");
  });
  http_->Get("/readyz", [this](const httplib::Request &, httplib::Response &res) {
    const auto [snapshot, fresh] = current();
    const bool ready = fresh && snapshot.ready;
    res.status = ready ? 200 : 503;
    res.set_content(ready ? "ready\n" : (fresh ? snapshot.notReady : "server thread not reporting") + "\n",
                    "text/plain");
  });
  http_->Get("/metrics", [this](const httplib::Request &, httplib::Response &res) {
    const auto [snapshot, fresh] = current();
    res.set_content(renderMetrics(snapshot, fresh), "text/plain; version=0.0.4");
  });
  const int bound = port == 0 ? http_->bind_to_any_port(bind) : (http_->bind_to_port(bind, port) ? port : -1);
  if (bound <= 0) {
    http_.reset();
    return Error(1, "Health endpoint: cannot listen on " + bind + ":" + std::to_string(port));
  }
  port_ = static_cast<uint16_t>(bound);
  thread_ = std::thread([this]() { http_->listen_after_bind(); });
  log().info << "Health endpoint on http://" << bind << ":" << port_ << " (/healthz, /readyz, /metrics)";
  return {};
}

void HealthEndpoint::stop() {
  if (http_) {
    http_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  http_.reset();
}

void HealthEndpoint::publish(HealthSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mu_);
  snapshot_ = std::move(snapshot);
  publishedAt_ = std::chrono::steady_clock::now();
  published_ = true;
}

std::pair<HealthSnapshot, bool> HealthEndpoint::current() const {
  std::lock_guard<std::mutex> lock(mu_);
  const bool fresh = published_ && std::chrono::steady_clock::now() - publishedAt_ < kStaleAfter;
  return {snapshot_, fresh};
}

std::string HealthEndpoint::renderMetrics(const HealthSnapshot &s, bool fresh) {
  std::ostringstream out;
  out << "# HELP pp_ledger_info Node identity\n# TYPE pp_ledger_info gauge\n"
      << "pp_ledger_info{role=\"" << label(s.role) << "\",peer_id=\"" << label(s.peerId) << "\",network=\""
      << label(s.networkId) << "\"} 1\n";
  gauge(out, "pp_ledger_up", "The server thread is reporting", fresh ? 1 : 0);
  gauge(out, "pp_ledger_ready", "Ready to serve (synced, not halted)", s.ready && fresh ? 1 : 0);
  gauge(out, "pp_ledger_next_block_id", "Next block id of the local chain", static_cast<double>(s.nextBlockId));
  if (s.upstreamNextBlockId) {
    gauge(out, "pp_ledger_upstream_next_block_id", "Next block id the upstream last reported",
          static_cast<double>(*s.upstreamNextBlockId));
    const double lag = *s.upstreamNextBlockId > s.nextBlockId
                           ? static_cast<double>(*s.upstreamNextBlockId - s.nextBlockId)
                           : 0;
    gauge(out, "pp_ledger_sync_lag_blocks", "Blocks behind the upstream", lag);
  }
  if (s.lastSyncAgeSeconds) {
    gauge(out, "pp_ledger_last_sync_age_seconds", "Seconds since the last successful sync",
          static_cast<double>(*s.lastSyncAgeSeconds));
  }
  if (s.signedTipHeight) {
    gauge(out, "pp_ledger_signed_tip_height", "Height of the beacon's latest signed tip held",
          static_cast<double>(*s.signedTipHeight));
  }
  gauge(out, "pp_ledger_beacon_generation", "Beacon generation in force", static_cast<double>(s.generation));
  gauge(out, "pp_ledger_halted", "Halted on equivocation evidence", s.halted ? 1 : 0);
  gauge(out, "pp_ledger_recovery_pending", "A recovery of the admin set is pending", s.recoveryPending ? 1 : 0);
  gauge(out, "pp_ledger_standing_down", "Beacon stood down (another key is certified)", s.standingDown ? 1 : 0);
  gauge(out, "pp_ledger_request_queue_depth", "Requests waiting in the server queue",
        static_cast<double>(s.requestQueueDepth));
  return out.str();
}

} // namespace pp
