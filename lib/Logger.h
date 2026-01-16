#ifndef PP_LEDGER_LOGGER_H
#define PP_LEDGER_LOGGER_H

#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace pp {
namespace logging {

enum class Level { DEBUG = 0, INFO = 1, WARNING = 2, ERROR = 3, CRITICAL = 4 };

class Handler {
public:
  virtual ~Handler() = default;
  virtual void emit(Level level, const std::string &loggerName,
                    const std::string &message) = 0;

  void setLevel(Level level) { level_ = level; }
  Level getLevel() const { return level_; }

protected:
  Level level_ = Level::DEBUG;
};

class ConsoleHandler : public Handler {
public:
  void emit(Level level, const std::string &loggerName,
            const std::string &message) override;
};

class FileHandler : public Handler {
public:
  explicit FileHandler(const std::string &filename);
  ~FileHandler() override;
  void emit(Level level, const std::string &loggerName,
            const std::string &message) override;

private:
  std::ofstream file_;
  std::string filename_;
};

class Logger;
class LogStream;

class LogProxy {
public:
  LogProxy(Logger *logger, Level level);

  // Stream operator that creates LogStream
  template <typename T> LogStream operator<<(const T &value);

private:
  Logger *logger_;
  Level level_;
};

class LogStream {
public:
  LogStream(Logger *logger, Level level);
  ~LogStream();

  // Delete copy operations
  LogStream(const LogStream &) = delete;
  LogStream &operator=(const LogStream &) = delete;

  // Enable move operations
  LogStream(LogStream &&other) noexcept;
  LogStream &operator=(LogStream &&other) noexcept;

  // Stream operator
  template <typename T> LogStream &operator<<(const T &value) {
    stream_ << value;
    return *this;
  }

private:
  Logger *logger_;
  Level level_;
  std::ostringstream stream_;
  bool moved_;
};

class Logger {
public:
  Logger(const std::string &name);
  ~Logger() = default;

  // Stream-style logging as member variables
  LogProxy debug;
  LogProxy info;
  LogProxy warning;
  LogProxy error;
  LogProxy critical;

  // Configuration
  void setLevel(Level level) { level_ = level; }
  Level getLevel() const { return level_; }

  void addHandler(std::shared_ptr<Handler> spHandler);
  void addFileHandler(const std::string &filename, Level level = Level::DEBUG);

  // Redirect logs to another logger
  void redirectTo(const std::string &targetLoggerName);
  void clearRedirect();
  bool hasRedirect() const { return !redirectTarget_.empty(); }
  const std::string &getRedirectTarget() const { return redirectTarget_; }

  const std::string &getName() const { return name_; }

private:
  friend class LogStream;
  friend class LogProxy;

  void log(Level level, const std::string &message);
  std::string formatMessage(Level level, const std::string &message);
  std::string levelToString(Level level);

  std::string name_;
  Level level_;
  std::string redirectTarget_;
  std::vector<std::shared_ptr<Handler>> spHandlers_;
  std::mutex mutex_;
};

// Template implementation for LogProxy (must be after LogStream definition)
template <typename T> LogStream LogProxy::operator<<(const T &value) {
  LogStream stream(logger_, level_);
  stream << value;
  return stream;
}

// Global logger management
std::shared_ptr<Logger> getLogger(const std::string &name = "");
std::shared_ptr<Logger> getRootLogger();

} // namespace logging
} // namespace pp

#endif // PP_LEDGER_LOGGER_H
