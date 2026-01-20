#include "../server/BeaconServer.h"
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
  std::cout << "Usage: pp-beacon -d <work-dir>\n";
  std::cout << "  -d <work-dir>  - Work directory (required)\n";
  std::cout << "\n";
  std::cout << "The work directory must contain:\n";
  std::cout << "  - config.json  - Beacon configuration file (required)\n";
  std::cout << "\n";
  std::cout << "Example:\n";
  std::cout << "  pp-beacon -d /some/path/to/work-dir\n";
  std::cout << "\n";
  std::cout << "The config.json file should contain:\n";
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

  auto logger = pp::logging::getLogger("beacon");
  logger->setLevel(pp::logging::Level::INFO);

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

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
