#include "../server/BeaconServer.h"
#include "../server/Beacon.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
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
  std::cout << "Usage: pp-beacon -d <work-dir> [--init] [-c <init-config.json>]\n";
  std::cout << "  -d <work-dir>              - Work directory (required)\n";
  std::cout << "  --init                     - Initialize a new beacon\n";
  std::cout << "  -c <init-config.json>      - Init config file (optional, used with --init)\n";
  std::cout << "\n";
  std::cout << "Mode 1: Mount existing beacon:\n";
  std::cout << "  pp-beacon -d /some/path/to/work-dir\n";
  std::cout << "  The work directory must contain:\n";
  std::cout << "    - config.json  - Beacon configuration file (required)\n";
  std::cout << "\n";
  std::cout << "Mode 2: Initialize new beacon:\n";
  std::cout << "  pp-beacon -d /some/path/to/work-dir --init\n";
  std::cout << "  pp-beacon -d /some/path/to/work-dir --init -c /path/to/init-config.json\n";
  std::cout << "\n";
  std::cout << "The init-config.json file should contain:\n";
  std::cout << "  {\n";
  std::cout << "    \"slotDuration\": 5,           // Slot duration in seconds (default: 5)\n";
  std::cout << "    \"slotsPerEpoch\": 432,        // Slots per epoch (default: 432)\n";
  std::cout << "    \"checkpointSize\": 1073741824, // Checkpoint size in bytes (default: 1GB)\n";
  std::cout << "    \"checkpointAge\": 31536000    // Checkpoint age in seconds (default: 1 year)\n";
  std::cout << "  }\n";
  std::cout << "\n";
  std::cout << "Note: All fields in init-config.json are optional and will use defaults if not specified.\n";
  std::cout << "      If -c is not provided with --init, all defaults will be used.\n";
  std::cout << "\n";
  std::cout << "The config.json file (for running beacon) should contain:\n";
  std::cout << "  {\n";
  std::cout << "    \"host\": \"localhost\",\n";
  std::cout << "    \"port\": 8517,\n";
  std::cout << "    \"beacons\": [\"host1:port1\", \"host2:port2\"]\n";
  std::cout << "  }\n";
  std::cout << "\n";
  std::cout << "Note: The 'host' and 'port' fields are optional.\n";
  std::cout << "      Defaults: host=\"localhost\", port=8517\n";
  std::cout << "      The 'beacons' array can be empty.\n";
}

int main(int argc, char *argv[]) {
  auto rootLogger = pp::logging::getRootLogger();
  rootLogger->info << "PP-Ledger Beacon Server";

  if (argc < 3) {
    std::cerr << "Error: Work directory required.\n";
    printUsage();
    return 1;
  }

  std::string workDir;
  std::string initConfigPath;
  bool initMode = false;

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
    } else if (strcmp(argv[i], "-c") == 0) {
      if (i + 1 < argc) {
        initConfigPath = argv[++i];
      } else {
        std::cerr << "Error: -c option requires a config file path.\n";
        printUsage();
        return 1;
      }
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

  // Validate that -c is only used with --init
  if (!initConfigPath.empty() && !initMode) {
    std::cerr << "Error: -c option can only be used with --init.\n";
    printUsage();
    return 1;
  }

  auto logger = pp::logging::getLogger("beacon");
  logger->setLevel(pp::logging::Level::INFO);

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  if (initMode) {
    logger->info << "Initializing new beacon with work directory: " << workDir;
    
    // Default configuration values
    uint64_t slotDuration = 5;
    uint64_t slotsPerEpoch = 432;
    uint64_t checkpointSize = 1024ULL * 1024 * 1024;
    uint64_t checkpointAge = 365ULL * 24 * 3600;
    
    // Load configuration from file if provided
    if (!initConfigPath.empty()) {
      logger->info << "Using init config file: " << initConfigPath;
      
      auto jsonResult = pp::utl::loadJsonFile(initConfigPath);
      if (!jsonResult) {
        logger->error << "Failed to load init config file: " << jsonResult.error().message;
        std::cerr << "Error: Failed to load init config file: " << jsonResult.error().message << "\n";
        return 1;
      }
      
      nlohmann::json config = jsonResult.value();
      
      // Extract configuration with defaults
      slotDuration = config.value("slotDuration", slotDuration);
      slotsPerEpoch = config.value("slotsPerEpoch", slotsPerEpoch);
      checkpointSize = config.value("checkpointSize", checkpointSize);
      checkpointAge = config.value("checkpointAge", checkpointAge);
    } else {
      logger->info << "Using default configuration values";
    }
    
    pp::Beacon beacon;
    beacon.redirectLogger("pp.Beacon");
    
    // Prepare init configuration
    pp::Beacon::InitConfig initConfig;
    initConfig.workDir = workDir;
    initConfig.chain.slotDuration = slotDuration;
    initConfig.chain.slotsPerEpoch = slotsPerEpoch;
    initConfig.checkpoint.minSizeBytes = checkpointSize;
    initConfig.checkpoint.ageSeconds = checkpointAge;
    
    logger->info << "Configuration:";
    logger->info << "  Slot duration: " << slotDuration << " seconds";
    logger->info << "  Slots per epoch: " << slotsPerEpoch;
    logger->info << "  Checkpoint size threshold: " << checkpointSize << " bytes";
    logger->info << "  Checkpoint age threshold: " << checkpointAge << " seconds";
    
    auto result = beacon.init(initConfig);
    if (!result) {
      logger->error << "Failed to initialize beacon: " << result.error().message;
      std::cerr << "Error: Failed to initialize beacon: " << result.error().message << "\n";
      return 1;
    }
    
    logger->info << "Beacon initialized successfully";
    std::cout << "Beacon initialized successfully\n";
    std::cout << "Work directory: " << workDir << "\n";
    std::cout << "You can now start the beacon with: pp-beacon -d " << workDir << "\n";
    return 0;
  }

  logger->info << "Starting beacon with work directory: " << workDir;

  pp::BeaconServer beacon;
  beacon.redirectLogger("pp.Server");

  if (beacon.start(workDir)) {
    logger->info << "Beacon started successfully";
    std::cout << "Beacon running\n";
    std::cout << "Work directory: " << workDir << "\n";
    std::cout << "Press Ctrl+C to stop the beacon...\n";

    // Wait for SIGINT
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [] { return !g_running.load(); });

    beacon.stop();
    logger->info << "Beacon stopped";
    return 0;
  } else {
    logger->error << "Failed to start beacon";
    std::cerr << "Error: Failed to start beacon\n";
    return 1;
  }
}
