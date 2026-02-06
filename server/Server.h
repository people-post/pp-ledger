#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "../lib/Service.h"
#include <string>

namespace pp {

/**
 * Server - Base class for RelayServer, MinerServer, and BeaconServer.
 *
 * Provides common run(workDir) behavior: work directory setup, optional
 * signature file for directory recognition, log file handler, then
 * Service::run() (onStart + runLoop).
 */
class Server : public Service {
public:
  Server() = default;
  ~Server() override = default;

  virtual Service::Roe<void> run(const std::string& workDir);

protected:
  /** Current work directory set by run(workDir). */
  const std::string& getWorkDir() const { return workDir_; }

  /** If true, run() creates/checks .signature in work dir. Default true; BeaconServer uses false. */
  virtual bool useSignatureFile() const { return true; }

  /** File name for signature file (e.g. ".signature"). Used when useSignatureFile() is true. */
  virtual const char* getFileSignature() const = 0;

  /** Log file name (e.g. "relay.log", "miner.log", "beacon.log"). */
  virtual const char* getFileLog() const = 0;

  /** Name used in run() log message, e.g. "RelayServer", "MinerServer", "BeaconServer". */
  virtual const char* getServerName() const = 0;

  /** Error code for run() failures (e.g. signature file). Derived can override. */
  virtual int32_t getRunErrorCode() const { return -1; }

private:
  std::string workDir_;
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
