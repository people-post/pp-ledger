#ifndef PP_LEDGER_APP_NODE_OPTIONS_H
#define PP_LEDGER_APP_NODE_OPTIONS_H

#include "common/Logger.h"

#include <cli11.hpp>

#include <string>

namespace pp::app {

/**
 * Options every node binary shares, each also read from the environment (CONFIGURATION.md
 * § Environment): `-d` / `PP_LEDGER_WORK_DIR`, `--log-level` / `PP_LEDGER_LOG_LEVEL`
 * (debug | info | warning | error; default warning), `--debug` (= --log-level debug).
 */
struct NodeOptions {
  std::string workDir;
  std::string logLevel{"warning"};
  bool debug{false};

  void add(CLI::App &app) {
    app.add_option("-d,--work-dir", workDir, "Work directory (required)")->envname("PP_LEDGER_WORK_DIR")->required();
    app.add_option("--log-level", logLevel, "Log level: debug, info, warning or error")
        ->envname("PP_LEDGER_LOG_LEVEL")
        ->check(CLI::IsMember({"debug", "info", "warning", "error"}, CLI::ignore_case));
    app.add_flag("--debug", debug, "Same as --log-level debug");
  }

  logging::Level level() const {
    if (debug || CLI::detail::to_lower(logLevel) == "debug") {
      return logging::Level::DEBUG;
    }
    const std::string name = CLI::detail::to_lower(logLevel);
    if (name == "info") {
      return logging::Level::INFO;
    }
    if (name == "error") {
      return logging::kLevelError;
    }
    return logging::Level::WARNING;
  }

  /** Apply the level to the root logger. */
  void applyLogLevel() const {
    auto logger = logging::getRootLogger();
    logger.setLevel(level());
    logger.info << "Logging level set to " << (debug ? std::string("debug") : logLevel);
  }
};

} // namespace pp::app

#endif // PP_LEDGER_APP_NODE_OPTIONS_H
