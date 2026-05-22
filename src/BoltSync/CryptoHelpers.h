#pragma once

#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

namespace BoltSync
{
  bool hexToSecretKey(const std::string &hex, crypto::SecretKey &key);

  bool isOutputOurs(const crypto::PublicKey &txPublicKey,
                    size_t outputIndex,
                    const crypto::PublicKey &outputKey,
                    const crypto::SecretKey &viewSecretKey,
                    const crypto::PublicKey &spendPublicKey);

  crypto::SecretKey deriveOutputSecretKey(const crypto::KeyDerivation &derivation,
                                          size_t outputIndex,
                                          const crypto::SecretKey &spendSecretKey);

  bool getTxHash(const cn::Transaction &tx, crypto::Hash &hash);

  // ── Two-pass filter helpers ──────────────────────────────────────────
  //
  // These implement the Monero-compatible view tag computation.
  // Used by both the MDBX scanner (local) and the RPC sync path (remote).

  // Pass 1: Compute view tag from key derivation.
  // Matches Monero's derive_view_tag: H("view_tag" || derivation || varint(outputIndex))[0]
  uint8_t computeWalletViewTag(const crypto::KeyDerivation &derivation, size_t outputIndex);

  // Pass 2: Compute nullifier from output key and block height.
  // nullifier = H(outputKey || blockHeight)[0:4]
  void computeWalletNullifier(const crypto::PublicKey &outputKey,
                              uint32_t blockHeight,
                              uint8_t nullifier[4]);

  // Combined two-pass check: returns true if the output passes BOTH filters.
  // If true, the caller should do the full ECDH derivation to confirm ownership.
  // False positive rate: ~1/2^32.
  bool passesFilter(const crypto::KeyDerivation &derivation,
                    size_t outputIndex,
                    const crypto::PublicKey &outputKey,
                    uint32_t blockHeight,
                    uint8_t expectedViewTag,
                    const uint8_t expectedNullifier[4]);

  // Daemon-side view tag (uses txPubKey since daemon lacks view secret key).
  // Produces the same first byte as computeWalletViewTag for a matching
  // (txPubKey, viewSecretKey) pair.
  uint8_t computeDaemonViewTag(const crypto::PublicKey &txPubKey, size_t outputIndex);

  // Wallet-side two-pass check using daemon-computed filter entry.
  // Takes the txPubKey from the filter entry (daemon side) and recomputes
  // using the wallet's view secret key to produce the derivation.
  bool passesFilter(const crypto::PublicKey &txPubKey,
                    const crypto::SecretKey &viewSecretKey,
                    size_t outputIndex,
                    const crypto::PublicKey &outputKey,
                    uint32_t blockHeight,
                    uint8_t expectedViewTag,
                    const uint8_t expectedNullifier[4]);
}