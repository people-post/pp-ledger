#include "Server.h"

namespace pp {

Server::Server() : running_(false), port_(0) {
    // Constructor
}

Server::~Server() {
    stop();
}

bool Server::start(int port) {
    if (running_) {
        return false;
    }
    
    port_ = port;
    // TODO: Implement server startup logic
    running_ = true;
    return true;
}

void Server::stop() {
    if (running_) {
        // TODO: Implement server shutdown logic
        running_ = false;
    }
}

bool Server::isRunning() const {
    return running_;
}

} // namespace pp
