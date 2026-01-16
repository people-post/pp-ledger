#pragma once

#include "Module.h"
#include <atomic>
#include <thread>

namespace pp {

/**
 * Service - Base class for components that run in a dedicated thread.
 *
 * Provides thread lifecycle management with start/stop functionality.
 * Derived classes implement the run() method which executes in the service
 * thread.
 */
class Service : public Module {
public:
  /**
   * Constructor
   * @param name Hierarchical name for the service's logger
   */
  explicit Service(const std::string &name);

  /**
   * Virtual destructor - stops the service if running
   */
  ~Service() override;

  // Delete copy operations (inherited, but explicit for clarity)
  Service(const Service &) = delete;
  Service &operator=(const Service &) = delete;

  /**
   * Start the service thread
   * @return true if service started successfully, false if already running
   */
  bool start();

  /**
   * Stop the service thread
   * Blocks until the thread has finished.
   */
  void stop();

  /**
   * Check if the service is running
   * @return true if the service thread is active
   */
  bool isRunning() const { return running_; }

protected:
  /**
   * Main service loop - implement in derived classes.
   * This method runs in the service thread and should check isRunning()
   * periodically to allow graceful shutdown.
   */
  virtual void run() = 0;

  /**
   * Called before the thread starts (in the calling thread context).
   * Override to perform pre-start initialization.
   * @return true if initialization succeeded, false to abort start
   */
  virtual bool onStart() { return true; }

  /**
   * Called after the thread has stopped (in the calling thread context).
   * Override to perform cleanup after thread termination.
   */
  virtual void onStop() {}

private:
  /// Flag indicating if the service should continue running
  std::atomic<bool> running_;

  /// The service thread
  std::thread thread_;
};

} // namespace pp
