#include "MinerServer.h"
#include "Logger.h"

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
  std::cout << "Usage: pp-miner -d <work-dir>\n";
  std::cout << "  -d <work-dir>  - Work directory (required)\n";
  std::cout << "\n";
  std::cout << "The work directory must contain:\n";
  std::cout << "  - config.json  - Miner configuration file (required)\n";
  std::cout << "\n";
  std::cout << "Example:\n";
  std::cout << "  pp-miner -d /some/path/to/work-dir\n";
  std::cout << "\n";
  std::cout << "The config.json file should contain:\n";
  std::cout << "  {\n";
  std::cout << "    \"minerId\": \"miner1\",\n";
  std::cout << "    \"stake\": 1000000,\n";
  std::cout << "    \"host\": \"localhost\",\n";
  std::cout << "    \"port\": 8518,\n";
  std::cout << "    \"beacons\": [\"127.0.0.1:8517\"]\n";
  std::cout << "  }\n";
}

int main(int argc, char *argv[]) {
  auto rootLogger = pp::logging::getRootLogger();
  rootLogger->info << "PP-Ledger Miner v1.0";

  if (argc < 3) {
    std::cerr << "Error: Work directory required.\n";
    printUsage();
    return 1;
  }

  std::string workDir;

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

  auto logger = pp::logging::getLogger("miner");
  logger->setLevel(pp::logging::Level::INFO);

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  logger->info << "Starting miner with work directory: " << workDir;

  pp::MinerServer miner;
  miner.redirectLogger("pp.MinerServer");
  if (miner.start(workDir)) {
    logger->info << "Miner started successfully";
    std::cout << "Miner running\n";
    std::cout << "Work directory: " << workDir << "\n";
    std::cout << "Press Ctrl+C to stop the miner...\n";

    // Wait for SIGINT
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [] { return !g_running.load(); });

    miner.stop();
    logger->info << "Miner stopped";
    return 0;
  } else {
    logger->error << "Failed to start miner";
    std::cerr << "Error: Failed to start miner\n";
    return 1;
  }
}
