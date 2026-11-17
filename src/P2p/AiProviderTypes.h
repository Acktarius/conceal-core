// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include "AiDiscoveryLimits.h"
#include "CryptoNote.h"
#include "P2pProtocolDefinitions.h"
#include "crypto/crypto.h"

namespace cn {

inline void rejectOversizedDiscoveryString(ISerializer& s, std::string& value, size_t maxLen) {
  if (s.type() == ISerializer::INPUT && value.size() > maxLen) {
    value.clear();
    throw std::runtime_error("discovery string exceeds limit");
  }
}

// Checks the collection count before resizing the destination vector.
// The KV parser may already have built a JsonValue from a body that passed the receive cap.
template <typename T, size_t Max>
void serializeBoundedVector(std::vector<T>& value, common::StringView name, ISerializer& serializer) {
  if (serializer.type() == ISerializer::OUTPUT && value.size() > Max) {
    throw std::runtime_error("discovery collection exceeds limit");
  }

  size_t size = value.size();
  if (!serializer.beginArray(size, name)) {
    value.clear();
    return;
  }

  if (serializer.type() == ISerializer::INPUT && size > Max) {
    throw std::runtime_error("discovery collection exceeds limit");
  }

  value.resize(size);
  for (size_t i = 0; i < value.size(); ++i) {
    serializer(value[i], "");
  }
  serializer.endArray();
}



const uint8_t AI_STATUS_AVAILABLE = 1;
const uint8_t AI_STATUS_BUSY = 2;
const uint8_t AI_STATUS_OFFLINE = 3;

const uint8_t AI_BILLING_PER_REQUEST = 1;
const uint8_t AI_BILLING_PER_THOUSAND_TOKENS = 2;
const uint8_t AI_BILLING_PER_SECOND = 3;

const uint8_t AI_CONTACT_NONE = 0;
const uint8_t AI_CONTACT_ENDPOINT = 1;
const uint8_t AI_CONTACT_CCX_ADDRESS = 2;

const uint8_t AI_DIGEST_NONE = 0;
const uint8_t AI_DIGEST_SHA256 = 1;
const size_t AI_MODEL_DIGEST_BYTES = 32;

const uint8_t AI_SOURCE_LOCAL = 1;
const uint8_t AI_SOURCE_PEER = 2;

const uint8_t AI_DIRECTORY_OK = 1;
const uint8_t AI_DIRECTORY_RATE_LIMITED = 2;
const uint8_t AI_DIRECTORY_INVALID = 3;
const uint8_t AI_DIRECTORY_UNSUPPORTED = 4;

const uint8_t AI_PEER_UNKNOWN = 0;
const uint8_t AI_PEER_PROBE_SENT = 1;
const uint8_t AI_PEER_SUPPORTED = 2;
const uint8_t AI_PEER_UNSUPPORTED = 3;

enum class AiAcceptResult { accepted, duplicate, stale, invalid, capacityRejected, rateLimited };

enum class AiRecordClass { timeSkipped, structuralInvalid, stale, duplicate, conflict, needsVerification };

struct AiRecordAssessment {
  AiRecordClass kind;
  uint32_t signatureCount;
  std::string reason;

  AiRecordAssessment() : kind(AiRecordClass::structuralInvalid), signatureCount(0) {}
};

struct AiIngestResult {
  AiAcceptResult result;
  bool relay;
  std::string reason;

  AiIngestResult() : result(AiAcceptResult::invalid), relay(false) {}
};

const char* aiAcceptResultName(AiAcceptResult result);

struct AiModelOffer {
  std::string modelId;
  uint32_t maxContextTokens;
  uint64_t priceAtomicUnits;
  uint8_t billingUnit;
  uint8_t modelDigestType;
  std::array<uint8_t, AI_MODEL_DIGEST_BYTES> modelDigest;

  AiModelOffer() : maxContextTokens(0), priceAtomicUnits(0), billingUnit(0), modelDigestType(AI_DIGEST_NONE) {
    modelDigest.fill(0);
  }

  void serialize(ISerializer& s) {
    s(modelId, "modelId");
    rejectOversizedDiscoveryString(s, modelId, AI_MAX_MODEL_ID_LENGTH);
    s(maxContextTokens, "maxContextTokens");
    s(priceAtomicUnits, "priceAtomicUnits");
    s(billingUnit, "billingUnit");
    s(modelDigestType, "modelDigestType");
    s(modelDigest, "modelDigest");
  }
};

struct AiProviderAdvertisement {
  uint8_t protocolVersion;
  boost::uuids::uuid networkId;
  crypto::PublicKey providerPublicKey;
  uint64_t sequence;
  int64_t issuedAt;
  int64_t expiresAt;
  uint8_t status;
  uint32_t availableSlots;
  std::vector<AiModelOffer> models;
  uint8_t contactType;
  std::string contactValue;
  crypto::Signature addressBindingSignature;
  crypto::Signature signature;

  AiProviderAdvertisement()
      : protocolVersion(0), sequence(0), issuedAt(0), expiresAt(0), status(0), availableSlots(0), contactType(0) {
    memset(networkId.data, 0, sizeof(networkId.data));
    memset(providerPublicKey.data, 0, sizeof(providerPublicKey.data));
    memset(addressBindingSignature.data, 0, sizeof(addressBindingSignature.data));
    memset(signature.data, 0, sizeof(signature.data));
  }

  void serialize(ISerializer& s) {
    s(protocolVersion, "protocolVersion");
    s(networkId, "networkId");
    s(providerPublicKey, "providerPublicKey");
    s(sequence, "sequence");
    s(issuedAt, "issuedAt");
    s(expiresAt, "expiresAt");
    s(status, "status");
    s(availableSlots, "availableSlots");
    serializeBoundedVector<AiModelOffer, AI_MAX_MODELS_PER_PROVIDER>(models, "models", s);
    s(contactType, "contactType");
    s(contactValue, "contactValue");
    rejectOversizedDiscoveryString(s, contactValue, AI_MAX_CONTACT_LENGTH);
    s(addressBindingSignature, "addressBindingSignature");
    s(signature, "signature");
  }
};

// Local metadata. Not part of the signed advertisement.
struct AiProviderObservation {
  int64_t firstSeen;
  int64_t lastSeen;
  uint8_t source;

  AiProviderObservation() : firstSeen(0), lastSeen(0), source(0) {}

  void serialize(ISerializer& s) {
    s(firstSeen, "localFirstSeen");
    s(lastSeen, "localLastSeen");
    s(source, "localSource");
  }
};

struct AiProviderListing {
  AiProviderAdvertisement advertisement;
  AiProviderObservation observation;

  void serialize(ISerializer& s) {
    s(advertisement, "advertisement");
    s(observation, "observation");
  }
};

struct AiProviderQuery {
  std::string modelId;
  uint8_t status;
  uint32_t minContextTokens;
  uint8_t billingUnit;
  uint64_t cursor;
  uint16_t limit;

  AiProviderQuery() : status(0), minContextTokens(0), billingUnit(0), cursor(0), limit(0) {}
};

struct AiProviderPage {
  std::vector<AiProviderListing> providers;
  uint64_t nextCursor;
  bool done;

  AiProviderPage() : nextCursor(0), done(true) {}
};

struct AiConnectionState {
  uint8_t capability;
  int64_t probeDeadline;
  uint8_t probeSends;
  uint8_t probeEnqueueFailures;
  bool hasOutstanding;
  uint64_t outstandingRequestId;
  uint64_t outstandingCursor;
  int64_t outstandingDeadline;
  uint64_t cursor;
  uint32_t pagesThisRound;
  uint32_t bytesThisRound;
  int64_t nextSyncAt;
  int64_t updateWindowStart;
  uint32_t updatesInWindow;
  int64_t queryWindowStart;
  uint32_t queriesInWindow;
  int64_t signatureWindowStart;
  uint32_t signaturesInWindow;

  AiConnectionState()
      : capability(AI_PEER_UNKNOWN),
        probeDeadline(0),
        probeSends(0),
        probeEnqueueFailures(0),
        hasOutstanding(false),
        outstandingRequestId(0),
        outstandingCursor(0),
        outstandingDeadline(0),
        cursor(0),
        pagesThisRound(0),
        bytesThisRound(0),
        nextSyncAt(0),
        updateWindowStart(0),
        updatesInWindow(0),
        queryWindowStart(0),
        queriesInWindow(0),
        signatureWindowStart(0),
        signaturesInWindow(0) {}
};

struct AiOutbound {
  uint32_t command;
  BinaryArray body;
  bool request;

  AiOutbound() : command(0), request(false) {}
};

struct AiVerificationStats {
  uint32_t providerSignatures;
  uint32_t bindingSignatures;

  AiVerificationStats() : providerSignatures(0), bindingSignatures(0) {}
};

void resetAiVerificationStats();
AiVerificationStats aiVerificationStats();

// Canonical signing bytes. Little-endian integers, length-prefixed strings, domain separated.
// Excludes the provider signature. Includes addressBindingSignature. Does not use C++ layout or JSON field order.
bool buildAiAdvertisementCanonical(const AiProviderAdvertisement& ad, std::vector<uint8_t>& out);
crypto::Hash aiAdvertisementSigningHash(const std::vector<uint8_t>& canonical);
bool signAiAdvertisement(AiProviderAdvertisement& ad, const crypto::SecretKey& secret);
bool verifyAiAdvertisementSignature(const AiProviderAdvertisement& ad, std::vector<uint8_t>& canonicalOut);

// Binding covers the service key and the address only. The miner can build it once, offline.
bool buildAiAddressBindingCanonical(const boost::uuids::uuid& networkId, const crypto::PublicKey& providerPublicKey,
                                    const std::string& address, std::vector<uint8_t>& out);
bool signAiAddressBinding(const boost::uuids::uuid& networkId, const crypto::PublicKey& providerPublicKey,
                          const std::string& address, const crypto::PublicKey& spendPublicKey,
                          const crypto::SecretKey& spendSecretKey, crypto::Signature& out);
bool verifyAiAddressBinding(const boost::uuids::uuid& networkId, const crypto::PublicKey& providerPublicKey,
                            const std::string& address, const crypto::Signature& signature);

}
