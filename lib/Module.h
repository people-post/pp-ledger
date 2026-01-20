#pragma once

#include "Logger.h"
#include <string>

namespace pp {

/**
 * Base class for modules that need logging functionality.
 * Provides a common interface for logger management across components.
 */
class Module {
public:
  /**
   * Constructor
   * Creates a module with a default logger. Use redirectLogger() to configure
   * the logger target.
   */
  Module();

  /**
   * Virtual destructor for proper cleanup
   */
  virtual ~Module() = default;

  // Delete copy operations
  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;

  /**
   * Set the logger for this module
   * @param name Name for the module's logger
   */
  void setLogger(const std::string &name);

  /**
   * Redirect this module's logger to another logger
   * @param targetLoggerName Name of the target logger
   */
  void redirectLogger(const std::string &targetLoggerName);

  /**
   * Clear logger redirect
   */
  void clearLoggerRedirect();

  /**
   * Get the logger instance for this module.
   * Use this to access the logger in derived classes and externally.
   *
   * @return Reference to the logger instance
   */
  logging::Logger &log() const;

private:
  std::shared_ptr<logging::Logger> spLogger_;
};

} // namespace pp
