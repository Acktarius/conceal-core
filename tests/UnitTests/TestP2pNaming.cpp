// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include "P2p/ConnectionContext.h"
#include "P2p/NetNodeCommon.h"
#include "P2p/P2pProtocolDefinitions.h"
#include "Serialization/SerializationTools.h"

using namespace cn;

namespace {

struct P2pStateConfigSample {
  network_config m_netConfig{};
  uint64_t m_peerId = 0;

  void serialize(ISerializer &s) {
    // Stable on-disk keys (legacy snake_case member names).
    s(m_netConfig, "m_net_config");
    s(m_peerId, "m_peer_id");
  }
};

} // namespace

TEST(P2pNaming, Ip2pEndpointStubUsesCamelCaseApi) {
  p2p_endpoint_stub stub;
  BinaryArray empty;
  CryptoNoteConnectionContext ctx{};

  EXPECT_EQ(0u, stub.getConnectionsCount());
  EXPECT_TRUE(stub.invokeNotifyToPeer(0, empty, ctx));
  stub.relayNotifyToAll(0, empty, nullptr);
  stub.forEachConnection([](CryptoNoteConnectionContext &, PeerIdType) {});
  stub.dropConnection(ctx, false);
  stub.externalRelayNotifyToAll(0, empty, nullptr);
  stub.externalRelayNotifyToList(0, empty, {});
}

TEST(P2pNaming, ConfigSerializeKeepsLegacyKeys) {
  P2pStateConfigSample original;
  original.m_netConfig.connections_count = 8;
  original.m_netConfig.handshake_interval = 60;
  original.m_netConfig.packet_max_size = 1000000;
  original.m_netConfig.config_id = 1;
  original.m_peerId = 0xAABBCCDDEEFF0011ull;

  const std::string blob = storeToBinaryKeyValue(original);
  ASSERT_FALSE(blob.empty());

  P2pStateConfigSample loaded;
  ASSERT_TRUE(loadFromBinaryKeyValue(loaded, blob));
  EXPECT_EQ(original.m_peerId, loaded.m_peerId);
  EXPECT_EQ(original.m_netConfig.connections_count, loaded.m_netConfig.connections_count);
  EXPECT_EQ(original.m_netConfig.handshake_interval, loaded.m_netConfig.handshake_interval);
  EXPECT_EQ(original.m_netConfig.packet_max_size, loaded.m_netConfig.packet_max_size);
  EXPECT_EQ(original.m_netConfig.config_id, loaded.m_netConfig.config_id);
}
