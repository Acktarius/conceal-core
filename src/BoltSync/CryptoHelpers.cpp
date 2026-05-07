#include "CryptoHelpers.h"
#include "Common/Util.h"
#include "CryptoNoteCore/CryptoNoteTools.h"

namespace BoltSync
{
  bool hexToSecretKey(const std::string &hex, crypto::SecretKey &key)
  {
    return common::podFromHex(hex, key);
  }

  bool isOutputOurs(const crypto::PublicKey &txPublicKey,
                    size_t outputIndex,
                    const crypto::PublicKey &outputKey,
                    const crypto::SecretKey &viewSecretKey,
                    const crypto::PublicKey &viewPublicKey)
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPublicKey, viewSecretKey, derivation))
      return false;
    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, outputIndex, viewPublicKey, derivedKey))
      return false;
    return derivedKey == outputKey;
  }

  crypto::SecretKey deriveOutputSecretKey(const crypto::KeyDerivation &derivation,
                                          size_t outputIndex,
                                          const crypto::SecretKey &spendSecretKey)
  {
    crypto::SecretKey outputSecret;
    crypto::derive_secret_key(derivation, outputIndex, spendSecretKey, outputSecret);
    return outputSecret;
  }

  bool getTxHash(const cn::Transaction &tx, crypto::Hash &hash)
  {
    return getObjectHash(tx, hash);
  }
}