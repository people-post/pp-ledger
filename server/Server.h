#ifndef PP_LEDGER_SERVER_H
#define PP_LEDGER_SERVER_H

#include "Module.h"
#include <string>

namespace pp {

class Server : public Module {
public:
    Server();
    ~Server();
    
    bool start(int port);
    void stop();
    bool isRunning() const;
    
private:
    bool running_;
    int port_;
};

} // namespace pp

#endif // PP_LEDGER_SERVER_H
