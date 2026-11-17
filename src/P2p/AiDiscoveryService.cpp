// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "AiDiscoveryService.h"

#include <algorithm>

#include "LevinProtocol.h"
#include "P2pNetworks.h"
#include "AiProviderValidation.h"
#include "crypto/crypto.h"

namespace cn {
namespace {

void finishRound(AiConnectionState& state, int64_t now) {
  state.cursor = 0;
  state.pagesThisRound = 0;
  state.bytesThisRound = 0;
  state.nextSyncAt = now + AI_SYNC_ROUND_INTERVAL_SECONDS;
}

void pauseRound(AiConnectionState& state, int64_t now) {
  state.pagesThisRound = 0;
  state.bytesThisRound = 0;
  state.nextSyncAt = now + AI_SYNC_CONTINUATION_INTERVAL_SECONDS;
}

}

AiDiscoveryService::AiDiscoveryService()
    : m_registry(CRYPTONOTE_NETWORK), m_enabled(false), m_stopped(false), m_activeSyncs(0) {}

void AiDiscoveryService::setEnabled(bool enabled) {
  m_enabled = enabled;
}

void AiDiscoveryService::setNetworkId(const boost::uuids::uuid& networkId) {
  m_registry.setNetworkId(networkId);
}

void AiDiscoveryService::shutdown() {
  m_stopped = true;
}

AiIngestResult AiDiscoveryService::registerLocal(const AiProviderAdvertisement& ad, int64_t now) {
  AiIngestResult result;
  if (!m_enabled || m_stopped) {
    result.result = AiAcceptResult::invalid;
    result.reason = "discovery disabled";
    return result;
  }
  if (!inspectAiAdvertisement(ad, m_registry.networkId(), now, result.reason)) {
    result.result = AiAcceptResult::invalid;
    return result;
  }
  if (!m_localRegistrations.allow(now, AI_MAX_LOCAL_REGISTRATIONS_PER_MINUTE, AI_RATE_WINDOW_SECONDS)) {
    result.result = AiAcceptResult::rateLimited;
    result.reason = "registration rate limited";
    return result;
  }
  return m_registry.accept(ad, now, AI_SOURCE_LOCAL);
}

AiProviderPage AiDiscoveryService::getProviders(const AiProviderQuery& query, int64_t now) const {
  return m_registry.query(query, now);
}

uint64_t AiDiscoveryService::listedCount(int64_t now) const {
  if (!m_enabled) {
    return 0;
  }
  return m_registry.listedCount(now);
}

bool AiDiscoveryService::shouldRelayTo(const AiConnectionState& state) const {
  return m_enabled && !m_stopped && state.capability == AI_PEER_SUPPORTED;
}

void AiDiscoveryService::clearOutstanding(AiConnectionState& state) {
  if (!state.hasOutstanding) {
    return;
  }
  state.hasOutstanding = false;
  state.outstandingRequestId = 0;
  if (m_activeSyncs > 0) {
    --m_activeSyncs;
  }
}

void AiDiscoveryService::backoff(AiConnectionState& state, int64_t now) {
  state.nextSyncAt = now + AI_SYNC_BACKOFF_SECONDS;
}

bool AiDiscoveryService::charge(int64_t& windowStart, uint32_t& count, int64_t now, uint32_t amount, uint32_t limit) {
  AiRateWindow window;
  window.windowStart = windowStart;
  window.count = count;
  const bool allowed = window.allowMany(now, amount, limit, AI_RATE_WINDOW_SECONDS);
  windowStart = window.windowStart;
  count = window.count;
  return allowed;
}

void AiDiscoveryService::noteSupported(AiConnectionState& state, int64_t now) {
  if (state.capability != AI_PEER_SUPPORTED) {
    state.capability = AI_PEER_SUPPORTED;
    if (state.nextSyncAt == 0) {
      state.nextSyncAt = now;
    }
  }
}

int AiDiscoveryService::handleCapabilities(const BinaryArray& in, BinaryArray& out, AiConnectionState& state, int64_t now) {
  out.clear();
  if (m_stopped) {
    return static_cast<int>(LevinError::ERROR_FORMAT);
  }

  COMMAND_AI_DISCOVERY_CAPABILITIES::request req;
  if (!LevinProtocol::decode(in, req)) {
    return static_cast<int>(LevinError::ERROR_FORMAT);
  }

  COMMAND_AI_DISCOVERY_CAPABILITIES::response rsp;
  rsp.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  rsp.maxPageRecords = AI_MAX_PAGE_RECORDS;
  rsp.maxMessageBytes = static_cast<uint32_t>(AI_DISCOVERY_MAX_MESSAGE_BYTES);
  rsp.discoverySupported = m_enabled && req.protocolVersion == AI_DISCOVERY_PROTOCOL_VERSION;
  if (rsp.discoverySupported) {
    noteSupported(state, now);
  }
  out = LevinProtocol::encode(rsp);
  return LEVIN_PROTOCOL_RETCODE_SUCCESS;
}

void AiDiscoveryService::maybeProbe(AiConnectionState& state, int64_t now, bool handshakeComplete, const AiEnqueueFn& enqueue) {
  if (!handshakeComplete || !enqueue) {
    return;
  }
  if (state.capability == AI_PEER_SUPPORTED || state.capability == AI_PEER_UNSUPPORTED) {
    return;
  }
  if (state.capability == AI_PEER_PROBE_SENT) {
    if (now < state.probeDeadline) {
      return;
    }
    if (state.probeSends >= AI_MAX_CAPABILITY_PROBES) {
      state.capability = AI_PEER_UNSUPPORTED;
      return;
    }
    state.capability = AI_PEER_UNKNOWN;
    state.probeDeadline = now + AI_PROBE_RETRY_BACKOFF_SECONDS;
    return;
  }
  if (now < state.probeDeadline) {
    return;
  }
  if (state.probeSends >= AI_MAX_CAPABILITY_PROBES) {
    state.capability = AI_PEER_UNSUPPORTED;
    return;
  }

  COMMAND_AI_DISCOVERY_CAPABILITIES::request req;
  req.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  AiOutbound outbound;
  outbound.command = COMMAND_AI_DISCOVERY_CAPABILITIES::ID;
  outbound.body = LevinProtocol::encode(req);
  outbound.request = true;
  if (!enqueue(outbound)) {
    ++state.probeEnqueueFailures;
    if (state.probeEnqueueFailures >= AI_MAX_PROBE_ENQUEUE_FAILURES) {
      state.capability = AI_PEER_UNSUPPORTED;
    }
    return;
  }

  ++state.probeSends;
  state.capability = AI_PEER_PROBE_SENT;
  state.probeDeadline = now + AI_REQUEST_TIMEOUT_SECONDS;
}

void AiDiscoveryService::handleCapabilityResponse(const BinaryArray& in, AiConnectionState& state, int64_t now,
                                                  const AiEnqueueFn& enqueue, int32_t returnCode) {
  if (m_stopped || state.capability != AI_PEER_PROBE_SENT) {
    return;
  }

  if (returnCode == static_cast<int32_t>(LevinError::ERROR_CONNECTION_HANDLER_NOT_DEFINED)) {
    state.capability = AI_PEER_UNSUPPORTED;
    return;
  }

  COMMAND_AI_DISCOVERY_CAPABILITIES::response rsp;
  if (!LevinProtocol::decode(in, rsp) || !rsp.discoverySupported || rsp.protocolVersion != AI_DISCOVERY_PROTOCOL_VERSION) {
    state.capability = AI_PEER_UNSUPPORTED;
    return;
  }

  noteSupported(state, now);
  state.nextSyncAt = now;
  requestAiProviderPage(state, now, enqueue);
}

bool AiDiscoveryService::ingestPeerBatch(const std::vector<AiProviderAdvertisement>& advertisements,
                                         AiConnectionState& state, int64_t now, const AiRelayFn& relay) {
  struct Prepared {
    AiProviderAdvertisement advertisement;
    AiRecordAssessment assessment;
    std::vector<uint8_t> canonical;
  };

  std::vector<Prepared> prepared;
  prepared.reserve(advertisements.size());
  for (size_t i = 0; i < advertisements.size(); ++i) {
    Prepared item;
    item.advertisement = advertisements[i];
    item.assessment = m_registry.assess(item.advertisement, now);
    if (item.assessment.kind == AiRecordClass::timeSkipped) {
      continue;
    }
    if (item.assessment.kind == AiRecordClass::structuralInvalid) {
      return false;
    }
    prepared.push_back(item);
  }
  if (prepared.empty()) {
    return true;
  }

  if (!charge(state.updateWindowStart, state.updatesInWindow, now, static_cast<uint32_t>(prepared.size()),
              AI_MAX_UPDATES_PER_PEER_PER_MINUTE)) {
    return false;
  }

  for (size_t i = 0; i < prepared.size(); ++i) {
    Prepared& item = prepared[i];
    if (item.assessment.kind != AiRecordClass::needsVerification) {
      continue;
    }
    if (!charge(state.signatureWindowStart, state.signaturesInWindow, now, item.assessment.signatureCount,
                AI_MAX_SIGNATURES_PER_PEER_PER_MINUTE)) {
      return false;
    }
    if (!m_registry.verifyRecord(item.advertisement, item.canonical, item.assessment.reason)) {
      return false;
    }
  }

  bool spoke = false;
  for (size_t i = 0; i < prepared.size(); ++i) {
    const Prepared& item = prepared[i];
    AiIngestResult ingested;
    if (item.assessment.kind == AiRecordClass::needsVerification) {
      ingested = m_registry.acceptVerified(item.advertisement, item.canonical, now, AI_SOURCE_PEER);
    } else if (item.assessment.kind == AiRecordClass::stale) {
      ingested.result = AiAcceptResult::stale;
      ingested.reason = item.assessment.reason;
    } else if (item.assessment.kind == AiRecordClass::duplicate) {
      ingested.result = AiAcceptResult::duplicate;
      ingested.reason = item.assessment.reason;
    } else {
      ingested.result = AiAcceptResult::invalid;
      ingested.reason = item.assessment.reason;
    }
    if (ingested.result != AiAcceptResult::invalid) {
      spoke = true;
    }
    if (ingested.relay && relay) {
      relay(item.advertisement);
    }
  }
  if (spoke) {
    noteSupported(state, now);
  }
  return true;
}

void AiDiscoveryService::handleAdvertisements(const BinaryArray& in, AiConnectionState& state, int64_t now, const AiRelayFn& relay) {
  if (!m_enabled || m_stopped) {
    return;
  }

  NOTIFY_AI_PROVIDER_ADVERTISEMENTS::request req;
  if (!LevinProtocol::decode(in, req)) {
    return;
  }
  if (req.protocolVersion != AI_DISCOVERY_PROTOCOL_VERSION || req.advertisements.empty() ||
      req.advertisements.size() > AI_MAX_ADS_PER_MESSAGE) {
    return;
  }

  ingestPeerBatch(req.advertisements, state, now, relay);
}

int AiDiscoveryService::handleProvidersRequest(const BinaryArray& in, BinaryArray& out, AiConnectionState& state, int64_t now) {
  out.clear();
  if (m_stopped) {
    return static_cast<int>(LevinError::ERROR_FORMAT);
  }

  COMMAND_REQUEST_AI_PROVIDERS::request req;
  if (!LevinProtocol::decode(in, req)) {
    return static_cast<int>(LevinError::ERROR_FORMAT);
  }

  COMMAND_REQUEST_AI_PROVIDERS::response rsp;
  rsp.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  rsp.requestId = req.requestId;
  rsp.nextCursor = req.cursor;
  rsp.done = true;
  rsp.result = AI_DIRECTORY_UNSUPPORTED;

  if (!m_enabled || req.protocolVersion != AI_DISCOVERY_PROTOCOL_VERSION) {
    out = LevinProtocol::encode(rsp);
    return LEVIN_PROTOCOL_RETCODE_SUCCESS;
  }

  noteSupported(state, now);
  if (!charge(state.queryWindowStart, state.queriesInWindow, now, 1, AI_MAX_QUERIES_PER_PEER_PER_MINUTE)) {
    rsp.result = AI_DIRECTORY_RATE_LIMITED;
    rsp.done = false;
    out = LevinProtocol::encode(rsp);
    return LEVIN_PROTOCOL_RETCODE_SUCCESS;
  }

  std::vector<AiProviderAdvertisement> page;
  uint64_t endCursor = req.cursor;
  bool end = true;
  uint16_t limit = req.maxRecords == 0 ? AI_MAX_PAGE_RECORDS : req.maxRecords;
  m_registry.directoryPage(req.cursor, limit, now, page, endCursor, end);

  rsp.result = AI_DIRECTORY_OK;
  rsp.advertisements.clear();
  uint32_t included = 0;
  for (size_t i = 0; i < page.size(); ++i) {
    rsp.advertisements.push_back(page[i]);
    const BinaryArray trial = LevinProtocol::encode(rsp);
    if (trial.size() > AI_MAX_PAGE_BYTES || trial.size() > AI_DISCOVERY_MAX_MESSAGE_BYTES) {
      rsp.advertisements.pop_back();
      break;
    }
    ++included;
  }

  if (rsp.advertisements.empty() && !page.empty()) {
    rsp.nextCursor = req.cursor + 1;
    rsp.done = end && page.size() == 1;
  } else {
    rsp.nextCursor = req.cursor + included;
    rsp.done = end && included == page.size();
  }
  out = LevinProtocol::encode(rsp);
  return LEVIN_PROTOCOL_RETCODE_SUCCESS;
}

void AiDiscoveryService::handleProvidersResponse(const BinaryArray& in, AiConnectionState& state, int64_t now,
                                                 const AiEnqueueFn& enqueue, const AiRelayFn& relay) {
  if (!state.hasOutstanding) {
    return;
  }

  COMMAND_REQUEST_AI_PROVIDERS::response rsp;
  if (!LevinProtocol::decode(in, rsp)) {
    clearOutstanding(state);
    backoff(state, now);
    return;
  }
  if (rsp.requestId != state.outstandingRequestId) {
    return;
  }

  clearOutstanding(state);
  if (rsp.result == AI_DIRECTORY_UNSUPPORTED || rsp.protocolVersion != AI_DISCOVERY_PROTOCOL_VERSION) {
    state.capability = AI_PEER_UNSUPPORTED;
    return;
  }
  if (rsp.result != AI_DIRECTORY_OK || rsp.advertisements.size() > AI_MAX_PAGE_RECORDS) {
    backoff(state, now);
    return;
  }
  if (!rsp.done && rsp.nextCursor <= state.cursor) {
    backoff(state, now);
    return;
  }

  if (!rsp.advertisements.empty() && !ingestPeerBatch(rsp.advertisements, state, now, relay)) {
    backoff(state, now);
    return;
  }

  if (in.size() > 0 && state.bytesThisRound <= AI_MAX_SYNC_BYTES) {
    const uint32_t room = AI_MAX_SYNC_BYTES - state.bytesThisRound;
    const uint32_t add = static_cast<uint32_t>(std::min(in.size(), static_cast<size_t>(room)));
    state.bytesThisRound += add;
  } else if (in.size() > 0) {
    state.bytesThisRound = AI_MAX_SYNC_BYTES;
  }

  if (rsp.done) {
    finishRound(state, now);
    return;
  }

  state.cursor = rsp.nextCursor;
  requestAiProviderPage(state, now, enqueue);
}

bool AiDiscoveryService::requestAiProviderPage(AiConnectionState& state, int64_t now, const AiEnqueueFn& enqueue) {
  if (!m_enabled || m_stopped || !enqueue || state.capability != AI_PEER_SUPPORTED || state.hasOutstanding) {
    return false;
  }
  if (now < state.nextSyncAt) {
    return false;
  }
  if (state.pagesThisRound >= AI_MAX_SYNC_PAGES || state.bytesThisRound >= AI_MAX_SYNC_BYTES) {
    pauseRound(state, now);
    return false;
  }
  if (m_activeSyncs >= AI_MAX_CONCURRENT_SYNCS) {
    backoff(state, now);
    return false;
  }

  uint64_t requestId = crypto::rand<uint64_t>();
  if (requestId == 0) {
    requestId = 1;
  }

  COMMAND_REQUEST_AI_PROVIDERS::request req;
  req.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  req.requestId = requestId;
  req.cursor = state.cursor;
  req.maxRecords = AI_MAX_PAGE_RECORDS;

  AiOutbound outbound;
  outbound.command = COMMAND_REQUEST_AI_PROVIDERS::ID;
  outbound.body = LevinProtocol::encode(req);
  outbound.request = true;
  if (outbound.body.size() > AI_DISCOVERY_MAX_MESSAGE_BYTES || !enqueue(outbound)) {
    backoff(state, now);
    return false;
  }

  state.hasOutstanding = true;
  state.outstandingRequestId = requestId;
  state.outstandingCursor = state.cursor;
  state.outstandingDeadline = now + AI_REQUEST_TIMEOUT_SECONDS;
  ++state.pagesThisRound;
  ++m_activeSyncs;
  return true;
}

void AiDiscoveryService::maintain(int64_t now, const std::vector<AiPeerRef>& peers) {
  if (!m_enabled || m_stopped) {
    return;
  }
  m_registry.expire(now);
  for (size_t i = 0; i < peers.size(); ++i) {
    AiConnectionState* state = peers[i].state;
    if (state == nullptr) {
      continue;
    }
    maybeProbe(*state, now, peers[i].handshakeComplete, peers[i].enqueue);
    if (state->hasOutstanding && now >= state->outstandingDeadline) {
      clearOutstanding(*state);
      backoff(*state, now);
    }
    if (peers[i].handshakeComplete) {
      requestAiProviderPage(*state, now, peers[i].enqueue);
    }
  }
}

void AiDiscoveryService::onDisconnect(AiConnectionState& state) {
  clearOutstanding(state);
}

}
