#ifndef PP_LEDGER_HEALTH_METRICS_H
#define PP_LEDGER_HEALTH_METRICS_H

#include "Client.h"

#include <optional>
#include <string>
#include <vector>

namespace pp {

/**
 * Prometheus text for nodes' HEALTH (Client::NodeHealth), as pp-http serves it on `/metrics`
 * (docs/ops/CONFIGURATION.md § Health and metrics). Nodes have no HTTP: a tool asks each over the
 * Client RPC and renders the answers here, one series per node (label `node`).
 */
struct HealthMetricsTarget {
  std::string node;                        ///< the label: beacon, miner, or a name given
  std::optional<Client::NodeHealth> health; ///< none: the node did not answer
};

std::string renderHealthMetrics(const std::vector<HealthMetricsTarget> &targets);

} // namespace pp

#endif // PP_LEDGER_HEALTH_METRICS_H
