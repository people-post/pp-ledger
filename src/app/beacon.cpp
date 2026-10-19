#include "ledger/server/BeaconServer.h"
#include "ledger/server/Beacon.h"
#include "ledger/server/RoleRunner.h"
#include "common/Logger.h"
#include "ledger/lib/common/Utilities.h"
#include "common/io/Json.h"

#include "NodeOptions.h"

#include <cli11.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
/** Set while the beacon runs: a signal stops it (one atomic store). */
std::atomic<pp::RoleRunner *> g_runner{nullptr};

void signalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    if (pp::RoleRunner *runner = g_runner.load()) {
      runner->stop();
    }
  }
}
} // namespace

int initBeacon(const std::string& workDir, const std::vector<std::string>& genesisKeys, bool force,
               const std::string& importState) {
  pp::BeaconServer beaconServer;
  beaconServer.redirectLogger("pp.BeaconServer");

  auto result = beaconServer.init(workDir, genesisKeys, force, importState);
  if (!result) {
    std::cerr << "Error: Failed to initialize beacon: " << result.error().message << "\n";
    return 1;
  }

  std::cout << "Beacon initialized (genesis signed with " << genesisKeys.size() << " genesis key(s)).\n"
            << "Keep the genesis keys offline: they are the chain's admin keys.\n"
            << "You can now start the beacon with: pp-beacon -d " << workDir << "\n";
  return 0;
}

int initBeaconSigned(const std::string& workDir, const std::string& operationPath, bool force) {
  pp::BeaconServer beaconServer;
  beaconServer.redirectLogger("pp.BeaconServer");
  if (auto result = beaconServer.initSigned(workDir, operationPath, force); !result) {
    std::cerr << "Error: Failed to initialize beacon: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Beacon initialized from the signed genesis " << operationPath << ".\n"
            << "You can now start the beacon with: pp-beacon -d " << workDir << "\n";
  return 0;
}

/** `start`: Unix seconds, or +N seconds from now; empty: now. */
pp::Roe<int64_t> parseStart(const std::string& start) {
  const int64_t now = pp::utl::getCurrentTime();
  if (start.empty()) {
    return now;
  }
  try {
    size_t used = 0;
    const bool relative = start.front() == '+';
    const long long value = std::stoll(relative ? start.substr(1) : start, &used);
    if (used != start.size() - (relative ? 1 : 0) || value < 0) {
      throw std::invalid_argument(start);
    }
    return relative ? now + value : static_cast<int64_t>(value);
  } catch (const std::exception&) {
    return pp::Error("--start: Unix seconds, or +N seconds from now: " + start);
  }
}

int planGenesis(const std::string& workDir, const std::string& outputPath, const std::string& start,
                const std::string& notes) {
  auto startTime = parseStart(start);
  if (!startTime) {
    std::cerr << "Error: " << startTime.error().message << "\n";
    return 1;
  }
  pp::BeaconServer beaconServer;
  beaconServer.redirectLogger("pp.BeaconServer");
  if (auto result = beaconServer.planGenesis(workDir, outputPath, startTime.value(), notes); !result) {
    std::cerr << "Error: Failed to plan the genesis: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Genesis plan written to " << outputPath << "; the network starts at "
            << pp::utl::formatTimestampLocal(startTime.value()) << ".\n"
            << "Each admin key holder signs a copy (pp-client op sign); merge them (pp-client op merge), then:\n"
            << "  pp-beacon -d " << workDir << " --init --genesis-op <signed file>\n";
  return 0;
}

int runBeacon(const std::string& workDir, uint64_t approvedRewindGeneration, bool promote, bool acceptLedgerTip,
              const std::string& logFile) {
  pp::RoleRunner::Options run;
  run.role = pp::RoleRunner::Role::Beacon;
  run.workDir = workDir;
  run.approvedRewindGeneration = approvedRewindGeneration;
  run.logFile = logFile;
  run.promote = promote;
  run.acceptLedgerTip = acceptLedgerTip;
  pp::RoleRunner runner(std::move(run));
  runner.redirectLogger("pp");
  g_runner = &runner;
  auto ran = runner.run();
  g_runner = nullptr;
  if (!ran) {
    pp::logging::getLogger("pp").error << ran.error().message;
    std::cerr << "Error: " << ran.error().message << "\n";
    return 1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-beacon - Beacon server for pp-ledger"};
  
  pp::app::NodeOptions options;
  options.add(app);

  bool initMode = false;
  app.add_flag("--init", initMode, "Initialize a new beacon (genesis) from init-config.json");

  std::vector<std::string> genesisKeys;
  app.add_option("--genesis-key", genesisKeys,
                 "Genesis private key file to sign the genesis block (repeat for M-of-N); --init only")
      ->envname("PP_GENESIS_KEY_FILES")
      ->delimiter(',');

  std::string genesisOp;
  app.add_option("--genesis-op", genesisOp,
                 "--init only: write the genesis signed offline in this operation file (--plan-genesis, then "
                 "pp-client op sign / merge) instead of signing with --genesis-key");

  std::string planGenesisOut;
  app.add_option("--plan-genesis", planGenesisOut,
                 "Write the genesis init-config.json and this beacon's key describe, unsigned, to this operation "
                 "file, for the admin keys to sign offline (docs/contracts/OPERATION_FILE.md)");
  std::string planStart;
  app.add_option("--start", planStart,
                 "--plan-genesis only: when the network starts — Unix seconds, or +N seconds from now (default: "
                 "now). Leave the signers time: nodes wait at slot 0 until then");
  std::string planNotes;
  app.add_option("--notes", planNotes, "--plan-genesis only: a note for the signers");

  bool forceInit = false;
  app.add_flag("--force", forceInit, "--init only: replace an existing chain (data/) — destroys it");

  std::string importState;
  app.add_option("--import-state", importState,
                 "--init only: a re-genesis importing this state snapshot (pp-client export-state): a new network "
                 "whose accounts are the old one's at the snapshot's height (docs/contracts/BEACON_AUTHORITY.md)");


  bool promote = false;
  app.add_flag("--promote", promote,
               "Turn this relay's work directory into the beacon taking over (beacon-authority case 2): the "
               "admins' certificate held here must name keys/beacon-signing.pub and carry a cutover");

  bool acceptLedgerTip = false;
  app.add_flag("--accept-ledger-tip", acceptLedgerTip,
               "No data/signed-tip.dat (a directory moved without it): sign the ledger's last block as the "
               "tip anyway. Only when this ledger certainly holds every block this key signed "
               "(docs/ops/MOVING_THE_BEACON.md)");

  uint64_t approvedRewindGeneration = 0;
  app.add_option("--approve-rewind", approvedRewindGeneration,
                 "Apply the rewind ordered by this beacon generation although it reaches back further "
                 "than a node rewinds on its own (7 days of slots); check the certificate first");

  app.footer(
    "Initialize (once):\n"
    "  pp-beacon -d <dir> --init --genesis-key genesis.key [--genesis-key ...]\n"
    "  init-config.json sets networkId and the system accounts' public keys\n"
    "  (pp-client keygen -o <name>); a missing file is created as a template.\n"
    "Or with the genesis keys kept offline:\n"
    "  pp-beacon -d <dir> --plan-genesis plan.json [--start +3600]\n"
    "  (each admin: pp-client op sign; then pp-client op merge ... -o signed.json)\n"
    "  pp-beacon -d <dir> --init --genesis-op signed.json\n"
    "\n"
    "Run:\n"
    "  pp-beacon -d <dir> [--debug]\n"
    "\n"
    "Take over (a relay's directory becomes the new beacon; once):\n"
    "  pp-beacon -d <relay dir> --promote\n"
    "\n"
    "Move to another host: docs/ops/MOVING_THE_BEACON.md\n"
    "\n"
    "Every field: docs/ops/CONFIGURATION.md"
  );

  CLI11_PARSE(app, argc, argv);

  options.applyLogLevel();

  // Ctrl+C, and the SIGTERM a service manager (docker, systemd) stops with: stop cleanly, so the
  // ledger commits what it appended.
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  if (!importState.empty() && !initMode) {
    std::cerr << "Error: --import-state goes with --init\n";
    return 1;
  }
  if (initMode && promote) {
    std::cerr << "Error: --init and --promote do not go together\n";
    return 1;
  }
  if (!genesisOp.empty() && (!initMode || !genesisKeys.empty() || !importState.empty())) {
    std::cerr << "Error: --genesis-op goes with --init, without --genesis-key or --import-state\n";
    return 1;
  }
  if ((!planStart.empty() || !planNotes.empty()) && planGenesisOut.empty()) {
    std::cerr << "Error: --start and --notes go with --plan-genesis\n";
    return 1;
  }
  if (!planGenesisOut.empty()) {
    if (initMode || promote) {
      std::cerr << "Error: --plan-genesis goes alone (then --init --genesis-op with the signed plan)\n";
      return 1;
    }
    return planGenesis(options.workDir, planGenesisOut, planStart, planNotes);
  }
  if (initMode && !genesisOp.empty()) {
    return initBeaconSigned(options.workDir, genesisOp, forceInit);
  }
  if (initMode) {
    return initBeacon(options.workDir, genesisKeys, forceInit, importState);
  } else {
    return runBeacon(options.workDir, approvedRewindGeneration, promote, acceptLedgerTip, options.logFile);
  }
}
