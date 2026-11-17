// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "AiProviderTypes.h"

#include <cstring>

#include "CryptoNoteCore/CryptoNoteBasicImpl.h"
#include "crypto/hash.h"

namespace cn {
namespace {

const char AI_ADVERTISEMENT_DOMAIN[] = "conceal-ai-provider-ad-v1";
const char AI_BINDING_DOMAIN[] = "conceal-ai-provider-binding-v1";

AiVerificationStats g_verificationStats;

void appendU8(std::vector<uint8_t>& out, uint8_t value) {
  out.push_back(value);
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

void appendU64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void appendBytes(std::vector<uint8_t>& out, const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + size);
}

bool modelsInCanonicalOrder(const std::vector<AiModelOffer>& models) {
  for (size_t i = 1; i < models.size(); ++i) {
    if (!(models[i - 1].modelId < models[i].modelId)) {
      return false;
    }
  }
  return true;
}

}

void resetAiVerificationStats() {
  g_verificationStats = AiVerificationStats();
}

AiVerificationStats aiVerificationStats() {
  return g_verificationStats;
}

const char* aiAcceptResultName(AiAcceptResult result) {
  switch (result) {
  case AiAcceptResult::accepted:
    return "accepted";
  case AiAcceptResult::duplicate:
    return "duplicate";
  case AiAcceptResult::stale:
    return "stale";
  case AiAcceptResult::invalid:
    return "invalid";
  case AiAcceptResult::capacityRejected:
    return "capacityRejected";
  case AiAcceptResult::rateLimited:
    return "rateLimited";
  }
  return "invalid";
}

bool buildAiAdvertisementCanonical(const AiProviderAdvertisement& ad, std::vector<uint8_t>& out) {
  out.clear();
  if (ad.models.size() > AI_MAX_MODELS_PER_PROVIDER || ad.contactValue.size() > AI_MAX_CONTACT_LENGTH) {
    return false;
  }
  if (!modelsInCanonicalOrder(ad.models)) {
    return false;
  }

  const uint8_t domainLen = static_cast<uint8_t>(sizeof(AI_ADVERTISEMENT_DOMAIN) - 1);
  out.reserve(256 + ad.models.size() * 96 + ad.contactValue.size());
  appendU8(out, domainLen);
  appendBytes(out, AI_ADVERTISEMENT_DOMAIN, domainLen);
  appendU8(out, ad.protocolVersion);
  appendBytes(out, ad.networkId.data, ad.networkId.size());
  appendBytes(out, ad.providerPublicKey.data, sizeof(ad.providerPublicKey.data));
  appendU64(out, ad.sequence);
  appendU64(out, static_cast<uint64_t>(ad.issuedAt));
  appendU64(out, static_cast<uint64_t>(ad.expiresAt));
  appendU8(out, ad.status);
  appendU32(out, ad.availableSlots);
  appendU32(out, static_cast<uint32_t>(ad.models.size()));

  for (size_t i = 0; i < ad.models.size(); ++i) {
    const AiModelOffer& model = ad.models[i];
    if (model.modelId.empty() || model.modelId.size() > AI_MAX_MODEL_ID_LENGTH) {
      out.clear();
      return false;
    }
    appendU32(out, static_cast<uint32_t>(model.modelId.size()));
    appendBytes(out, model.modelId.data(), model.modelId.size());
    appendU32(out, model.maxContextTokens);
    appendU64(out, model.priceAtomicUnits);
    appendU8(out, model.billingUnit);
    appendU8(out, model.modelDigestType);
    appendBytes(out, model.modelDigest.data(), model.modelDigest.size());
  }

  appendU8(out, ad.contactType);
  appendU32(out, static_cast<uint32_t>(ad.contactValue.size()));
  if (!ad.contactValue.empty()) {
    appendBytes(out, ad.contactValue.data(), ad.contactValue.size());
  }
  appendBytes(out, ad.addressBindingSignature.data, sizeof(ad.addressBindingSignature.data));

  if (out.size() > AI_ADVERTISEMENT_MAX_BYTES) {
    out.clear();
    return false;
  }
  return true;
}

crypto::Hash aiAdvertisementSigningHash(const std::vector<uint8_t>& canonical) {
  return crypto::cn_fast_hash(canonical.data(), canonical.size());
}

bool signAiAdvertisement(AiProviderAdvertisement& ad, const crypto::SecretKey& secret) {
  std::vector<uint8_t> canonical;
  if (!buildAiAdvertisementCanonical(ad, canonical)) {
    return false;
  }
  const crypto::Hash hash = aiAdvertisementSigningHash(canonical);
  crypto::generate_signature(hash, ad.providerPublicKey, secret, ad.signature);
  return true;
}

bool verifyAiAdvertisementSignature(const AiProviderAdvertisement& ad, std::vector<uint8_t>& canonicalOut) {
  if (!buildAiAdvertisementCanonical(ad, canonicalOut)) {
    return false;
  }
  const crypto::Hash hash = aiAdvertisementSigningHash(canonicalOut);
  ++g_verificationStats.providerSignatures;
  return crypto::check_signature(hash, ad.providerPublicKey, ad.signature);
}

bool buildAiAddressBindingCanonical(const boost::uuids::uuid& networkId, const crypto::PublicKey& providerPublicKey,
                                    const std::string& address, std::vector<uint8_t>& out) {
  out.clear();
  if (address.size() > AI_MAX_CONTACT_LENGTH) {
    return false;
  }
  const uint8_t domainLen = static_cast<uint8_t>(sizeof(AI_BINDING_DOMAIN) - 1);
  out.reserve(domainLen + 64 + address.size());
  appendU8(out, domainLen);
  appendBytes(out, AI_BINDING_DOMAIN, domainLen);
  appendBytes(out, networkId.data, networkId.size());
  appendBytes(out, providerPublicKey.data, sizeof(providerPublicKey.data));
  appendU32(out, static_cast<uint32_t>(address.size()));
  if (!address.empty()) {
    appendBytes(out, address.data(), address.size());
  }
  return true;
}

bool signAiAddressBinding(const boost::uuids::uuid& networkId, const crypto::PublicKey& providerPublicKey,
                          const std::string& address, const crypto::PublicKey& spendPublicKey,
                          const crypto::SecretKey& spendSecretKey, crypto::Signature& out) {
  std::vector<uint8_t> canonical;
  if (!buildAiAddressBindingCanonical(networkId, providerPublicKey, address, canonical)) {
    return false;
  }
  const crypto::Hash hash = crypto::cn_fast_hash(canonical.data(), canonical.size());
  crypto::generate_signature(hash, spendPublicKey, spendSecretKey, out);
  return true;
}

bool verifyAiAddressBinding(const boost::uuids::uuid& networkId, const crypto::PublicKey& providerPublicKey,
                            const std::string& address, const crypto::Signature& signature) {
  std::vector<uint8_t> canonical;
  if (!buildAiAddressBindingCanonical(networkId, providerPublicKey, address, canonical)) {
    return false;
  }
  uint64_t prefix = 0;
  AccountPublicAddress parsed;
  if (!parseAccountAddressString(prefix, parsed, address)) {
    return false;
  }
  const crypto::Hash hash = crypto::cn_fast_hash(canonical.data(), canonical.size());
  ++g_verificationStats.bindingSignatures;
  return crypto::check_signature(hash, parsed.spendPublicKey, signature);
}

}
