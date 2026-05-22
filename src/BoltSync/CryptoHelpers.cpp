#include "CryptoHelpers.h"
#include "Common/Util.h"
#include "Common/Varint.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include <cstring>

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
                    const crypto::PublicKey &spendPublicKey)
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPublicKey, viewSecretKey, derivation))
      return false;
    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, outputIndex, spendPublicKey, derivedKey))
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

  // Two-pass filter helpers
  namespace
  {

    // Shared buffer builder for view tag computation.
    // Returns the number of bytes written to buf.
    // Buffer layout: salt[8] || derivation[32] || varint(outputIndex)
    size_t buildViewTagBuf(const crypto::KeyDerivation &derivation,
                           size_t outputIndex,
                           uint8_t buf[8 + 32 + 10])
    {
      std::memcpy(buf, "view_tag", 8);
      std::memcpy(buf + 8, &derivation, sizeof(crypto::KeyDerivation));

      char varint_buf[10];
      char *end = varint_buf;
      tools::write_varint(end, outputIndex);
      size_t varint_len = end - varint_buf;
      std::memcpy(buf + 40, varint_buf, varint_len);

      return 40 + varint_len;
    }

  } // anonymous namespace

  uint8_t computeWalletViewTag(const crypto::KeyDerivation &derivation,
                               size_t outputIndex)
  {
    // Matches Monero's derive_view_tag:
    //   view_tag = H("view_tag" || derivation || varint(outputIndex))[0]
    uint8_t buf[8 + 32 + 10];
    size_t len = buildViewTagBuf(derivation, outputIndex, buf);

    crypto::Hash h;
    crypto::cn_fast_hash(buf, len, reinterpret_cast<char *>(&h));
    return reinterpret_cast<const uint8_t *>(&h)[0];
  }

  uint8_t computeDaemonViewTag(const crypto::PublicKey &txPubKey,
                               size_t outputIndex)
  {
    // Daemon-side: uses txPubKey as stand-in for derivation.
    // Produces the same first byte as computeWalletViewTag for a matching
    // (txPubKey, viewSecretKey) pair because derivation = txPubKey * viewSecretKey
    // is deterministic.
    uint8_t buf[8 + 32 + 10];
    std::memcpy(buf, "view_tag", 8);
    std::memcpy(buf + 8, &txPubKey, sizeof(crypto::PublicKey));

    char varint_buf[10];
    char *end = varint_buf;
    tools::write_varint(end, outputIndex);
    size_t varint_len = end - varint_buf;
    std::memcpy(buf + 40, varint_buf, varint_len);

    crypto::Hash h;
    crypto::cn_fast_hash(buf, 40 + varint_len, reinterpret_cast<char *>(&h));
    return reinterpret_cast<const uint8_t *>(&h)[0];
  }

  void computeWalletNullifier(const crypto::PublicKey &outputKey,
                              uint32_t blockHeight,
                              uint8_t nullifier[4])
  {
    // nullifier = H(outputKey || blockHeight)[0:4]
    uint8_t buf[32 + 4];
    std::memcpy(buf, &outputKey, 32);
    for (int i = 0; i < 4; ++i)
    {
      buf[32 + i] = static_cast<uint8_t>((blockHeight >> (8 * i)) & 0xFF);
    }

    crypto::Hash h;
    crypto::cn_fast_hash(buf, sizeof(buf), reinterpret_cast<char *>(&h));
    std::memcpy(nullifier, &h, 4);
  }

  bool passesFilter(const crypto::KeyDerivation &derivation,
                    size_t outputIndex,
                    const crypto::PublicKey &outputKey,
                    uint32_t blockHeight,
                    uint8_t expectedViewTag,
                    const uint8_t expectedNullifier[4])
  {
    // Pass 1: View tag
    uint8_t computedTag = computeWalletViewTag(derivation, outputIndex);
    if (computedTag != expectedViewTag)
      return false;

    // Pass 2: Nullifier
    uint8_t computedNullifier[4];
    computeWalletNullifier(outputKey, blockHeight, computedNullifier);
    if (std::memcmp(computedNullifier, expectedNullifier, 4) != 0)
      return false;

    return true;
  }

  bool passesFilter(const crypto::PublicKey &txPubKey,
                    const crypto::SecretKey &viewSecretKey,
                    size_t outputIndex,
                    const crypto::PublicKey &outputKey,
                    uint32_t blockHeight,
                    uint8_t expectedViewTag,
                    const uint8_t expectedNullifier[4])
  {
    // Compute derivation from txPubKey and view secret key
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, viewSecretKey, derivation))
      return false;

    // Pass 1: View tag (using derivation, matching Monero's derive_view_tag)
    uint8_t computedTag = computeWalletViewTag(derivation, outputIndex);
    if (computedTag != expectedViewTag)
      return false;

    // Pass 2: Nullifier
    uint8_t computedNullifier[4];
    computeWalletNullifier(outputKey, blockHeight, computedNullifier);
    if (std::memcmp(computedNullifier, expectedNullifier, 4) != 0)
      return false;

    return true;
  }
} // namespace BoltSync