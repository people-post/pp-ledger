#ifndef PP_LEDGER_LIB_H
#define PP_LEDGER_LIB_H

#include <string>

namespace pp {

class Lib {
public:
    Lib();
    ~Lib();
    
    std::string getVersion() const;
};

} // namespace pp

#endif // PP_LEDGER_LIB_H
