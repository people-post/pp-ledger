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

static std::mutex& getRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

// LoggerNode registry (forward declaration for use in Logger methods)
static std::unordered_map<std::string, std::shared_ptr<LoggerNode>>& getLoggerRegistry() {
  static std::unordered_map<std::string, std::shared_ptr<LoggerNode>> registry;
  return registry;
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
    : name_(name), level_(Level::DEBUG), propagate_(true) {
  // Add default console handler
  addHandler(std::make_shared<ConsoleHandler>());
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
  // Log to own handlers if level is sufficient
  if (level >= level_) {
    logToHandlers(level, message);
  }

  // Propagate to parent if enabled
  if (propagate_) {
    auto parentNode = getParent();
    if (parentNode) {
      parentNode->log(level, message);
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
  children_.push_back(child);
}

void LoggerNode::removeChild(LoggerNode* child) {
  std::lock_guard<std::mutex> lock(mutex_);
  children_.erase(
    std::remove_if(children_.begin(), children_.end(),
      [child](const std::weak_ptr<LoggerNode>& weak) {
        auto ptr = weak.lock();
        return !ptr || ptr.get() == child;
      }),
    children_.end()
  );
}

// ========== Logger Implementation ==========

Logger::Logger(std::shared_ptr<LoggerNode> node)
    : node_(node),
      debug(this, Level::DEBUG),
      info(this, Level::INFO),
      warning(this, Level::WARNING),
      error(this, Level::ERROR),
      critical(this, Level::CRITICAL) {
}

Logger Logger::getParent() const {
  auto parentNode = node_->getParent();
  if (parentNode) {
    return Logger(parentNode);
  }
  return Logger(nullptr);
}

std::vector<Logger> Logger::getChildren() const {
  std::vector<Logger> result;
  const auto& children = node_->getChildren();
  for (const auto& weakChild : children) {
    auto childNode = weakChild.lock();
    if (childNode) {
      result.push_back(Logger(childNode));
    }
  }
  return result;
}

void Logger::redirectTo(const std::string &targetLoggerName) {
  // Get the target logger - use global registry, not hierarchical child
  auto targetLogger = logging::getLogger(targetLoggerName);
  redirectTo(targetLogger);
}

void Logger::switchTo(const std::string &targetLoggerName) {
  // Switch this Logger wrapper to point to a different logger node
  auto targetLogger = logging::getLogger(trimLeadingDot(targetLoggerName));
  node_ = targetLogger.getNode();
}

void Logger::redirectTo(Logger targetLogger) {
  if (!targetLogger.getNode()) {
    throw std::invalid_argument("Cannot redirect to null logger");
  }
  
  if (targetLogger.getNode() == node_) {
    throw std::invalid_argument("Cannot redirect logger to itself");
  }
  
  auto targetNode = targetLogger.getNode();
  
  // Check for circular redirection by checking if target is a descendant
  auto ancestor = targetNode;
  while (ancestor) {
    if (ancestor == node_) {
      throw std::invalid_argument("Cannot create circular parent relationship");
    }
    ancestor = ancestor->getParent();
  }
  
  // Remove from current parent's children list
  auto oldParent = node_->getParent();
  if (oldParent) {
    oldParent->removeChild(node_.get());
  }
  
  // Set new parent and add to new parent's children
  node_->setParent(targetNode);
  targetNode->addChild(node_);
}

// ========== Global logger management ==========

Logger getLogger(const std::string &name) {
  std::string trimmedName = trimLeadingDot(name);
  std::lock_guard<std::mutex> lock(getRegistryMutex());
  auto& registry = getLoggerRegistry();

  // Check if node already exists
  auto it = registry.find(trimmedName);
  if (it != registry.end()) {
    return Logger(it->second);
  }

  // Parse the hierarchical name to extract node name and parent path
  std::string nodeName = trimmedName;
  std::string parentPath;
  auto lastDot = trimmedName.rfind('.');
  if (lastDot != std::string::npos) {
    parentPath = trimmedName.substr(0, lastDot);
    nodeName = trimmedName.substr(lastDot + 1);
  }

  // Create new node with just the node name (not full path)
  auto node = std::make_shared<LoggerNode>(nodeName);
  registry[trimmedName] = node;  // Registry uses full path as key

  // Establish parent-child relationship for hierarchical loggers
  if (!trimmedName.empty()) {
    if (lastDot != std::string::npos) {
      // Has a parent in the hierarchy
      // Create parent if it doesn't exist (releases lock temporarily via recursion)
      getRegistryMutex().unlock();
      auto parentLogger = getLogger(parentPath);
      getRegistryMutex().lock();
      auto parentNode = parentLogger.getNode();
      
      // Establish parent-child relationship
      node->setParent(parentNode);
      parentNode->addChild(node);
    } else {
      // Top-level logger, attach to root
      auto rootIt = registry.find("");
      if (rootIt != registry.end()) {
        auto rootNode = rootIt->second;
        node->setParent(rootNode);
        rootNode->addChild(node);
      }
    }
  }

  return Logger(node);
}

Logger getRootLogger() {
  return getLogger("");
}

} // namespace logging
} // namespace pp
