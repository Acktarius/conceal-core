// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "AiProviderValidation.h"

#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/CryptoNoteBasicImpl.h"
#include "crypto/crypto.h"

namespace cn {
namespace {

bool knownStatus(uint8_t status) {
  return status == AI_STATUS_AVAILABLE || status == AI_STATUS_BUSY || status == AI_STATUS_OFFLINE;
}

bool knownBilling(uint8_t billing) {
  return billing == AI_BILLING_PER_REQUEST || billing == AI_BILLING_PER_THOUSAND_TOKENS || billing == AI_BILLING_PER_SECOND;
}

bool signatureIsZero(const crypto::Signature& signature) {
  for (size_t i = 0; i < sizeof(signature.data); ++i) {
    if (signature.data[i] != 0) {
      return false;
    }
  }
  return true;
}

bool digestIsZero(const std::array<uint8_t, AI_MODEL_DIGEST_BYTES>& digest) {
  for (size_t i = 0; i < digest.size(); ++i) {
    if (digest[i] != 0) {
      return false;
    }
  }
  return true;
}

bool acceptCcxAddress(const std::string& address, std::string& reason) {
  uint64_t prefix = 0;
  AccountPublicAddress parsed;
  if (!parseAccountAddressString(prefix, parsed, address)) {
    reason = "malformed ccx address";
    return false;
  }
  if (prefix != parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX) {
    reason = "wrong address prefix";
    return false;
  }
  return true;
}

void rollWindow(AiRateWindow& window, int64_t now, int64_t windowSeconds) {
  if (window.windowStart == 0 || now < window.windowStart || now - window.windowStart >= windowSeconds) {
    window.windowStart = now;
    window.count = 0;
  }
}

}

bool AiRateWindow::allow(int64_t now, uint32_t limit, int64_t windowSeconds) {
  return allowMany(now, 1, limit, windowSeconds);
}

bool AiRateWindow::allowMany(int64_t now, uint32_t amount, uint32_t limit, int64_t windowSeconds) {
  if (amount == 0) {
    return true;
  }
  if (windowSeconds <= 0 || amount > limit) {
    return false;
  }
  rollWindow(*this, now, windowSeconds);
  if (count > limit - amount) {
    return false;
  }
  count += amount;
  return true;
}

bool inspectAiAdvertisement(const AiProviderAdvertisement& ad, const boost::uuids::uuid& expectedNetwork, int64_t now,
                            std::string& reason) {
  if (ad.protocolVersion != AI_DISCOVERY_PROTOCOL_VERSION) {
    reason = "unsupported protocol version";
    return false;
  }
  if (ad.networkId != expectedNetwork) {
    reason = "wrong network";
    return false;
  }
  if (!knownStatus(ad.status)) {
    reason = "unknown status";
    return false;
  }
  if (ad.availableSlots > AI_MAX_AVAILABLE_SLOTS) {
    reason = "available slots exceed limit";
    return false;
  }
  if (ad.issuedAt <= 0 || ad.expiresAt <= ad.issuedAt) {
    reason = "invalid lifetime";
    return false;
  }
  if (ad.expiresAt - ad.issuedAt > AI_MAX_ADVERTISEMENT_LIFETIME_SECONDS) {
    reason = "lifetime exceeds maximum";
    return false;
  }
  if (ad.issuedAt > now + AI_MAX_CLOCK_SKEW_SECONDS) {
    reason = "issued too far in the future";
    return false;
  }
  if (ad.expiresAt <= now) {
    reason = "expired";
    return false;
  }
  if (ad.issuedAt < now - AI_MAX_ADVERTISEMENT_LIFETIME_SECONDS - AI_MAX_CLOCK_SKEW_SECONDS) {
    reason = "issued too far in the past";
    return false;
  }
  if (ad.models.size() > AI_MAX_MODELS_PER_PROVIDER) {
    reason = "too many models";
    return false;
  }
  if (ad.status != AI_STATUS_OFFLINE && ad.models.empty()) {
    reason = "available advertisement has no models";
    return false;
  }
  if (ad.contactType == AI_CONTACT_NONE) {
    if (!ad.contactValue.empty()) {
      reason = "empty contact type has a value";
      return false;
    }
    if (!signatureIsZero(ad.addressBindingSignature)) {
      reason = "address binding is not empty";
      return false;
    }
  } else if (ad.contactType == AI_CONTACT_ENDPOINT) {
    if (ad.contactValue.empty() || ad.contactValue.size() > AI_MAX_CONTACT_LENGTH) {
      reason = "invalid contact";
      return false;
    }
    if (!signatureIsZero(ad.addressBindingSignature)) {
      reason = "address binding is not empty";
      return false;
    }
  } else if (ad.contactType == AI_CONTACT_CCX_ADDRESS) {
    if (ad.contactValue.empty() || ad.contactValue.size() > AI_MAX_CONTACT_LENGTH) {
      reason = "invalid contact";
      return false;
    }
    if (!acceptCcxAddress(ad.contactValue, reason)) {
      return false;
    }
    if (signatureIsZero(ad.addressBindingSignature)) {
      reason = "missing address binding";
      return false;
    }
  } else {
    reason = "unknown contact type";
    return false;
  }

  std::string previousModel;
  for (size_t i = 0; i < ad.models.size(); ++i) {
    const AiModelOffer& model = ad.models[i];
    if (model.modelId.empty() || model.modelId.size() > AI_MAX_MODEL_ID_LENGTH) {
      reason = "invalid model id";
      return false;
    }
    if (i > 0 && !(previousModel < model.modelId)) {
      reason = "models are not strictly ordered";
      return false;
    }
    if (!knownBilling(model.billingUnit)) {
      reason = "unknown billing unit";
      return false;
    }
    if (model.modelDigestType == AI_DIGEST_NONE) {
      if (!digestIsZero(model.modelDigest)) {
        reason = "empty digest type has a digest";
        return false;
      }
    } else if (model.modelDigestType == AI_DIGEST_SHA256) {
      if (digestIsZero(model.modelDigest)) {
        reason = "sha256 digest is zero";
        return false;
      }
    } else {
      reason = "unknown model digest type";
      return false;
    }
    if (model.maxContextTokens == 0) {
      reason = "context length is zero";
      return false;
    }
    previousModel = model.modelId;
  }

  if (!crypto::check_key(ad.providerPublicKey)) {
    reason = "invalid provider public key";
    return false;
  }

  std::vector<uint8_t> canonical;
  if (!buildAiAdvertisementCanonical(ad, canonical)) {
    reason = "canonical encoding rejected";
    return false;
  }
  return true;
}

bool aiAdvertisementFailureIsTimeOnly(const std::string& reason) {
  return reason == "expired" || reason == "issued too far in the future" || reason == "issued too far in the past";
}

}
