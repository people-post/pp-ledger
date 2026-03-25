#ifndef PP_LEDGER_TX_LEDGER_META_H
#define PP_LEDGER_TX_LEDGER_META_H

#include "TxError.h"
#include "Types.h"
#include "AccountBuffer.h"
#include "../client/Client.h"
#include "../ledger/Ledger.h"

#include <cstdint>
#include <string>

namespace pp::chain_tx {

Roe<Client::UserAccount>
getUserAccountMetaFromBlock(const Ledger::Block &block, uint64_t accountId);

Roe<GenesisAccountMeta>
getGenesisAccountMetaFromBlock(const Ledger::Block &block);

Roe<std::string> getUpdatedAccountMetadataForRenewal(
    const Ledger::Block &block, const AccountBuffer::Account &account,
    uint64_t minFee);

} // namespace pp::chain_tx

#endif
