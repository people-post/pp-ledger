#include "Logger.h"

#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <unordered_map>

namespace pp {
namespace logging {

// Helper function to trim leading dot from logger name
static std::string trimLeadingDot(const std::string& name) {
  if (!name.empty() && name[0] == '.') {
    return name.substr(1);
  }
  return name;
}

// Helper function to get current timestamp
static std::string getCurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;

  std::stringstream ss;
  ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
  ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
  return ss.str();
}

// ConsoleHandler implementation
void ConsoleHandler::emit(Level level, const std::string &loggerName,
                          const std::string &message) {
  if (level < level_) {
    return;
  }
  std::cout << message << std::endl;
}

// FileHandler implementation
FileHandler::FileHandler(const std::string &filename) : filename_(filename) {
  file_.open(filename_, std::ios::app);
  if (!file_.is_open()) {
    throw std::runtime_error("Failed to open log file: " + filename_);
  }
}

FileHandler::~FileHandler() {
  if (file_.is_open()) {
    file_.close();
  }
}

void FileHandler::emit(Level level, const std::string &loggerName,
                       const std::string &message) {
  if (level < level_) {
    return;
  }
  if (file_.is_open()) {
    file_ << message << std::endl;
    file_.flush();
  }
}

// LogProxy implementation
LogProxy::LogProxy(Logger *logger, Level level)
    : logger_(logger), level_(level) {}

// LogStream implementation
LogStream::LogStream(Logger *logger, Level level)
    : logger_(logger), level_(level), moved_(false) {}

LogStream::~LogStream() {
  if (!moved_ && logger_) {
    logger_->log(level_, stream_.str());
  }
}

LogStream::LogStream(LogStream &&other) noexcept
    : logger_(other.logger_), level_(other.level_),
      stream_(std::move(other.stream_)), moved_(false) {
  other.moved_ = true;
}

LogStream &LogStream::operator=(LogStream &&other) noexcept {
  if (this != &other) {
    logger_ = other.logger_;
    level_ = other.level_;
    stream_ = std::move(other.stream_);
    moved_ = false;
    other.moved_ = true;
  }
  return *this;
}

// ========== LoggerNode Implementation ==========

LoggerNode::LoggerNode(const std::string &name)
    : name_(name) {
}

std::string LoggerNode::getFullName() const {
  std::vector<std::string> parts;
  
  // Traverse to root, collecting names
  auto current = const_cast<LoggerNode*>(this)->shared_from_this();
  while (current && !current->getName().empty()) {
    parts.push_back(current->getName());
    current = current->getParent();
  }
  
  // Build full name from root to this node
  if (parts.empty()) {
    return "";
  }
  
  std::string fullName;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!fullName.empty()) {
      fullName += ".";
    }
    fullName += *it;
  }
  return fullName;
}

void LoggerNode::addHandler(std::shared_ptr<Handler> spHandler) {
  std::lock_guard<std::mutex> lock(mutex_);
  spHandlers_.push_back(spHandler);
}

void LoggerNode::addFileHandler(const std::string &filename, Level level) {
  auto spHandler = std::make_shared<FileHandler>(filename);
  spHandler->setLevel(level);
  addHandler(spHandler);
}

void LoggerNode::log(Level level, const std::string &message) {
  // Use this logger's full name as the originating name
  logWithOriginatingName(level, message, getFullName());
}

void LoggerNode::logWithOriginatingName(Level level, const std::string &message, const std::string &originatingLoggerName) {
  // Log to own handlers if level is sufficient
  if (level >= level_) {
    logToHandlersWithOriginatingName(level, message, originatingLoggerName);
  }

  // Propagate to parent if enabled, preserving the originating logger name
  if (propagate_) {
    auto parentNode = getParent();
    if (parentNode) {
      parentNode->logWithOriginatingName(level, message, originatingLoggerName);
    }
  }
}

void LoggerNode::logToHandlers(Level level, const std::string &message) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string formattedMessage = formatMessage(level, message);

  for (auto &spHandler : spHandlers_) {
    spHandler->emit(level, name_, formattedMessage);
  }
}

void LoggerNode::logToHandlersWithOriginatingName(Level level, const std::string &message, const std::string &originatingLoggerName) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string formattedMessage = formatMessage(level, message, originatingLoggerName);

  for (auto &spHandler : spHandlers_) {
    spHandler->emit(level, originatingLoggerName, formattedMessage);
  }
}

std::string LoggerNode::formatMessage(Level level, const std::string &message) {
  std::stringstream ss;
  ss << "[" << getCurrentTimestamp() << "] ";
  ss << "[" << levelToString(level) << "] ";
  std::string fullName = getFullName();
  if (!fullName.empty()) {
    ss << "[" << fullName << "] ";
  }
  ss << message;
  return ss.str();
}

std::string LoggerNode::formatMessage(Level level, const std::string &message, const std::string &originatingLoggerName) {
  std::stringstream ss;
  ss << "[" << getCurrentTimestamp() << "] ";
  ss << "[" << levelToString(level) << "] ";
  if (!originatingLoggerName.empty()) {
    ss << "[" << originatingLoggerName << "] ";
  }
  ss << message;
  return ss.str();
}

std::string LoggerNode::levelToString(Level level) {
  switch (level) {
  case Level::DEBUG:
    return "DEBUG";
  case Level::INFO:
    return "INFO";
  case Level::WARNING:
    return "WARNING";
  case Level::ERROR:
    return "ERROR";
  case Level::CRITICAL:
    return "CRITICAL";
  default:
    return "UNKNOWN";
  }
}

void LoggerNode::addChild(std::shared_ptr<LoggerNode> child) {
  std::lock_guard<std::mutex> lock(mutex_);
  spChildren_.push_back(child);
}

void LoggerNode::removeChild(LoggerNode* child) {
  std::lock_guard<std::mutex> lock(mutex_);
  spChildren_.erase(
    std::remove_if(spChildren_.begin(), spChildren_.end(),
      [child](const std::shared_ptr<LoggerNode>& ptr) {
        return ptr.get() == child;
      }),
    spChildren_.end()
  );
}

std::shared_ptr<LoggerNode> LoggerNode::getOrInitChild(const std::string& fullName) {
  std::string trimmedName = trimLeadingDot(fullName);
  if (trimmedName.empty()) {
    return shared_from_this();
  }

  auto firstDot = trimmedName.find('.');
  if (firstDot == std::string::npos) {
    return getOrInitDirectChild(trimmedName);
  } else {
    auto sp = getOrInitChild(trimmedName.substr(0, firstDot));
    return sp->getOrInitChild(trimmedName.substr(firstDot + 1));
  }
}

std::shared_ptr<LoggerNode> LoggerNode::getOrInitDirectChild(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  // Search for existing child with this name
  for (auto& child : spChildren_) {
    if (child->getName() == name) {
      return child;
    }
  }
  
  // Child doesn't exist, create a new one
  auto newChild = std::make_shared<LoggerNode>(name);
  newChild->setParent(shared_from_this());
  spChildren_.push_back(newChild);
  
  return newChild;
}

static std::shared_ptr<LoggerNode> initRootLogger() {
  auto root = std::make_shared<LoggerNode>("");
  root->addHandler(std::make_shared<ConsoleHandler>());
  return root;
}

static std::shared_ptr<LoggerNode> g_spRoot = initRootLogger();

// ========== Logger Implementation ==========

Logger::Logger(std::shared_ptr<LoggerNode> node)
    : spNode_(node),
      debug(this, Level::DEBUG),
      info(this, Level::INFO),
      warning(this, Level::WARNING),
      error(this, Level::ERROR),
      critical(this, Level::CRITICAL) {
}

void Logger::redirectTo(const std::string &targetLoggerName) {
  // Get the target logger - use global registry, not hierarchical child
  auto targetLogger = logging::getLogger(targetLoggerName);
  if (!targetLogger.getNode()) {
    throw std::invalid_argument("Cannot redirect to null logger");
  }
  
  if (targetLogger.getNode() == spNode_) {
    throw std::invalid_argument("Cannot redirect logger to itself");
  }
  
  auto targetNode = targetLogger.getNode();
  
  // If this is the root logger, only switch the wrapper (don't move the root node)
  if (spNode_ == g_spRoot) {
    spNode_ = targetNode;
    return;
  }
  
  // Check for circular redirection by checking if target is a descendant
  auto ancestor = targetNode;
  while (ancestor) {
    if (ancestor == spNode_) {
      throw std::invalid_argument("Cannot create circular parent relationship");
    }
    ancestor = ancestor->getParent();
  }
  
  // Remove from current parent's children list
  auto oldParent = spNode_->getParent();
  if (oldParent) {
    oldParent->removeChild(spNode_.get());
  }
  
  // Merge all children of current node to the target node
  // Make a copy of the children vector since we'll be modifying it
  auto children = spNode_->getChildren();
  for (auto& child : children) {
    child->setParent(targetNode);
    targetNode->addChild(child);
  }
  
  // Switch the wrapper to point to target node (dissolve current node)
  spNode_ = targetNode;
}

// ========== Global logger management ==========

Logger getLogger(const std::string &name) {
  auto spNode = g_spRoot->getOrInitChild(name);
  return Logger(spNode);
}

Logger getRootLogger() {
  return Logger(g_spRoot);
}

} // namespace logging
} // namespace pp
