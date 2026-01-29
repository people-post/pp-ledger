#include "../server/MinerServer.h"
#include "../lib/Logger.h"

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
  std::cout << "Usage: pp-miner -d <work-dir> [--debug]\n";
  std::cout << "  -d <work-dir>  - Work directory (required)\n";
  std::cout << "  --debug        - Enable debug logging (default: warning level)\n";
  std::cout << "\n";
  std::cout << "Example:\n";
  std::cout << "  pp-miner -d /some/path/to/work-dir [--debug]\n";
  std::cout << "\n";
  std::cout << "The miner will automatically create a default config.json file if it doesn't exist.\n";
  std::cout << "You can edit the config.json file to customize the miner settings:\n";
  std::cout << "  {\n";
  std::cout << "    \"minerId\": \"miner1\",\n";
  std::cout << "    \"stake\": 1000000,\n";
  std::cout << "    \"host\": \"localhost\",\n";
  std::cout << "    \"port\": 8518,\n";
  std::cout << "    \"beacons\": [\"127.0.0.1:8517\"]\n";
  std::cout << "  }\n";
  std::cout << "\n";
  std::cout << "Note: The 'host' and 'port' fields are optional.\n";
  std::cout << "      Defaults: host=\"localhost\", port=8518\n";
  std::cout << "      The 'beacons' array must contain at least one beacon address.\n";
}

int runMiner(const std::string& workDir) {
  auto logger = pp::logging::getLogger("pp");
  
  logger.info << "Starting miner with work directory: " << workDir;

  pp::MinerServer miner;
  miner.redirectLogger("pp.M");
  auto startResult = miner.start(workDir);
  if (!startResult) {
    logger.error << "Failed to start miner: " + startResult.error().message;
    return 1;
  }
  logger.info << "Miner started successfully";
  logger.info << "Miner running";
  logger.info << "Work directory: " << workDir;
  logger.info << "Press Ctrl+C to stop the miner...";

  // Wait for SIGINT
  std::unique_lock<std::mutex> lock(g_mutex);
  g_cv.wait(lock, [] { return !g_running.load(); });

  miner.stop();
  logger.info << "Miner stopped";
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << "Error: Work directory required.\n";
    printUsage();
    return 1;
  }

  std::string workDir;
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

  return runMiner(workDir);
}
