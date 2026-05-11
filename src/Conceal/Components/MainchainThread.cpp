// MainchainThread.cpp — daemon wrapper for unified Conceal client
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "MainchainThread.h"

#include "version.h"
#include "Common/PathTools.h"
#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/Checkpoints.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/CoreConfig.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/MinerConfig.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "P2p/NetNode.h"
#include "P2p/NetNodeConfig.h"
#include "Rpc/RpcServer.h"
#include "Rpc/RpcServerConfig.h"

#ifdef HAVE_MDBX
#include "Storage/MDBXBlockchainStorage.h"
#endif

#include <Logging/LoggerManager.h>
#include <boost/filesystem.hpp>
#include <thread>

using namespace cn;

namespace Conceal
{

  void runMainchain(const Config &cfg, MainchainStatus &status, std::atomic<bool> &stopRequested)
  {
    logging::LoggerManager logManager;
    logging::LoggerRef logger(logManager, "mainchain");

    // Build config folder path
    std::string configFolder = cfg.dataDir;
    if (!configFolder.empty() && configFolder.back() != '/')
      configFolder += '/';
    configFolder += "main";

    if (!tools::create_directories_if_necessary(configFolder))
    {
      logger(logging::ERROR) << "Failed to create mainchain data directory: " << configFolder;
      return;
    }

    // Currency
    cn::CurrencyBuilder currencyBuilder(logManager);
    currencyBuilder.testnet(cfg.testnet);
    cn::Currency currency = currencyBuilder.currency();

    // Core
    CoreConfig coreConfig;
    coreConfig.configFolder = configFolder;
    coreConfig.testnet = cfg.testnet;
    coreConfig.useMdbx = cfg.useMdbx;

    cn::core ccore(currency, nullptr, logManager, false, false, coreConfig.useMdbx);

    // Checkpoints
    cn::Checkpoints checkpoints(logManager);
    checkpoints.set_testnet(cfg.testnet);
    checkpoints.load_checkpoints();
    checkpoints.load_checkpoints_from_dns();
    ccore.set_checkpoints(std::move(checkpoints));

    // Network config
    NetNodeConfig netNodeConfig;
    netNodeConfig.setTestnet(cfg.testnet);
    netNodeConfig.setConfigFolder(configFolder);
    netNodeConfig.setBindIp(cfg.p2pBindIp);
    netNodeConfig.setBindPort(cfg.p2pBindPort);

    MinerConfig minerConfig;

    RpcServerConfig rpcConfig;
    rpcConfig.bindIp = cfg.rpcBindIp;
    rpcConfig.bindPort = cfg.rpcBindPort;

    // MDBX auto-recovery
#ifdef HAVE_MDBX
    if (coreConfig.useMdbx)
    {
      std::string dbPath = configFolder;
      if (!dbPath.empty() && dbPath.back() != '/')
        dbPath += '/';
      dbPath += "mdbx_blocks";

      if (boost::filesystem::exists(dbPath))
      {
        CryptoNote::MDBXBlockchainStorage storage(dbPath, 0);
        uint32_t topHeight = storage.topBlockHeight();

        if (topHeight > 0)
        {
          bool needsRecovery = false;
          cn::BinaryArray ba;
          if (!storage.getBlockEntry(topHeight, ba))
            needsRecovery = true;
          else if (ba.empty())
            needsRecovery = true;

          if (needsRecovery)
          {
            uint32_t recoverTo = topHeight > 100 ? topHeight - 100 : 0;
            logger(logging::INFO) << "Auto-recovering mainchain: removing blocks above " << recoverTo;
            for (uint32_t h = recoverTo + 1; h <= topHeight; ++h)
            {
              crypto::Hash blockHash = storage.getBlockHash(h);
              if (blockHash != NULL_HASH)
                storage.removeBlock(blockHash);
              storage.popBlockEntry(h);
              storage.removeBlockHeader(h);
            }
            storage.setTopBlockHeight(recoverTo);
            storage.putMeta("idx_hashes", std::vector<uint8_t>());
            storage.putMeta("idx_topheight", std::vector<uint8_t>());
            storage.flush();
            logger(logging::INFO) << "Mainchain recovery complete. Height: " << recoverTo;
          }
        }
        storage.close();
      }
    }
#endif

    // Protocol handler
    platform_system::Dispatcher dispatcher;
    cn::CryptoNoteProtocolHandler cprotocol(currency, dispatcher, ccore, nullptr, logManager);
    cn::NodeServer p2psrv(dispatcher, cprotocol, logManager);
    cn::RpcServer rpcServer(dispatcher, logManager, ccore, p2psrv, cprotocol);

    cprotocol.set_p2p_endpoint(&p2psrv);
    ccore.set_cryptonote_protocol(&cprotocol);

    // Checkpoint distribution callback
    ccore.setCheckpointGeneratedCallback(
        [&cprotocol](uint32_t height, const crypto::Hash &hash)
        { cprotocol.broadcastCheckpoint(height, hash); });

    // Initialize P2P
    logger(logging::INFO) << "Initializing P2P...";
    if (!p2psrv.init(netNodeConfig))
    {
      logger(logging::ERROR) << "Failed to init P2P";
      return;
    }

    // Initialize core
    logger(logging::INFO) << "Initializing core...";
    if (!ccore.init(coreConfig, minerConfig, true))
    {
      logger(logging::ERROR) << "Failed to init core";
      return;
    }

    // Start RPC
    rpcServer.start(rpcConfig.bindIp, rpcConfig.bindPort);
    rpcServer.enableCors("*");

    // Run P2P in background thread
    std::thread p2pThread([&]()
                          { p2psrv.run(); });

    // Status update loop
   while (!stopRequested)
    {
      uint32_t localHeight = 0;
      crypto::Hash topId;
      ccore.get_blockchain_top(localHeight, topId);

      status.localHeight = localHeight;          // MainchainStatus has this
      status.networkHeight = cprotocol.getObservedHeight();  // MainchainStatus has this
      status.peerCount = cprotocol.getPeerCount();           // MainchainStatus has this
      status.synced = cprotocol.isSynchronized();
      status.setHash(common::podToHex(topId));    // MainchainStatus has this

      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Shutdown
    p2psrv.sendStopSignal();
    if (p2pThread.joinable())
      p2pThread.join();

    // Force close MDBX to release lock
    ccore.saveBlockchain();

    rpcServer.stop();
    ccore.deinit();
    p2psrv.deinit();
    ccore.set_cryptonote_protocol(nullptr);
    cprotocol.set_p2p_endpoint(nullptr);
  }

} // namespace Conceal