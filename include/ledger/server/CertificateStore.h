#ifndef PP_LEDGER_CERTIFICATE_STORE_H
#define PP_LEDGER_CERTIFICATE_STORE_H

#include "Chain.h"

#include "../ledger/Ledger.h"
#include "common/Module.h"
#include "common/ResultOrError.hpp"

#include <optional>
#include <string>

namespace pp {

/**
 * The highest beacon certificate a node has seen out of band (beacon-authority ba5c): on STATUS,
 * in a TX_ADD, or from an operator's file submitted to the node. The chain verifies and keeps it
 * (Chain::offerCertificate); this store writes the record the chain kept to `beacon-cert.dat`
 * beside the ledger before the offer returns, so a restart keeps refusing what it refused. The
 * file holds the packed T_BEACON_CERT record, as `pp-client mk-beacon-cert` writes it.
 *
 * A node with no blocks cannot verify a certificate yet: the stored one waits until the chain
 * holds its first block (retryPending). Server thread only.
 */
class CertificateStore : public Module {
public:
  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };
  template <typename T> using Roe = ResultOrError<T, Error>;

  static constexpr const int32_t E_IO = 1;

  CertificateStore(Chain &chain, std::string path);

  /**
   * After mount: offer the stored certificate to the chain again. The file is removed when the
   * chain will not keep it: on chain already (or a later one), or it no longer verifies.
   */
  Roe<void> load();

  /** Offer `record` to the chain; persist it when kept. */
  Roe<Chain::CertificateOffer> offer(const Ledger::Record &record);

  /**
   * offer() a certificate a peer serves or submits (`from`): a refusal is logged, once per record
   * (STATUS repeats it). None when refused.
   */
  std::optional<Chain::CertificateOffer> note(const Ledger::Record &record, const std::string &from);

  /** A stored certificate the chain could not take yet (no blocks): offer it again. */
  void retryPending();

  /**
   * After blocks apply: remove the file once the chain no longer holds the certificate (a block
   * committed its generation or a later one, or an admin change left it unsigned).
   */
  void refresh();

private:
  /** Offer the stored record; remove the file when the chain will never keep it. */
  void offerStored(const Ledger::Record &record);
  Roe<void> persist(const Ledger::Record &record);
  void remove();

  Chain &chain_;
  std::string path_;
  std::optional<Ledger::Record> pending_;
  /** The file holds a certificate (it may be removed). */
  bool stored_{false};
  /** Data of the last certificate note() reported as refused. */
  std::string lastRefused_;
};

} // namespace pp

#endif // PP_LEDGER_CERTIFICATE_STORE_H
