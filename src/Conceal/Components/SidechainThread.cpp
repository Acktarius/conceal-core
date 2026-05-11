  // SidechainThread.cpp — sidechain validator wrapper for unified Conceal client
  // Copyright (c) 2018-2026 Conceal Network & Conceal Devs
  // Distributed under the MIT/X11 software license

  #include "SidechainThread.h"

  #include "Sidechain/SidechainConfig.h"
  #include "Sidechain/SidechainTypes.h"
  #include "Sidechain/SidechainStorage.h"
  #include "Sidechain/SidechainValidator.h"
  #include "Sidechain/SidechainRpcServer.h"
  #include "Sidechain/BridgeWatcher.h"
  #include "Sidechain/GossipManager.h"
  #include "Sidechain/BftConsensus.h"
  #include "Sidechain/BoltDex.h"

  #include "BoltSync/BoltSync.h"
  #include "BoltSync/CryptoHelpers.h"

  #include "Common/StringTools.h"
  #include "Common/Util.h"
  #include "crypto/crypto.h"
  #include "CryptoNoteCore/Currency.h"
  #include "Logging/ConsoleLogger.h"
  #include "Logging/LoggerManager.h"
  #include "NodeRpcProxy/NodeRpcProxy.h"
  #include <System/Dispatcher.h>

  #include <future>
  #include <memory>

  namespace Conceal
  {

    namespace
    {
      class NodeInitObserver
      {
      public:
        void initCompleted(std::error_code ec)
        {
          std::lock_guard<std::mutex> lock(m_mutex);
          if (m_fulfilled)
            return;
          m_ec = ec;
          m_fulfilled = true;
          m_promise.set_value();
        }
        void waitForInitEnd()
        {
          m_promise.get_future().get();
          if (m_ec)
            throw std::system_error(m_ec);
        }

      private:
        std::error_code m_ec;
        std::promise<void> m_promise;
        std::mutex m_mutex;
        bool m_fulfilled = false;
      };
    }

    void runSidechain(const Config &cfg, SidechainStatus &status, std::atomic<bool> &stopRequested)
    {
      logging::LoggerManager logManager;
      logging::LoggerRef logger(logManager, "sidechain");

      logger(logging::INFO) << "Sidechain starting...";

      // Data directory
      std::string sidechainDir = cfg.dataDir;
      if (!sidechainDir.empty() && sidechainDir.back() != '/')
        sidechainDir += '/';
      sidechainDir += "sidechain";

      if (!tools::create_directories_if_necessary(sidechainDir))
      {
        logger(logging::ERROR) << "Failed to create sidechain directory: " << sidechainDir;
        return;
      }

      // Storage
      Sidechain::SidechainStorage storage(sidechainDir);
      uint64_t topHeight = storage.topBlockHeight();
      logger(logging::INFO) << "Sidechain height: " << topHeight;

      // Gossip
      uint16_t gossipPort = cfg.sidechainBindPort + SidechainConfig::GOSSIP_PORT_OFFSET;
      std::vector<std::string> seedNodes;
      if (!cfg.seedHost.empty())
      {
        uint16_t seedGossipPort = cfg.seedPort > 0
                                      ? cfg.seedPort + SidechainConfig::GOSSIP_PORT_OFFSET
                                      : cfg.sidechainBindPort + SidechainConfig::GOSSIP_PORT_OFFSET;
        seedNodes.push_back(cfg.seedHost + ":" + std::to_string(seedGossipPort));
      }

      Sidechain::GossipManager gossip(gossipPort, seedNodes);
      gossip.start();
      logger(logging::INFO) << "Gossip on port " << gossipPort;

      // Validator identity
      crypto::PublicKey validatorPub;
      crypto::SecretKey validatorSec;
      crypto::generate_keys(validatorPub, validatorSec);

      Sidechain::ValidatorInfo self;
      self.publicKey = validatorPub;
      self.secretKey = validatorSec;
      self.host = cfg.rpcBindIp;
      self.port = cfg.sidechainBindPort;
      self.stake = 1000000;
      self.active = true;

      auto existingValidators = storage.getActiveValidators();
      std::vector<Sidechain::ValidatorInfo> validators;

      if (existingValidators.empty() && seedNodes.empty())
      {
        self.id = 0;
        storage.addValidator(self);
        validators.push_back(self);
        logger(logging::INFO) << "Genesis validator ID 0";
      }
      else if (!existingValidators.empty() && seedNodes.empty())
      {
        validators = existingValidators;
        self.id = static_cast<uint32_t>(validators.size());
        storage.addValidator(self);
        validators.push_back(self);
        logger(logging::INFO) << "Validator restarted ID " << self.id;
      }
      else
      {
        self.id = 0;
        storage.addValidator(self);

        Sidechain::BftConsensus tempConsensus(storage, self, gossip);
        uint16_t seedGossipPort = cfg.seedPort > 0
                                      ? cfg.seedPort + SidechainConfig::GOSSIP_PORT_OFFSET
                                      : SidechainConfig::DEFAULT_RPC_BIND_PORT + SidechainConfig::GOSSIP_PORT_OFFSET;
        tempConsensus.syncValidators(cfg.seedHost, seedGossipPort);

        validators = storage.getActiveValidators();
        bool found = false;
        for (const auto &v : validators)
        {
          if (v.publicKey == self.publicKey)
          {
            self.id = v.id;
            found = true;
            break;
          }
        }
        if (!found)
        {
          self.id = static_cast<uint32_t>(validators.size());
          storage.addValidator(self);
          validators.push_back(self);
        }
        logger(logging::INFO) << "Validator joined network ID " << self.id;
      }

      // Reward key
      crypto::PublicKey rewardPub;
      bool hasRewardWallet = false;
      if (!cfg.rewardAddress.empty())
      {
        cn::Currency currency = cn::CurrencyBuilder(logManager).currency();
        std::string addr = cfg.rewardAddress;
        if (addr.size() == 64 && addr.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        {
          common::podFromHex(addr, rewardPub);
          hasRewardWallet = true;
        }
        else
        {
          cn::AccountPublicAddress acc;
          if (currency.parseAccountAddressString(addr, acc))
          {
            rewardPub = acc.spendPublicKey;
            hasRewardWallet = true;
          }
        }
      }

      // Validator
      Sidechain::SidechainValidator validator(storage, self, validators, gossip, cfg.testnet);
      validator.start();

      if (hasRewardWallet)
        validator.setRewardKey(rewardPub);

      // DEX Engine
      std::unique_ptr<Sidechain::BoltDex::Engine> dexEngine;
      if (cfg.dexFee >= 0.0)
      {
        dexEngine.reset(new Sidechain::BoltDex::Engine());
        dexEngine->setStorage(storage);
        dexEngine->setValidator(validator);
        dexEngine->setTradingFee(cfg.dexFee);

        if (hasRewardWallet)
        {
          crypto::SecretKey emptySec{};
          dexEngine->setDexKeys(rewardPub, emptySec);
        }
        else
        {
          dexEngine->setDexKeys(self.publicKey, validatorSec);
        }

        if (dexEngine)
          validator.setDexEngine(dexEngine.get());
      }

      // RPC server
      Sidechain::SidechainRpcServer rpcServer(logManager, storage, validator);
      rpcServer.setTestnet(cfg.testnet);
      rpcServer.start(cfg.rpcBindIp, cfg.sidechainBindPort, cfg.rpcThreads);

      if (dexEngine)
        rpcServer.setDexEngine(dexEngine.get());

      // Status update loop
      while (!stopRequested)
      {
        status.height = storage.topBlockHeight();
        status.validatorCount = gossip.getPeerCount();
        status.pendingTxCount = validator.getPendingTransactions().size();
        status.tokenCount = storage.getAllTokens().size();
        status.dexFee = cfg.dexFee;
        status.bridgeWatching = cfg.watchBridge;
        status.pendingUnlocks = 0;

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

      // Shutdown
      rpcServer.stop();
      gossip.stop();
      validator.stop();
      storage.flush();
      logger(logging::INFO) << "Sidechain stopped.";
    }

  } // namespace Conceal