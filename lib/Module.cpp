#include "Module.h"

namespace pp {

Module::Module() : logger_(logging::getLogger("")) {}

void Module::redirectLogger(const std::string &targetLoggerName) {
  logger_.switchTo(targetLoggerName);
}

logging::Logger &Module::log() const { return logger_; }

} // namespace pp
