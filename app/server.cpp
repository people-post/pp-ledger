#include "Server.h"
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
  std::cout << "Usage: pp-ledger-server -d <work-dir>\n";
  std::cout << "  -d <work-dir>  - Work directory (required)\n";
  std::cout << "\n";
  std::cout << "The work directory must contain:\n";
  std::cout << "  - config.json  - Server configuration file (required)\n";
  std::cout << "\n";
  std::cout << "Example:\n";
  std::cout << "  pp-ledger-server -d /some/path/to/work-dir\n";
  std::cout << "\n";
  std::cout << "The config.json file should contain:\n";
  std::cout << "  {\n";
  std::cout << "    \"port\": 8080,\n";
  std::cout << "    \"network\": {\n";
  std::cout << "      \"enableP2P\": false,\n";
  std::cout << "      \"nodeId\": \"\",\n";
  std::cout << "      \"bootstrapPeers\": [],\n";
  std::cout << "      \"listenAddr\": \"0.0.0.0\",\n";
  std::cout << "      \"p2pPort\": 9000,\n";
  std::cout << "      \"maxPeers\": 50\n";
  std::cout << "    }\n";
  std::cout << "  }\n";
}

int main(int argc, char *argv[]) {
  auto &rootLogger = pp::logging::getRootLogger();
  rootLogger.info << "PP-Ledger Server v" << pp::Client::VERSION;

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

  auto &logger = pp::logging::getLogger("server");
  logger.setLevel(pp::logging::Level::INFO);
  logger.addFileHandler("server.log", pp::logging::Level::DEBUG);

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  logger.info << "Starting server with work directory: " << workDir;

  pp::Server server;
  if (server.start(workDir)) {
    logger.info << "Server started successfully";
    std::cout << "Server running\n";
    std::cout << "Work directory: " << workDir << "\n";
    std::cout << "Press Ctrl+C to stop the server...\n";

    // Wait for SIGINT
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [] { return !g_running.load(); });

    server.stop();
    logger.info << "Server stopped";
    return 0;
  } else {
    logger.error << "Failed to start server";
    std::cerr << "Error: Failed to start server\n";
    return 1;
  }
}
