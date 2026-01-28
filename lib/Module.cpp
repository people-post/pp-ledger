#include "Module.h"

namespace pp {

Module::Module() {
  spLogger_ = logging::getLogger("");
}

void Module::redirectLogger(const std::string &targetLoggerName) {
  spLogger_->redirectTo(targetLoggerName);
}

logging::Logger &Module::log() const { return *spLogger_; }

} // namespace pp
