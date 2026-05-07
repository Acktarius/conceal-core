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
                    const crypto::PublicKey &viewPublicKey);

  crypto::SecretKey deriveOutputSecretKey(const crypto::KeyDerivation &derivation,
                                          size_t outputIndex,
                                          const crypto::SecretKey &spendSecretKey);

  bool getTxHash(const cn::Transaction &tx, crypto::Hash &hash);
}