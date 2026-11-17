// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <functional>
#include <vector>

#include <boost/uuid/uuid.hpp>

#include "AiDiscoveryProtocolDefinitions.h"
#include "AiProviderRegistry.h"
#include "AiProviderValidation.h"
#include "LevinProtocol.h"

namespace cn {

typedef std::function<bool(const AiOutbound&)> AiEnqueueFn;
typedef std::function<void(const AiProviderAdvertisement&)> AiRelayFn;

struct AiPeerRef {
  boost::uuids::uuid connectionId;
  AiConnectionState* state;
  bool handshakeComplete;
  AiEnqueueFn enqueue;

  AiPeerRef() : state(nullptr), handshakeComplete(false) {}
};

// Discovery state for one daemon. Call it on the dispatcher thread only.
class AiDiscoveryService {
public:
  AiDiscoveryService();

  void setEnabled(bool enabled);
  bool enabled() const { return m_enabled; }
  void setNetworkId(const boost::uuids::uuid& networkId);
  void shutdown();
  bool stopped() const { return m_stopped; }

  AiIngestResult registerLocal(const AiProviderAdvertisement& ad, int64_t now);
  AiProviderPage getProviders(const AiProviderQuery& query, int64_t now) const;
  uint64_t listedCount(int64_t now) const;

  bool shouldRelayTo(const AiConnectionState& state) const;

  int handleCapabilities(const BinaryArray& in, BinaryArray& out, AiConnectionState& state, int64_t now);
  void handleCapabilityResponse(const BinaryArray& in, AiConnectionState& state, int64_t now, const AiEnqueueFn& enqueue,
                               int32_t returnCode = LEVIN_PROTOCOL_RETCODE_SUCCESS);
  void handleAdvertisements(const BinaryArray& in, AiConnectionState& state, int64_t now, const AiRelayFn& relay);
  int handleProvidersRequest(const BinaryArray& in, BinaryArray& out, AiConnectionState& state, int64_t now);
  void handleProvidersResponse(const BinaryArray& in, AiConnectionState& state, int64_t now, const AiEnqueueFn& enqueue,
                               const AiRelayFn& relay);

  bool requestAiProviderPage(AiConnectionState& state, int64_t now, const AiEnqueueFn& enqueue);
  void maintain(int64_t now, const std::vector<AiPeerRef>& peers);
  void onDisconnect(AiConnectionState& state);

private:
  void clearOutstanding(AiConnectionState& state);
  void backoff(AiConnectionState& state, int64_t now);
  bool charge(int64_t& windowStart, uint32_t& count, int64_t now, uint32_t amount, uint32_t limit);
  bool ingestPeerBatch(const std::vector<AiProviderAdvertisement>& advertisements, AiConnectionState& state, int64_t now,
                       const AiRelayFn& relay);
  void noteSupported(AiConnectionState& state, int64_t now);
  void maybeProbe(AiConnectionState& state, int64_t now, bool handshakeComplete, const AiEnqueueFn& enqueue);

  AiProviderRegistry m_registry;
  AiRateWindow m_localRegistrations;
  bool m_enabled;
  bool m_stopped;
  uint32_t m_activeSyncs;
};

}
