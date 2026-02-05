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

// Forward declarations
class Logger;
class LogStream;
class LoggerNode;

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

// LoggerNode - Internal tree node structure
class LoggerNode : public std::enable_shared_from_this<LoggerNode> {
public:
  explicit LoggerNode(const std::string &name);
  ~LoggerNode() = default;

  // Configuration
  void setLevel(Level level) { level_ = level; }
  Level getLevel() const { return level_; }

  void addHandler(std::shared_ptr<Handler> spHandler);
  void addFileHandler(const std::string &filename, Level level);

  // Control log propagation to parent
  void setPropagate(bool propagate) { propagate_ = propagate; }
  bool getPropagate() const { return propagate_; }

  // Tree structure methods
  void setParent(std::weak_ptr<LoggerNode> parent) { parent_ = parent; }
  std::shared_ptr<LoggerNode> getParent() const { return parent_.lock(); }
  void addChild(std::shared_ptr<LoggerNode> child);
  void removeChild(LoggerNode* child);
  const std::vector<std::shared_ptr<LoggerNode>>& getChildren() const { return spChildren_; }

  // Logging
  void log(Level level, const std::string &message);

  // Name access - only stores node name, not full path
  const std::string &getName() const { return name_; }
  // Get full hierarchical name by traversing to root
  std::string getFullName() const;

  std::shared_ptr<LoggerNode> getOrInitChild(const std::string& fullName);

private:
  std::shared_ptr<LoggerNode> getOrInitDirectChild(const std::string& name);

  void logToHandlers(Level level, const std::string &message);
  
  // Internal logging with originating logger name (for propagation)
  void logWithOriginatingName(Level level, const std::string &message, const std::string &originatingLoggerName);
  void logToHandlersWithOriginatingName(Level level, const std::string &message, const std::string &originatingLoggerName);
  std::string formatMessage(Level level, const std::string &message);
  std::string formatMessage(Level level, const std::string &message, const std::string &originatingLoggerName);
  std::string levelToString(Level level);

  std::string name_;  // Only the node name, not the full path
  std::weak_ptr<LoggerNode> parent_;  // Parent node in the tree
  Level level_{ Level::DEBUG };
  bool propagate_{ true };
  std::vector<std::shared_ptr<LoggerNode>> spChildren_;
  std::vector<std::shared_ptr<Handler>> spHandlers_;
  mutable std::mutex mutex_;
};

// Logger - Lightweight wrapper providing access to LoggerNode
class Logger {
public:
  explicit Logger(std::shared_ptr<LoggerNode> node);
  ~Logger() = default;

  // Stream-style logging as member variables
  LogProxy debug;
  LogProxy info;
  LogProxy warning;
  LogProxy error;
  LogProxy critical;

  // Configuration - delegate to node
  void setLevel(Level level) { spNode_->setLevel(level); }
  Level getLevel() const { return spNode_->getLevel(); }

  void addHandler(std::shared_ptr<Handler> spHandler) { spNode_->addHandler(spHandler); }
  void addFileHandler(const std::string &filename, Level level = Level::DEBUG) { 
    spNode_->addFileHandler(filename, level); 
  }

  void setPropagate(bool propagate) { spNode_->setPropagate(propagate); }
  bool getPropagate() const { return spNode_->getPropagate(); }

  // Tree structure: Redirect this logger (and all its children) to another parent
  void redirectTo(const std::string &targetLoggerName);

  const std::string &getName() const { return spNode_->getName(); }
  std::string getFullName() const { return spNode_->getFullName(); }
  
  // Equality comparison based on underlying node
  bool operator==(const Logger& other) const { return spNode_ == other.spNode_; }
  bool operator!=(const Logger& other) const { return spNode_ != other.spNode_; }

private:
  friend class LogStream;
  friend class LogProxy;

  // Access to underlying node (for internal use)
  std::shared_ptr<LoggerNode> getNode() const { return spNode_; }  
  void log(Level level, const std::string &message) { spNode_->log(level, message); }

  std::shared_ptr<LoggerNode> spNode_;
};

// Template implementation for LogProxy (must be after LogStream definition)
template <typename T> LogStream LogProxy::operator<<(const T &value) {
  LogStream stream(logger_, level_);
  stream << value;
  return stream;
}

// Global logger management
Logger getLogger(const std::string &name);
Logger getRootLogger();

// Default level for all loggers
Level getLevel();
void setLevel(Level level);

} // namespace logging
} // namespace pp

#endif // PP_LEDGER_LOGGER_H
