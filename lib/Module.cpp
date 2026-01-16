#include "Module.h"

namespace pp {

Module::Module(const std::string &name) {
  spLogger_ = logging::getLogger(name);
}

void Module::redirectLogger(const std::string &targetLoggerName) {
  spLogger_->redirectTo(targetLoggerName);
}

void Module::clearLoggerRedirect() { spLogger_->clearRedirect(); }

logging::Logger &Module::log() const { return *spLogger_; }

} // namespace pp
