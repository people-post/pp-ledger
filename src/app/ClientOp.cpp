#include "ClientOp.h"

#include "AccountAttachment.h"
#include "AccountIds.h"
#include "Client.h"
#include "../operation/Operation.h"
#include "lib/common/Crypto.h"
#include "lib/common/Utilities.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

namespace op = pp::operation;

namespace {

int64_t nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int fail(const std::string &message) {
  std::cerr << "Error: " << message << "\n";
  return 1;
}

/** An ML-DSA-65 public key given as hex or as a .pub file; empty when it is neither. */
std::string readPublicKey(const std::string &arg) {
  std::string hex = arg;
  if (std::ifstream file(arg); file) {
    std::ostringstream text;
    text << file.rdbuf();
    hex = text.str();
  }
  hex.erase(std::remove_if(hex.begin(), hex.end(), [](unsigned char c) { return std::isspace(c); }), hex.end());
  if (hex.rfind("0x", 0) == 0) {
    hex = hex.substr(2);
  }
  std::string key = pp::utl::hexDecode(hex);
  return key.size() == pp::utl::kMlDsaPublicKeyBytes ? key : std::string{};
}

pp::Roe<std::vector<std::string>> readPublicKeys(const std::vector<std::string> &args, const std::string &option) {
  std::vector<std::string> keys;
  for (const auto &arg : args) {
    std::string key = readPublicKey(arg);
    if (key.empty()) {
      return pp::Error(option + " " + arg + ": not an ML-DSA-65 public key (hex or a .pub file)");
    }
    keys.push_back(std::move(key));
  }
  return keys;
}

std::string shortHex(const std::string &raw) {
  const std::string hex = pp::utl::hexEncode(raw);
  return hex.size() > 16 ? hex.substr(0, 16) + "…" : hex;
}

void print(const op::Operation &operation) {
  auto summary = op::summarize(operation);
  if (summary) {
    std::cout << summary.value().title << "\n";
    for (const auto &[label, value] : summary.value().lines) {
      std::cout << "  " << label << ": " << value << "\n";
    }
  } else {
    std::cout << "(cannot summarize: " << summary.error().message << ")\n";
  }
  std::cout << "Chain: network " << operation.networkId << ", genesis "
            << (operation.genesisHash.empty() ? std::string("not named") : shortHex(operation.genesisHash)) << "\n";
  if (summary && summary.value().validUntil != 0) {
    const int64_t now = nowSeconds();
    std::cout << "Valid: " << pp::utl::formatTimestampLocal(summary.value().validFrom) << " until "
              << pp::utl::formatTimestampLocal(summary.value().validUntil);
    if (now > summary.value().validUntil) {
      std::cout << " — EXPIRED: make the operation again";
    } else if (now < summary.value().validFrom) {
      std::cout << " — not valid yet";
    }
    std::cout << "\n";
  }
  const auto check = op::check(operation);
  std::cout << "Signers: " << operation.signers.minSignatures << " of " << operation.signers.publicKeys.size();
  if (operation.signers.account) {
    std::cout << " (account " << *operation.signers.account << ")";
  } else {
    std::cout << " (a key set)";
  }
  std::cout << "\n";
  for (const auto &key : operation.signers.publicKeys) {
    const bool signedIt = std::find(check.signedBy.begin(), check.signedBy.end(), key) != check.signedBy.end();
    std::cout << "  " << (signedIt ? "[signed] " : "[ ]      ") << op::fingerprint(key) << "\n";
  }
  if (check.invalid > 0) {
    std::cout << "Invalid signatures: " << check.invalid << " (by no signer, or a repeat)\n";
  }
  std::cout << "Signatures: " << check.signedBy.size() << " of " << check.needed << " needed — "
            << (check.complete() ? "complete, ready to submit" : "incomplete") << "\n";
  if (!operation.notes.empty()) {
    std::cout << "Notes (the creator's own words): " << operation.notes << "\n";
  }
}

/** A batch's payments: a CSV of `to,amount[,token]` lines ('#' starts a comment). */
pp::Roe<std::vector<op::Payment>> readPayments(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    return pp::Error("cannot open " + path);
  }
  std::vector<op::Payment> payments;
  std::string line;
  for (size_t number = 1; std::getline(in, line); ++number) {
    line = line.substr(0, line.find('#'));
    line.erase(std::remove_if(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c); }), line.end());
    if (line.empty()) {
      continue;
    }
    std::vector<uint64_t> fields;
    std::istringstream cells(line);
    for (std::string cell; std::getline(cells, cell, ',');) {
      if (cell.empty() || !std::all_of(cell.begin(), cell.end(), [](unsigned char c) { return std::isdigit(c); }) ||
          cell.size() > 19) {
        return pp::Error(path + " line " + std::to_string(number) + ": not to,amount[,token]");
      }
      fields.push_back(std::stoull(cell));
    }
    if (fields.size() < 2 || fields.size() > 3) {
      return pp::Error(path + " line " + std::to_string(number) + ": not to,amount[,token]");
    }
    payments.push_back({fields[0], fields[1], fields.size() == 3 ? fields[2] : 0});
  }
  return payments;
}

pp::Roe<op::Operation> loadChecked(const std::string &path) {
  auto operation = op::load(path);
  if (!operation) {
    return pp::Error(path + ": " + operation.error().message);
  }
  return operation;
}

} // namespace

OpCommands::OpCommands(CLI::App &app) {
  op_ = app.add_subcommand("op", "Operation files: transactions several people sign, passed around as files");
  op_->require_subcommand(1);

  show_ = op_->add_subcommand("show", "Show what an operation does, who signed it, and whether it is complete");
  show_->add_option("file", file_, "Operation file")->required();

  sign_ = op_->add_subcommand("sign", "Sign an operation (with one of its signers' keys)");
  sign_->add_option("file", file_, "Operation file")->required();
  sign_->add_option("-k,--key", key_, "Private key (hex or file)")->required();
  sign_->add_option("-o,--output", output_, "Write the signed copy here (default: replace the file)");

  merge_ = op_->add_subcommand("merge", "Merge copies of one operation signed apart");
  merge_->add_option("files", files_, "Signed copies")->required()->expected(2, -1);
  merge_->add_option("-o,--output", output_, "Merged operation file")->required();
  merge_->add_flag("--force", force_, "Replace the output file if it exists");

  submit_ = op_->add_subcommand("submit", "Submit a complete operation (to a relay or the beacon, -b)");
  submit_->add_option("file", file_, "Operation file, signed by its quorum")->required();

  transfer_.cmd = op_->add_subcommand("transfer", "Create a transfer from a (multi-signature) account");
  transfer_.cmd->add_option("--from", from_, "Paying account (its keys sign)")->required();
  transfer_.cmd->add_option("--to", to_, "Receiving account")->required();
  transfer_.cmd->add_option("--amount", amount_, "Amount")->required();
  transfer_.cmd->add_option("--token", tokenId_, "Token id (0: the native token)");
  addCreateOptions(transfer_, true);

  batch_.cmd = op_->add_subcommand("batch", "Create a batch: several transfers from one account, signed together");
  batch_.cmd->add_option("--from", from_, "Paying account (its keys sign)")->required();
  batch_.cmd->add_option("--payments", paymentsFile_, "CSV of to,amount[,token] lines")->required();
  batch_.cmd->add_option("--meta", meta_, "Text every transfer carries (e.g. what the batch is for)");
  addCreateOptions(batch_, true);

  mint_.cmd = op_->add_subcommand("mint", "Create a mint: new native tokens to the reserve (the admin keys sign)");
  mint_.cmd->add_option("--amount", amount_, "Amount")->required();
  addCreateOptions(mint_, true);

  newAccount_.cmd = op_->add_subcommand("new-account", "Create a new account, funded by a (multi-signature) account");
  newAccount_.cmd->add_option("--from", from_, "Funding account (its keys sign; the registrar for issued ids)")
      ->required();
  newAccount_.cmd->add_option("--id", accountId_, "The new account's id")->required();
  newAccount_.cmd->add_option("--account-key", accountKeys_, "The new account's public key (hex or .pub; repeat)")
      ->required();
  newAccount_.cmd->add_option("--account-min-signatures", accountMinSignatures_,
                              "Signatures the new account needs")
      ->default_val(1);
  newAccount_.cmd->add_option("--amount", amount_, "Initial balance, paid by --from")->required();
  addCreateOptions(newAccount_, true);

  updateKeys_.cmd = op_->add_subcommand(
      "update-keys", "Replace an account's keys (signed by the keys in force; reads the account from the node)");
  updateKeys_.cmd->add_option("--account", from_, "The account")->required();
  updateKeys_.cmd->add_option("--new-key", newKeys_, "A key of the new set (hex or .pub; repeat)")->required();
  updateKeys_.cmd->add_option("--new-min-signatures", newMinSignatures_, "Signatures the new set needs")
      ->default_val(1);
  addCreateOptions(updateKeys_, true);

  wrap_.cmd = op_->add_subcommand(
      "wrap", "Make an operation of a transaction file (mk-beacon-cert, mk-recovery, mk-tx …)");
  wrap_.cmd->add_option("file", file_, "Transaction file (its signatures are dropped)")->required();
  addCreateOptions(wrap_, false);
}

void OpCommands::addCreateOptions(Create &create, bool withFee) {
  create.cmd->add_option("-o,--output", create.output, "Operation file to write")->required();
  create.cmd->add_option("--valid-for", create.validFor, "Seconds the transaction stays valid (the chain caps it)")
      ->default_val(86400);
  create.cmd->add_option("--notes", create.notes, "Free text for the signers (shown as yours)");
  create.cmd->add_option("--network-id", create.networkId, "Network id (default: the node's)");
  create.cmd->add_option("--genesis-hash", create.genesisHash, "Block 0 hash, hex (default: the node's)");
  create.cmd->add_option("--signer-key", create.signerKeys,
                         "A signer's public key (hex or .pub; repeat; default: the account's keys on chain)");
  create.cmd->add_option("--min-signatures", create.minSignatures,
                         "Signatures needed (with --signer-key; default: all of them)");
  if (withFee) {
    create.cmd->add_option("-f,--fee", create.fee, "Fee, paid by the signers' account (a batch: each transfer's)")
        ->required();
  }
}

bool OpCommands::parsed() const { return op_->parsed(); }

const OpCommands::Create *OpCommands::parsedCreate() const {
  for (const Create *create : {&transfer_, &batch_, &mint_, &newAccount_, &updateKeys_, &wrap_}) {
    if (create->cmd->parsed()) {
      return create;
    }
  }
  return nullptr;
}

bool OpCommands::isSubmit() const { return submit_->parsed(); }

bool OpCommands::needsNode() const {
  if (submit_->parsed()) {
    return true;
  }
  const Create *create = parsedCreate();
  // A key replacement carries the account's balances as they are: always read from the node.
  return create && (create == &updateKeys_ || create->networkId.empty() || create->signerKeys.empty());
}

int OpCommands::run(pp::Client *client) {
  if (show_->parsed()) {
    auto operation = loadChecked(file_);
    if (!operation) {
      return fail(operation.error().message);
    }
    print(operation.value());
    return 0;
  }

  if (sign_->parsed()) {
    auto operation = loadChecked(file_);
    if (!operation) {
      return fail(operation.error().message);
    }
    auto privateKey = pp::utl::readPrivateKey(key_, "");
    if (!privateKey) {
      return fail("--key: " + privateKey.error().message);
    }
    auto signedBy = op::sign(operation.value(), privateKey.value());
    if (!signedBy) {
      return fail(signedBy.error().message);
    }
    const std::string out = output_.empty() ? file_ : output_;
    if (auto saved = op::save(operation.value(), out, output_.empty()); !saved) {
      return fail(saved.error().message);
    }
    const auto check = op::check(operation.value());
    std::cout << "Signed with " << op::fingerprint(signedBy.value()) << " into " << out << ": "
              << check.signedBy.size() << " of " << check.needed << " signatures"
              << (check.complete() ? " — complete" : "") << "\n";
    return 0;
  }

  if (merge_->parsed()) {
    std::vector<op::Operation> copies;
    for (const auto &file : files_) {
      auto operation = loadChecked(file);
      if (!operation) {
        return fail(operation.error().message);
      }
      copies.push_back(std::move(operation.value()));
    }
    auto merged = op::merge(copies);
    if (!merged) {
      return fail(merged.error().message);
    }
    if (auto saved = op::save(merged.value(), output_, force_); !saved) {
      return fail(saved.error().message);
    }
    const auto check = op::check(merged.value());
    std::cout << "Merged " << copies.size() << " copies into " << output_ << ": " << check.signedBy.size() << " of "
              << check.needed << " signatures" << (check.complete() ? " — complete" : "") << "\n";
    return 0;
  }

  if (submit_->parsed()) {
    auto operation = loadChecked(file_);
    if (!operation) {
      return fail(operation.error().message);
    }
    if (operation.value().kind == op::Kind::Genesis) {
      return fail("a genesis is not submitted: it starts a beacon (pp-beacon --init --genesis-op " + file_ + ")");
    }
    auto records = op::signedRecords(operation.value());
    if (!records) {
      return fail(records.error().message);
    }
    auto networkId = client->fetchNetworkId();
    if (!networkId) {
      return fail("reading the network id: " + networkId.error().message);
    }
    if (networkId.value() != operation.value().networkId) {
      return fail("the operation is for network " + operation.value().networkId + ", the node serves " +
                  networkId.value());
    }
    if (!operation.value().genesisHash.empty()) {
      auto genesis = client->fetchBlock(0);
      if (!genesis) {
        return fail("reading block 0: " + genesis.error().message);
      }
      if (genesis.value().hash != operation.value().genesisHash) {
        return fail("the operation is for another chain of network " + networkId.value() +
                    " (its genesis hash differs from the node's)");
      }
    }
    // A batch's transfers go one by one: each stands alone (sending one again is harmless).
    size_t refused = 0;
    for (size_t i = 0; i < records.value().size(); ++i) {
      if (auto reply = client->submitTransaction(records.value()[i]); !reply) {
        if (records.value().size() == 1) {
          return fail(reply.error().message);
        }
        ++refused;
        std::cerr << "Transfer " << (i + 1) << " refused: " << reply.error().message << "\n";
      }
    }
    if (refused > 0) {
      return fail(std::to_string(refused) + " of " + std::to_string(records.value().size()) +
                  " transfers refused (the others were submitted)");
    }
    std::cout << "Submitted: " << op::kindName(operation.value().kind)
              << (records.value().size() > 1 ? " of " + std::to_string(records.value().size()) + " transfers" : "")
              << "\n";
    return 0;
  }

  if (const Create *create = parsedCreate()) {
    return runCreate(*create, client);
  }
  return fail("unknown op command");
}

int OpCommands::runCreate(const Create &create, pp::Client *client) {
  // The transaction a wrap carries decides its signers' account; the others are built from options.
  pp::Ledger::Record wrapped;
  std::optional<uint64_t> account;
  if (&create == &wrap_) {
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
      return fail("cannot open " + file_);
    }
    std::ostringstream content;
    content << in.rdbuf();
    auto record = pp::utl::binaryUnpack<pp::Ledger::Record>(content.str());
    if (!record) {
      return fail(file_ + ": not a transaction file: " + record.error().message);
    }
    wrapped = record.value();
    account = op::signerAccountOf(wrapped);
  } else if (&create == &mint_) {
    account = pp::AccountIds::ID_GENESIS;
  } else {
    account = from_;
  }

  op::Common common;
  common.networkId = create.networkId;
  common.signers.account = account;
  common.window = op::windowFrom(nowSeconds(), create.validFor);
  common.notes = create.notes;
  common.createdAt = nowSeconds();
  if (!create.genesisHash.empty()) {
    common.genesisHash = pp::utl::hexDecode(create.genesisHash);
    if (common.genesisHash.size() != 32) {
      return fail("--genesis-hash: not a 32-byte hash in hex");
    }
  }
  if (!create.signerKeys.empty()) {
    auto keys = readPublicKeys(create.signerKeys, "--signer-key");
    if (!keys) {
      return fail(keys.error().message);
    }
    common.signers.publicKeys = keys.value();
    common.signers.minSignatures =
        create.minSignatures != 0 ? create.minSignatures : static_cast<uint32_t>(keys.value().size());
  }

  if (client) {
    auto networkId = client->fetchNetworkId();
    if (!networkId) {
      return fail("reading the network id: " + networkId.error().message);
    }
    if (!common.networkId.empty() && common.networkId != networkId.value()) {
      return fail("--network-id " + common.networkId + ", the node serves " + networkId.value());
    }
    common.networkId = networkId.value();
    auto genesis = client->fetchBlock(0);
    if (!genesis) {
      return fail("reading block 0: " + genesis.error().message);
    }
    if (!common.genesisHash.empty() && common.genesisHash != genesis.value().hash) {
      return fail("--genesis-hash is not the node's block 0");
    }
    common.genesisHash = genesis.value().hash;
    if (common.signers.publicKeys.empty()) {
      if (!account) {
        return fail("a " + op::kindName(op::kindOf(wrapped)) +
                    " is signed by a key set, not an account: give --signer-key and --min-signatures");
      }
      auto onChain = client->fetchUserAccount(*account);
      if (!onChain) {
        return fail("reading account " + std::to_string(*account) + ": " + onChain.error().message);
      }
      common.signers.publicKeys = onChain.value().wallet.publicKeys;
      common.signers.minSignatures = onChain.value().wallet.minSignatures;
      if (create.minSignatures != 0 && create.minSignatures != common.signers.minSignatures) {
        return fail("account " + std::to_string(*account) + " needs " +
                    std::to_string(common.signers.minSignatures) + " signatures, not " +
                    std::to_string(create.minSignatures));
      }
    }
  }

  pp::Roe<op::Operation> built = pp::Error("unknown op command");
  if (&create == &transfer_) {
    built = op::transfer(common, to_, amount_, create.fee, tokenId_);
  } else if (&create == &batch_) {
    auto payments = readPayments(paymentsFile_);
    if (!payments) {
      return fail("--payments: " + payments.error().message);
    }
    built = op::transfers(common, payments.value(), create.fee, meta_);
  } else if (&create == &mint_) {
    built = op::mint(common, amount_, create.fee);
  } else if (&create == &updateKeys_) {
    auto keys = readPublicKeys(newKeys_, "--new-key");
    if (!keys) {
      return fail(keys.error().message);
    }
    auto current = client->fetchUserAccount(from_);
    if (!current) {
      return fail("reading account " + std::to_string(from_) + ": " + current.error().message);
    }
    built = op::replaceKeys(common, current.value(), keys.value(), newMinSignatures_, create.fee);
  } else if (&create == &newAccount_) {
    auto keys = readPublicKeys(accountKeys_, "--account-key");
    if (!keys) {
      return fail(keys.error().message);
    }
    if (accountMinSignatures_ == 0 || accountMinSignatures_ > keys.value().size() || accountMinSignatures_ > 255) {
      return fail("--account-min-signatures must be between 1 and the number of --account-key");
    }
    pp::UserAccount newAccount;
    newAccount.wallet.publicKeys = keys.value();
    newAccount.wallet.minSignatures = static_cast<uint8_t>(accountMinSignatures_);
    newAccount.wallet.keyType = pp::Crypto::TK_ML_DSA_65;
    newAccount.wallet.mBalances[pp::AccountIds::ID_GENESIS] = static_cast<int64_t>(amount_);
    newAccount.meta = pp::AccountAttachment::emptySerialized();
    built = op::newAccount(common, accountId_, newAccount, amount_, create.fee);
  } else {
    built = op::wrap(common, wrapped);
  }
  if (!built) {
    return fail(built.error().message);
  }
  if (auto saved = op::save(built.value(), create.output); !saved) {
    return fail(saved.error().message);
  }
  print(built.value());
  std::cout << "Written to " << create.output << ": hand a copy to each signer (pp-client op sign), merge the "
            << "signed copies (op merge) and submit (op submit).\n";
  return 0;
}
