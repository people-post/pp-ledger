#include "../ledger/Ledger.h"
#include "../ledger/BlockChain.h"
#include "../network/Types.hpp"
#include "../lib/Module.h"
#include "../lib/ResultOrError.hpp"

#include <string>
#include <cstdint>
#include <list>

namespace pp {
class Beacon : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  struct Config {
    std::string workDir;
  };

  struct Stakeholder {
    network::TcpEndpoint endpoint;
  };

  Beacon() = default;
  ~Beacon() override = default;

  uint32_t getVersion() const { return VERSION; }
  uint64_t getCurrentCheckpointId() const;
  const std::list<Stakeholder>& getStakeholders() const;
  std::string getBlock(uint64_t blockId) const;

  Roe<void> init(const Config& config);

  Roe<void> addBlock(const std::string& blockData);
  void addStakeholder(const Stakeholder& stakeholder);

  Roe<void> syncChain(const BlockChain& otherChain);


private:
  static constexpr uint32_t VERSION = 1;

  Ledger ledger_;

  std::list<Stakeholder> stakeholders_;
};

} // namespace pp