// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 The TurtleCoin developers
// Copyright (c) 2016-2020 The Karbo developers
// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <functional>
#include <unordered_map>

#include <boost/functional/hash.hpp>
#include <list>
#include <boost/uuid/uuid.hpp>

#include <System/Context.h>
#include <System/ContextGroup.h>
#include <System/Dispatcher.h>
#include <System/Event.h>
#include <System/Timer.h>
#include <System/TcpConnection.h>
#include <System/TcpListener.h>

#include "CryptoNoteCore/OnceInInterval.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "Common/CommandLine.h"
#include "Logging/LoggerRef.h"

#include "ConnectionContext.h"
#include "LevinProtocol.h"
#include "NetNodeCommon.h"
#include "NetNodeConfig.h"
#include "P2pProtocolDefinitions.h"
#include "AiDiscoveryService.h"
#include "P2pNetworks.h"
#include "PeerListManager.h"

namespace platform_system {
class TcpConnection;
}

namespace cn
{
  class LevinProtocol;
  class ISerializer;

  struct P2pMessage {
    enum Type {
      COMMAND,
      REPLY,
      NOTIFY
    };

    P2pMessage(Type type, uint32_t command, const BinaryArray& buffer, int32_t returnCode = 0, bool discoveryMessage = false) :
      type(type), command(command), buffer(buffer), returnCode(returnCode), discovery(discoveryMessage) {
    }

    size_t size() const {
      return buffer.size();
    }

    Type type;
    uint32_t command;
    const BinaryArray buffer;
    int32_t returnCode;
    bool discovery;
  };

  struct P2pConnectionContext : public CryptoNoteConnectionContext {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    platform_system::Context<void>* context = nullptr;
    PeerIdType peerId = 0;
    platform_system::TcpConnection connection;

    P2pConnectionContext(platform_system::Dispatcher& dispatcher, logging::ILogger& log, platform_system::TcpConnection&& conn) :
      connection(std::move(conn)),
      logger(log, "node_server"),
      queueEvent(dispatcher) {
    }

    bool pushMessage(P2pMessage&& msg);
    // Drops the message when the discovery queue budget is exhausted. Does not interrupt the connection.
    bool pushDiscoveryMessage(P2pMessage&& msg);
    std::vector<P2pMessage> popBuffer();
    void interrupt();

    AiConnectionState aiDiscovery;

    uint64_t writeDuration(TimePoint now) const;

  private:
    logging::LoggerRef logger;
    TimePoint writeOperationStartTime;
    platform_system::Event queueEvent;
    std::vector<P2pMessage> writeQueue;
    size_t writeQueueSize = 0;
    size_t m_discoveryQueuedBytes = 0;
    bool stopped = false;
  };

  class NodeServer :  public IP2pEndpoint
  {
  public:

    static void initOptions(boost::program_options::options_description& desc);

    NodeServer(platform_system::Dispatcher& dispatcher, cn::CryptoNoteProtocolHandler& payload_handler, logging::ILogger& log);

    bool run();
    bool init(const NetNodeConfig& config);
    bool deinit();
    bool sendStopSignal();
    uint32_t getThisPeerPort() const { return m_listeningPort; }
    cn::CryptoNoteProtocolHandler& getPayloadObject();

    void serialize(ISerializer& s);

    // debug functions
    bool logPeerlist() const;
    bool logConnections() const;
    uint64_t getConnectionsCount() override;
    size_t getOutgoingConnectionsCount() const;

    cn::PeerlistManager& getPeerlistManager() { return m_peerlist; }

    bool isAiDiscoveryEnabled() const { return m_aiDiscovery.enabled(); }
    AiIngestResult submitAiProviderAdvertisement(const AiProviderAdvertisement& ad);
    AiProviderPage getAiProviders(const AiProviderQuery& query) const;
    uint64_t listedAiProviderCount() const;
    uint64_t aiDiscoveryPeerCount() const;

  private:
    enum PeerType
    {
      anchor = 0,
      white,
      gray
    };
    int handleCommand(const LevinProtocol::Command& cmd, BinaryArray& buff_out, P2pConnectionContext& context, bool& handled);

    //----------------- commands handlers ----------------------------------------------
    int handleHandshake(int command, const COMMAND_HANDSHAKE::request& arg, COMMAND_HANDSHAKE::response& rsp, P2pConnectionContext& context);
    int handleTimedSync(int command, const COMMAND_TIMED_SYNC::request& arg, COMMAND_TIMED_SYNC::response& rsp, P2pConnectionContext& context);
    int handlePing(int command, const COMMAND_PING::request& arg, COMMAND_PING::response& rsp, const P2pConnectionContext& context) const;

    bool enqueueAiDiscovery(P2pConnectionContext& context, const AiOutbound& message);
    void relayAiProviderAdvertisements(const AiProviderAdvertisement& ad, const boost::uuids::uuid* exclude);
    void maintainAiDiscovery();
    void observeAiDiscoveryPeers();

#ifdef ALLOW_DEBUG_COMMANDS
    int handleGetStatInfo(int command, COMMAND_REQUEST_STAT_INFO::request &arg, COMMAND_REQUEST_STAT_INFO::response &rsp, P2pConnectionContext &context);
    int handleGetNetworkState(int command, COMMAND_REQUEST_NETWORK_STATE::request &arg, COMMAND_REQUEST_NETWORK_STATE::response &rsp, P2pConnectionContext &context);
    int handleGetPeerId(int command, COMMAND_REQUEST_PEER_ID::request &arg, COMMAND_REQUEST_PEER_ID::response &rsp, P2pConnectionContext &context);
    bool checkTrust(const proof_of_trust &tr);
#endif

    bool initConfig();
    bool makeDefaultConfig();
    bool storeConfig();
    void initUpnp();

    bool handshake(cn::LevinProtocol& proto, P2pConnectionContext& context, bool just_take_peerlist = false);
    bool timedSync();
    bool handleTimedSyncResponse(const BinaryArray& in, P2pConnectionContext& context);
    void forEachConnection(const std::function<void(P2pConnectionContext &)> &action);

    void onConnectionNew(P2pConnectionContext& context);
    void onConnectionClose(P2pConnectionContext& context);

    //----------------- i_p2p_endpoint -------------------------------------------------------------
    void relayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) override;
    bool invokeNotifyToPeer(int command, const BinaryArray &req_buff, const CryptoNoteConnectionContext &context) override;
    void dropConnection(CryptoNoteConnectionContext &context, bool add_fail) override;
    void forEachConnection(const std::function<void(cn::CryptoNoteConnectionContext &, PeerIdType)> &f) override;
    void externalRelayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) override;
    void externalRelayNotifyToList(int command, const BinaryArray &data_buff, const std::list<boost::uuids::uuid> &relayList) override;
    //-----------------------------------------------------------------------------------------------
    bool handleCommandLine(const boost::program_options::variables_map& vm);
    bool isAddrRecentlyFailed(const uint32_t address_ip);
    bool handleConfig(const NetNodeConfig& config);
    bool appendNetAddress(std::vector<NetworkAddress>& nodes, const std::string& addr);
    bool idleWorker();
    bool handleRemotePeerlist(const std::list<PeerlistEntry>& peerlist, time_t local_time, const CryptoNoteConnectionContext& context);
    bool getLocalNodeData(basic_node_data& node_data) const;
    bool mergePeerlistWithLocal(const std::list<PeerlistEntry>& bs);
    bool fixTimeDelta(std::list<PeerlistEntry>& local_peerlist, time_t local_time, int64_t& delta) const;

    bool connectionsMaker();
    bool makeNewConnectionFromPeerlist(bool use_white_list);
    bool makeNewConnectionFromAnchorPeerlist(const std::vector<AnchorPeerlistEntry> &anchor_peerlist);
    bool tryToConnectAndHandshakeWithNewPeer(const NetworkAddress &na, bool just_take_peerlist = false, uint64_t last_seen_stamp = 0, PeerType peer_type = white, uint64_t first_seen_stamp = 0);
    bool isPeerUsed(const PeerlistEntry &peer) const;
    bool isPeerUsed(const AnchorPeerlistEntry &peer) const;
    bool isAddrConnected(const NetworkAddress& peer) const;
    bool tryPing(const basic_node_data& node_data, const P2pConnectionContext& context);
    bool makeExpectedConnectionsCount(PeerType peer_type, size_t expected_connections);
    bool isPriorityNode(const NetworkAddress& na);

    bool connectToPeerlist(const std::vector<NetworkAddress>& peers);

    bool parsePeersAndAddToContainer(const boost::program_options::variables_map& vm,
      const command_line::arg_descriptor<std::vector<std::string> > & arg, std::vector<NetworkAddress>& container) const;

    //debug functions
    std::string printConnectionsContainer() const;

    using ConnectionContainer = std::unordered_map<boost::uuids::uuid, P2pConnectionContext, boost::hash<boost::uuids::uuid>>;
    using ConnectionIterator = ConnectionContainer::iterator;
    ConnectionContainer m_connections;

    void acceptLoop();
    void connectionHandler(const boost::uuids::uuid& connectionId, P2pConnectionContext& connection);
    void writeHandler(P2pConnectionContext& ctx) const;
    void onIdle();
    void timedSyncLoop();
    void timeoutLoop();

    template<typename T>
    void safeInterrupt(T& obj) const;

    struct config
    {
      network_config m_netConfig;
      uint64_t m_peerId;

      void serialize(ISerializer& s) {
        s(m_netConfig, "m_net_config");
        s(m_peerId, "m_peer_id");
      }
    };

    config m_config;
    std::string m_configFolder;

    bool m_haveAddress;
    bool m_firstConnectionMakerCall;
    uint32_t m_listeningPort;
    uint32_t m_externalPort;
    uint32_t m_ipAddress;
    bool m_allowLocalIp = false;
    bool m_hideMyPort = false;
    std::string m_p2pStateFilename;

    platform_system::Dispatcher& m_dispatcher;
    platform_system::ContextGroup m_workingContextGroup;
    platform_system::Event m_stopEvent;
    platform_system::Timer m_idleTimer;
    platform_system::Timer m_timeoutTimer;
    platform_system::TcpListener m_listener;
    logging::LoggerRef logger;
    std::atomic<bool> m_stop{false};

    CryptoNoteProtocolHandler& m_payloadHandler;
    PeerlistManager m_peerlist;
    AiDiscoveryService m_aiDiscovery;
    bool m_aiDiscoveryPeersNonZero = false;

    OnceInInterval m_connectionsMakerInterval = OnceInInterval(1);
    OnceInInterval m_peerlistStoreInterval = OnceInInterval(60 * 30, false);
    platform_system::Timer m_timedSyncTimer;

    std::string m_bindIp;
    std::string m_port;
#ifdef ALLOW_DEBUG_COMMANDS
    uint64_t m_lastStatRequestTime;
#endif
    std::vector<NetworkAddress> m_priorityPeers;
    std::vector<NetworkAddress> m_exclusivePeers;
    std::vector<NetworkAddress> m_seedNodes;
    std::list<PeerlistEntry> m_commandLinePeers;
    uint64_t m_peerLivetime;
    boost::uuids::uuid m_networkId = CRYPTONOTE_NETWORK;
    std::map<uint32_t, uint64_t> m_hostFailsScore;
    mutable std::mutex mutex;
  };
}
