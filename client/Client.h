#ifndef PP_LEDGER_CLIENT_H
#define PP_LEDGER_CLIENT_H

#include "Module.h"
#include <string>

namespace pp {

class Client : public Module {
public:
    Client();
    ~Client();
    
    bool connect(const std::string& address, int port);
    void disconnect();
    bool isConnected() const;
    
private:
    bool connected_;
};

} // namespace pp

#endif // PP_LEDGER_CLIENT_H
