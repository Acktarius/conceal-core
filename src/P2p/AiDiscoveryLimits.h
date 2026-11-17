// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cn {

// Command ids sit above the P2P pool (1001-1006) and the blockchain pool (2001-2010).
const uint32_t AI_DISCOVERY_COMMANDS_POOL_BASE = 3000;
const uint32_t AI_DISCOVERY_CAPABILITIES_ID = AI_DISCOVERY_COMMANDS_POOL_BASE + 1;
const uint32_t AI_DISCOVERY_ADVERTISEMENTS_ID = AI_DISCOVERY_COMMANDS_POOL_BASE + 2;
const uint32_t AI_DISCOVERY_REQUEST_PROVIDERS_ID = AI_DISCOVERY_COMMANDS_POOL_BASE + 3;

const uint8_t AI_DISCOVERY_PROTOCOL_VERSION = 1;

// Receive cap applied before Levin allocates a discovery body.
// This is not packet_max_size and it does not change the 100 MB cap on other commands.
const size_t AI_DISCOVERY_MAX_MESSAGE_BYTES = 16 * 1024;
const size_t AI_ADVERTISEMENT_MAX_BYTES = 8 * 1024;
const size_t AI_MAX_MODELS_PER_PROVIDER = 8;
const size_t AI_MAX_MODEL_ID_LENGTH = 64;
const size_t AI_MAX_CONTACT_LENGTH = 128;
const uint32_t AI_MAX_REGISTRY_ENTRIES = 1024;
// Kept free of new peer records so a local miner can still register.
// A registry smaller than this still keeps one slot away from peers when it holds more than one record.
const uint32_t AI_LOCAL_RESERVED_ENTRIES = 64;
const uint32_t AI_MAX_AVAILABLE_SLOTS = 1024;
const int64_t AI_MAX_ADVERTISEMENT_LIFETIME_SECONDS = 30 * 60;
const int64_t AI_MAX_CLOCK_SKEW_SECONDS = 2 * 60;

const size_t AI_MAX_ADS_PER_MESSAGE = 8;
const uint16_t AI_MAX_PAGE_RECORDS = 8;
const size_t AI_MAX_PAGE_BYTES = 12 * 1024;
const uint32_t AI_MAX_SYNC_PAGES = 8;
const uint32_t AI_MAX_SYNC_BYTES = 64 * 1024;
const uint32_t AI_MAX_CONCURRENT_SYNCS = 4;
const int64_t AI_REQUEST_TIMEOUT_SECONDS = 20;
const int64_t AI_SYNC_BACKOFF_SECONDS = 60;
const int64_t AI_SYNC_ROUND_INTERVAL_SECONDS = 5 * 60;
// A round that stops on the page or byte budget waits this long and keeps its cursor.
const int64_t AI_SYNC_CONTINUATION_INTERVAL_SECONDS = AI_SYNC_ROUND_INTERVAL_SECONDS;
const int64_t AI_PROBE_RETRY_BACKOFF_SECONDS = AI_SYNC_BACKOFF_SECONDS;
const uint8_t AI_MAX_CAPABILITY_PROBES = 2;
const int64_t AI_RATE_WINDOW_SECONDS = 60;

const uint32_t AI_MAX_LOCAL_REGISTRATIONS_PER_MINUTE = 30;
const uint32_t AI_MAX_UPDATES_PER_PEER_PER_MINUTE = 32;
const uint32_t AI_MAX_QUERIES_PER_PEER_PER_MINUTE = 4;
const uint32_t AI_MAX_SIGNATURES_PER_PEER_PER_MINUTE = 64;

// Discovery bytes waiting in the existing per-connection write queue.
const size_t AI_DISCOVERY_MAX_QUEUE_BYTES = 32 * 1024;
// Do not append discovery once blockchain traffic already occupies this much of the write queue.
const size_t AI_DISCOVERY_YIELD_QUEUE_BYTES = 1 * 1024 * 1024;

const uint8_t AI_MAX_PROBE_ENQUEUE_FAILURES = 3;

const char AI_REGISTER_RPC_PATH[] = "/registeraiprovider";
const char AI_GET_PROVIDERS_RPC_PATH[] = "/getaiproviders";

inline bool isAiDiscoveryCommand(uint32_t command) {
  return command == AI_DISCOVERY_CAPABILITIES_ID || command == AI_DISCOVERY_ADVERTISEMENTS_ID ||
         command == AI_DISCOVERY_REQUEST_PROVIDERS_ID;
}

// True only for discovery commands over the receive cap. Other commands are unchanged.
inline bool aiDiscoveryBodyExceedsCap(uint32_t command, uint64_t bodyBytes) {
  return isAiDiscoveryCommand(command) && bodyBytes > AI_DISCOVERY_MAX_MESSAGE_BYTES;
}

inline bool aiDiscoveryEnqueueAllowed(size_t discoveryQueuedBytes, size_t messageBytes, size_t totalWriteQueuedBytes) {
  if (messageBytes == 0 || messageBytes > AI_DISCOVERY_MAX_MESSAGE_BYTES) {
    return false;
  }
  if (discoveryQueuedBytes > AI_DISCOVERY_MAX_QUEUE_BYTES ||
      messageBytes > AI_DISCOVERY_MAX_QUEUE_BYTES - discoveryQueuedBytes) {
    return false;
  }
  if (totalWriteQueuedBytes >= AI_DISCOVERY_YIELD_QUEUE_BYTES) {
    return false;
  }
  return true;
}

inline bool aiDiscoveryRpcBodyCap(const std::string& url, size_t& cap) {
  if (url == AI_REGISTER_RPC_PATH || url == AI_GET_PROVIDERS_RPC_PATH) {
    cap = AI_DISCOVERY_MAX_MESSAGE_BYTES;
    return true;
  }
  return false;
}

// Host-order IPv4. Matches platform_system::Ipv4Address::isLoopback (127.0.0.0/8).
inline bool isLoopbackIpv4HostOrder(uint32_t value) {
  return (value & 0xff000000u) == (127u << 24);
}

enum AiRegistrationAccess { aiRegistrationDisabled = 0, aiRegistrationForbidden = 1, aiRegistrationAllowed = 2 };

inline AiRegistrationAccess aiRegistrationAccess(bool enabled, uint32_t peerHostOrder) {
  if (!enabled) {
    return aiRegistrationDisabled;
  }
  if (!isLoopbackIpv4HostOrder(peerHostOrder)) {
    return aiRegistrationForbidden;
  }
  return aiRegistrationAllowed;
}

}
