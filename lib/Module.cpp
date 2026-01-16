#include "Module.h"

namespace pp {

Module::Module(const std::string &name) : loggerName_(name) {}

void Module::redirectLogger(const std::string &targetLoggerName) {
  auto &logger = logging::getLogger(loggerName_);
  logger.redirectTo(targetLoggerName);
}

void Module::clearLoggerRedirect() {
  auto &logger = logging::getLogger(loggerName_);
  logger.clearRedirect();
}

const std::string &Module::getLoggerName() const { return loggerName_; }

logging::Logger &Module::log() { return logging::getLogger(loggerName_); }

} // namespace pp
