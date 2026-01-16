#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace pp {
namespace logging {

// Global logger registry
static std::map<std::string, std::shared_ptr<Logger>> loggerRegistry;
static std::mutex registryMutex;

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

// Logger implementation
Logger::Logger(const std::string &name)
    : name_(name), level_(Level::DEBUG), debug(this, Level::DEBUG),
      info(this, Level::INFO), warning(this, Level::WARNING),
      error(this, Level::ERROR), critical(this, Level::CRITICAL) {
  // Add default console handler
  addHandler(std::make_shared<ConsoleHandler>());
}

void Logger::log(Level level, const std::string &message) {
  // Check for redirect first (before checking level)
  if (!redirectTarget_.empty()) {
    Logger &targetLogger = getLogger(redirectTarget_);
    targetLogger.log(level, message);
    return;
  }

  if (level < level_) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  std::string formattedMessage = formatMessage(level, message);

  for (auto &spHandler : spHandlers_) {
    spHandler->emit(level, name_, formattedMessage);
  }
}

std::string Logger::formatMessage(Level level, const std::string &message) {
  std::stringstream ss;
  ss << "[" << getCurrentTimestamp() << "] ";
  ss << "[" << levelToString(level) << "] ";
  if (!name_.empty()) {
    ss << "[" << name_ << "] ";
  }
  ss << message;
  return ss.str();
}

std::string Logger::levelToString(Level level) {
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

void Logger::addHandler(std::shared_ptr<Handler> spHandler) {
  std::lock_guard<std::mutex> lock(mutex_);
  spHandlers_.push_back(spHandler);
}

void Logger::addFileHandler(const std::string &filename, Level level) {
  auto spHandler = std::make_shared<FileHandler>(filename);
  spHandler->setLevel(level);
  addHandler(spHandler);
}

void Logger::redirectTo(const std::string &targetLoggerName) {
  std::lock_guard<std::mutex> lock(mutex_);
  redirectTarget_ = targetLoggerName;
}

void Logger::clearRedirect() {
  std::lock_guard<std::mutex> lock(mutex_);
  redirectTarget_.clear();
}

// Global logger management functions
Logger &getLogger(const std::string &name) {
  std::lock_guard<std::mutex> lock(registryMutex);

  // Check if logger already exists
  auto it = loggerRegistry.find(name);
  if (it != loggerRegistry.end()) {
    return *(it->second);
  }

  // Create new logger
  auto logger = std::make_shared<Logger>(name);
  loggerRegistry[name] = logger;

  // If this is a child logger (contains dots), we could inherit settings
  // from parent logger, but for now we just create independent loggers

  return *logger;
}

Logger &getRootLogger() { return getLogger(""); }

} // namespace logging
} // namespace pp
