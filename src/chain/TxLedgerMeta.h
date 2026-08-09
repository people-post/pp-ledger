#ifndef PP_LEDGER_TX_LEDGER_META_H
#define PP_LEDGER_TX_LEDGER_META_H

#include "TxError.h"
#include "Types.h"
#include "AccountBuffer.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pp::chain_tx {

using FnUserAccountMetaForRecord =
    std::function<std::optional<std::string>(const Ledger::Record &,
                                           uint64_t accountId)>;
using FnGenesisAccountMetaForRecord =
    std::function<std::optional<std::string>(const Ledger::Record &,
                                             const Ledger::Block &block)>;

Roe<Client::UserAccount>
getUserAccountMetaFromBlock(const Ledger::Block &block, uint64_t accountId,
                            const FnUserAccountMetaForRecord &fnUserMetaForRecord);

Roe<GenesisAccountMeta>
getGenesisAccountMetaFromBlock(
    const Ledger::Block &block,
    const FnGenesisAccountMetaForRecord &fnGenesisMetaForRecord);

Roe<std::string> getUpdatedAccountMetadataForRenewal(
    const Ledger::Block &block, const AccountBuffer::Account &account,
    uint64_t minFee, const FnUserAccountMetaForRecord &fnUserMetaForRecord,
    const FnGenesisAccountMetaForRecord &fnGenesisMetaForRecord);

} // namespace pp::chain_tx

#endif
