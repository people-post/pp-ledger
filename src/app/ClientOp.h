#pragma once
// pp-client op …: operation files (docs/contracts/OPERATION_FILE.md) — create, show, sign, merge and
// submit a transaction that several people sign, passed between them as a file.

#include <cli11.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pp {
class Client;
}

class OpCommands {
public:
  explicit OpCommands(CLI::App &app);

  bool parsed() const;
  /** Whether the command reads the chain: submit, and a creator not given its signers and network. */
  bool needsNode() const;
  bool isSubmit() const;
  /** Run the parsed command; `client` is null when it needs no node. */
  int run(pp::Client *client);

private:
  struct Create {
    CLI::App *cmd{nullptr};
    std::string output;
    int64_t validFor{86400};
    std::string notes;
    std::string networkId;
    std::string genesisHash;
    std::vector<std::string> signerKeys;
    uint32_t minSignatures{0};
    uint64_t fee{0};
  };

  void addCreateOptions(Create &create, bool withFee);
  const Create *parsedCreate() const;
  int runCreate(const Create &create, pp::Client *client);

  CLI::App *op_{nullptr};
  CLI::App *show_{nullptr};
  CLI::App *sign_{nullptr};
  CLI::App *merge_{nullptr};
  CLI::App *submit_{nullptr};
  std::string file_;
  std::vector<std::string> files_;
  std::string key_;
  std::string output_;
  bool force_{false};

  Create transfer_;
  uint64_t from_{0};
  uint64_t to_{0};
  uint64_t amount_{0};
  uint64_t tokenId_{0};
  Create mint_;
  Create newAccount_;
  uint64_t accountId_{0};
  std::vector<std::string> accountKeys_;
  uint32_t accountMinSignatures_{1};
  Create wrap_;
  std::optional<uint64_t> wrapAccount_;
  Create batch_;
  std::string paymentsFile_;
  std::string meta_;
  Create updateKeys_;
  std::vector<std::string> newKeys_;
  uint32_t newMinSignatures_{1};
  Create setRecovery_;
  std::vector<std::string> recoveryKeys_;
  uint32_t recoveryMinSignatures_{1};
  uint64_t recoveryDelayEpochs_{0};
  bool noRecovery_{false};
  Create cancelRecovery_;
  Create recover_;
};
