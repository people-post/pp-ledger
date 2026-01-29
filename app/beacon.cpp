#include "../server/BeaconServer.h"
#include "../server/Beacon.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>

namespace {
std::atomic<bool> g_running{true};
std::mutex g_mutex;
std::condition_variable g_cv;

void signalHandler(int signal) {
  if (signal == SIGINT) {
    g_running = false;
    g_cv.notify_one();
  }
}
} // namespace

void printUsage() {
  std::cout << "Usage: pp-beacon -d <work-dir> [--init] [--debug]\n";
  std::cout << "  -d <work-dir>              - Work directory (required)\n";
  std::cout << "  --init                     - Initialize a new beacon\n";
  std::cout << "  --debug                    - Enable debug logging (default: warning level)\n";
  std::cout << "\n";
  std::cout << "Mode 1: Mount existing beacon:\n";
  std::cout << "  pp-beacon -d /some/path/to/work-dir [--debug]\n";
  std::cout << "  The work directory must contain:\n";
  std::cout << "    - config.json  - Beacon configuration file (required)\n";
  std::cout << "\n";
  std::cout << "Mode 2: Initialize new beacon:\n";
  std::cout << "  pp-beacon -d /some/path/to/work-dir --init [--debug]\n";
  std::cout << "\n";
  std::cout << "  When initializing, the command will:\n";
  std::cout << "  1. Create work-dir/init-config.json with default parameters if it doesn't exist\n";
  std::cout << "  2. Initialize the beacon using parameters from init-config.json\n";
  std::cout << "\n";
  std::cout << "  You can edit work-dir/init-config.json before initialization to customize:\n";
  std::cout << "  {\n";
  std::cout << "    \"slotDuration\": 5,           // Slot duration in seconds (default: 5)\n";
  std::cout << "    \"slotsPerEpoch\": 432         // Slots per epoch (default: 432)\n";
  std::cout << "  }\n";
  std::cout << "\n";
  std::cout << "The config.json file (for running beacon) should contain:\n";
  std::cout << "  {\n";
  std::cout << "    \"host\": \"localhost\",\n";
  std::cout << "    \"port\": 8517,\n";
  std::cout << "    \"beacons\": [\"host1:port1\", \"host2:port2\"],\n";
  std::cout << "    \"checkpointSize\": 1073741824, // Checkpoint size in bytes (default: 1GB)\n";
  std::cout << "    \"checkpointAge\": 31536000    // Checkpoint age in seconds (default: 1 year)\n";
  std::cout << "  }\n";
  std::cout << "\n";
  std::cout << "Note: The 'host' and 'port' fields are optional.\n";
  std::cout << "      Defaults: host=\"localhost\", port=8517\n";
  std::cout << "      The 'beacons' array can be empty.\n";
  std::cout << "      Checkpoint fields are optional and will use defaults if not specified.\n";
}

int initBeacon(const std::string& workDir) {
  pp::BeaconServer beaconServer;
  beaconServer.redirectLogger("pp.BeaconServer");
  
  auto result = beaconServer.init(workDir);
  if (!result) {
    std::cerr << "Error: Failed to initialize beacon: " << result.error().message << "\n";
    return 1;
  }
  
  std::cout << "Beacon initialized successfully (to reinitialize, edit the init config file and run the same command)\n";
  std::cout << "You can now start the beacon with: pp-beacon -d " << workDir << "\n";
  return 0;
}

int runBeacon(const std::string& workDir) {
  auto logger = pp::logging::getLogger("pp");
  
  logger.info << "Starting beacon with work directory: " << workDir;

  pp::BeaconServer beacon;
  beacon.redirectLogger("pp.B");

  auto startResult = beacon.start(workDir);
  if (!startResult) {
    logger.error << "Failed to start beacon: " + startResult.error().message;
    std::cerr << "Error: Failed to start beacon: " + startResult.error().message << "\n";
    return 1;
  }

  logger.info << "Beacon started successfully";
  logger.info << "Beacon running";
  logger.info << "Work directory: " << workDir;
  logger.info << "Press Ctrl+C to stop the beacon...";

  // Wait for SIGINT
  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait(lock, [] { return !g_running.load(); });

  beacon.stop();
  logger.info << "Beacon stopped";
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << "Error: Work directory required.\n";
    printUsage();
    return 1;
  }

  std::string workDir;
  bool initMode = false;
  bool debugMode = false;

  // Parse arguments
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-d") == 0) {
      if (i + 1 < argc) {
        workDir = argv[++i];
      } else {
        std::cerr << "Error: -d option requires a directory path.\n";
        printUsage();
        return 1;
      }
    } else if (strcmp(argv[i], "--init") == 0) {
      initMode = true;
    } else if (strcmp(argv[i], "--debug") == 0) {
      debugMode = true;
    } else {
      std::cerr << "Error: Unknown argument: " << argv[i] << "\n";
      printUsage();
      return 1;
    }
  }

  if (workDir.empty()) {
    std::cerr << "Error: Work directory (-d) is required.\n";
    printUsage();
    return 1;
  }

  auto logger = pp::logging::getRootLogger();
  pp::logging::Level logLevel = debugMode ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING;
  logger.setLevel(logLevel);
  logger.info << "Logging level set to " << (debugMode ? "DEBUG" : "WARNING");

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  if (initMode) {
    return initBeacon(workDir);
  } else {
    return runBeacon(workDir);
  }
}
