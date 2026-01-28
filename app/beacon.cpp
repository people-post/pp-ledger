#include "../server/BeaconServer.h"
#include "../server/Beacon.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
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
  auto logger = pp::logging::getLogger("beacon");
  logger.info << "Initializing new beacon with work directory: " << workDir;
  
  std::filesystem::path workDirPath(workDir);
  std::filesystem::path initConfigPath = workDirPath / "init-config.json";
  
  // Default configuration values
  uint64_t slotDuration = 5;
  uint64_t slotsPerEpoch = 432;
  
  // Create work directory if it doesn't exist (to write init-config.json)
  if (!std::filesystem::exists(workDirPath)) {
    std::filesystem::create_directories(workDirPath);
    logger.info << "Created work directory: " << workDir;
  }
  
  // Create or load init-config.json
  if (!std::filesystem::exists(initConfigPath)) {
    logger.info << "Creating init-config.json with default parameters";
    
    nlohmann::json defaultConfig;
    defaultConfig["slotDuration"] = slotDuration;
    defaultConfig["slotsPerEpoch"] = slotsPerEpoch;
    
    std::ofstream configFile(initConfigPath);
    if (!configFile) {
      logger.error << "Failed to create init-config.json";
      std::cerr << "Error: Failed to create init-config.json\n";
      return 1;
    }
    configFile << defaultConfig.dump(2) << std::endl;
    configFile.close();
    
    logger.info << "Created: " << initConfigPath.string();
    std::cout << "Created init-config.json with default parameters at: " << initConfigPath.string() << "\n";
  } else {
    logger.info << "Found existing init-config.json";
  }
  
  // Load configuration from init-config.json
  logger.info << "Loading configuration from: " << initConfigPath.string();
  
  auto jsonResult = pp::utl::loadJsonFile(initConfigPath.string());
  if (!jsonResult) {
    logger.error << "Failed to load init config file: " << jsonResult.error().message;
    std::cerr << "Error: Failed to load init config file: " << jsonResult.error().message << "\n";
    return 1;
  }
  
  nlohmann::json config = jsonResult.value();
  
  // Extract configuration with defaults
  slotDuration = config.value("slotDuration", slotDuration);
  slotsPerEpoch = config.value("slotsPerEpoch", slotsPerEpoch);
  
  pp::BeaconServer beaconServer;
  beaconServer.redirectLogger("pp.BeaconServer");
  
  // Prepare init configuration
  pp::Beacon::InitConfig initConfig;
  initConfig.workDir = workDir;
  initConfig.slotDuration = slotDuration;
  initConfig.slotsPerEpoch = slotsPerEpoch;
  
  logger.info << "Configuration:";
  logger.info << "  Slot duration: " << slotDuration << " seconds";
  logger.info << "  Slots per epoch: " << slotsPerEpoch;
  
  auto result = beaconServer.init(initConfig);
  if (!result) {
    logger.error << "Failed to initialize beacon: " << result.error().message;
    std::cerr << "Error: Failed to initialize beacon: " << result.error().message << "\n";
    return 1;
  }
  
  logger.info << "Beacon initialized successfully";
  std::cout << "Beacon initialized successfully\n";
  std::cout << "Work directory: " << workDir << "\n";
  std::cout << "Configuration: " << initConfigPath.string() << "\n";
  std::cout << "You can now start the beacon with: pp-beacon -d " << workDir << "\n";
  return 0;
}

int runBeacon(const std::string& workDir) {
  auto logger = pp::logging::getLogger("pp");
  
  logger.info << "Starting beacon with work directory: " << workDir;

  pp::BeaconServer beacon;
  beacon.redirectLogger("pp.B");

  if (beacon.start(workDir)) {
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
  } else {
    logger.error << "Failed to start beacon";
    std::cerr << "Error: Failed to start beacon\n";
    return 1;
  }
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
