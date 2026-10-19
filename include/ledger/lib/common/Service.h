#pragma once

#include "common/Module.h"
#include "Utilities.h"
#include <atomic>
#include <thread>

namespace pp {

/**
 * Service - Base class for components that run in a dedicated thread.
 *
 * Provides thread lifecycle management with start/stop functionality.
 * Derived classes implement the runLoop() method which executes in the service
 * thread or in the current thread when using run().
 */
class Service : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  /**
   * Constructor
   */
  Service() = default;

  /**
   * Virtual destructor - stops the service if running
   */
  ~Service() override;

  // Delete copy operations (inherited, but explicit for clarity)
  Service(const Service &) = delete;
  Service &operator=(const Service &) = delete;

  /** Stop requested: on this service, or by its stop source. */
  bool isStopSet() const {
    const std::atomic<bool> *source = stopSource_.load();
    return isStopSet_ || (source && source->load());
  }

  void setStop(bool value) { isStopSet_ = value; }
  /**
   * A flag owned by whoever runs this service (a RoleRunner, platform-integration pi2): set, it
   * stops the service as setStop(true) does — also before run() starts, which clears its own flag.
   * Setting it is one atomic store: safe from any thread and a signal handler. It must outlive
   * the service.
   */
  void setStopSource(const std::atomic<bool> *source) { stopSource_ = source; }

  Roe<void> run();
  Roe<void> start();
  void stop();

protected:
  /**
   * Main service loop - implement in derived classes.
   * Runs in the service thread or in the caller thread when using run().
   * Should check !isStopSet() periodically to allow graceful shutdown.
   */
  virtual void runLoop() = 0;

  /**
   * Called before the thread starts (in the calling thread context).
   * Override to perform pre-start initialization.
   * @return true if initialization succeeded, false to abort start
   */
  virtual Roe<void> onStart() { return {}; }

  /**
   * Called after the thread has stopped (in the calling thread context).
   * Override to perform cleanup after thread termination.
   */
  virtual void onStop() {}

private:
  /// Flag indicating if stop has been requested
  std::atomic<bool> isStopSet_{ false };
  /// See setStopSource
  std::atomic<const std::atomic<bool> *> stopSource_{ nullptr };

  /// The service thread
  std::thread thread_;
};

} // namespace pp
