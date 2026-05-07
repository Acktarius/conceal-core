// conceal-side — Conceal Sidechain Validator Node with BFT consensus
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <future>

#include <boost/program_options.hpp>

#include "SidechainConfig.h"
#include "SidechainTypes.h"
#include "SidechainStorage.h"
#include "SidechainValidator.h"
#include "SidechainRpcServer.h"
#include "BridgeWatcher.h"
#include "GossipManager.h"
#include "BftConsensus.h"

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltCore/BoltCore.h"

#include "Common/SignalHandler.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include <System/Dispatcher.h>

namespace po = boost::program_options;

struct Config
{
  bool testnet = false;
  std::string dataDir = "./sidechain-data";
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string bindIp = "127.0.0.1";
  uint16_t bindPort = 8080;
  std::string bridgeViewKeyHex;
  std::string bridgeSpendKeyHex;
  bool watchBridge = false;
  std::string seedHost;
  uint16_t seedPort = 0;
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Conceal Sidechain Validator");
  desc.add_options()("help,h", "Show help")("data-dir", po::value<std::string>()->default_value("./sidechain-data"), "Sidechain data directory")("daemon-host", po::value<std::string>()->default_value("127.0.0.1"), "Main chain daemon host")("daemon-port", po::value<uint16_t>()->default_value(16000), "Main chain daemon port")("bind-ip", po::value<std::string>()->default_value("127.0.0.1"), "Sidechain RPC bind IP")("bind-port", po::value<uint16_t>()->default_value(8080), "Sidechain RPC bind port")("bridge-view-key", po::value<std::string>()->default_value(""), "Bridge view key")("bridge-spend-key", po::value<std::string>()->default_value(""), "Bridge spend key")("watch-bridge", po::bool_switch(), "Watch main chain for CCX deposits")("testnet", po::bool_switch(), "Run sidechain in testnet mode")("seed-host", po::value<std::string>()->default_value(""), "Seed validator host to connect to")("seed-port", po::value<uint16_t>()->default_value(0), "Seed validator RPC port (gossip port is RPC + 1000)");

  po::variables_map vm;
  try
  {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help"))
    {
      std::cout << desc << std::endl;
      return false;
    }
    po::notify(vm);
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    return false;
  }

  cfg.dataDir = vm["data-dir"].as<std::string>();
  cfg.daemonHost = vm["daemon-host"].as<std::string>();
  cfg.daemonPort = vm["daemon-port"].as<uint16_t>();
  cfg.bindIp = vm["bind-ip"].as<std::string>();
  cfg.bindPort = vm["bind-port"].as<uint16_t>();
  cfg.bridgeViewKeyHex = vm["bridge-view-key"].as<std::string>();
  cfg.bridgeSpendKeyHex = vm["bridge-spend-key"].as<std::string>();
  cfg.watchBridge = vm["watch-bridge"].as<bool>();
  cfg.testnet = vm["testnet"].as<bool>();
  cfg.seedHost = vm["seed-host"].as<std::string>();
  cfg.seedPort = vm["seed-port"].as<uint16_t>();

  return true;
}

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

int main(int argc, char *argv[])
{
  Config cfg;
  if (!parseArgs(argc, argv, cfg))
    return 1;

  // Force unbuffered output so all debug logs appear immediately
  std::cout.setf(std::ios::unitbuf);

  logging::LoggerManager logManager;
  logging::ConsoleLogger consoleLogger;
  logging::LoggerRef logger(logManager, "Sidechain");

  logger(logging::INFO) << "Conceal Sidechain Validator starting";
  logger(logging::INFO) << "Data directory: " << cfg.dataDir;

  // Storage
  Sidechain::SidechainStorage storage(cfg.dataDir);
  uint64_t topHeight = storage.topBlockHeight();
  logger(logging::INFO) << "Sidechain height: " << topHeight;

  // Setup gossip
  uint16_t gossipPort = cfg.bindPort + SidechainConfig::GOSSIP_PORT_OFFSET;
  std::vector<std::string> seedNodes;

  if (!cfg.seedHost.empty())
  {
    uint16_t seedGossipPort = cfg.seedPort > 0
                                  ? cfg.seedPort + SidechainConfig::GOSSIP_PORT_OFFSET
                                  : cfg.bindPort + SidechainConfig::GOSSIP_PORT_OFFSET;
    seedNodes.push_back(cfg.seedHost + ":" + std::to_string(seedGossipPort));
  }

  Sidechain::GossipManager gossip(gossipPort, seedNodes);
  gossip.start();
  logger(logging::INFO) << "Gossip listening on port " << gossipPort;

  // Validator identity
  crypto::PublicKey validatorPub;
  crypto::SecretKey validatorSec;
  crypto::generate_keys(validatorPub, validatorSec);

  Sidechain::ValidatorInfo self;
  self.publicKey = validatorPub;
  self.secretKey = validatorSec;
  self.host = cfg.bindIp;
  self.port = cfg.bindPort;
  self.stake = 1000000;
  self.active = true;

  auto existingValidators = storage.getActiveValidators();
  std::vector<Sidechain::ValidatorInfo> validators;

  if (existingValidators.empty() && seedNodes.empty())
  {
    // Genesis validator
    self.id = 0;
    storage.addValidator(self);
    validators.push_back(self);
    logger(logging::INFO) << "Genesis validator started with ID 0";
  }
  else if (!existingValidators.empty() && seedNodes.empty())
  {
    // Restarting, validators in storage
    validators = existingValidators;
    self.id = validators.size();
    storage.addValidator(self);
    validators.push_back(self);
    logger(logging::INFO) << "Validator restarted with ID " << self.id;
  }
  else
  {
    // Joining via seed
    self.id = 0;
    storage.addValidator(self);

    // Sync validator set from seed
    Sidechain::BftConsensus tempConsensus(storage, self, gossip);
    uint16_t seedGossipPort = cfg.seedPort > 0
                                  ? cfg.seedPort + SidechainConfig::GOSSIP_PORT_OFFSET
                                  : SidechainConfig::DEFAULT_RPC_BIND_PORT + SidechainConfig::GOSSIP_PORT_OFFSET;
    tempConsensus.syncValidators(cfg.seedHost, seedGossipPort);

    // Re-read validators after sync
    validators = storage.getActiveValidators();

    // Find our ID in the synced list
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
      self.id = validators.size();
      storage.addValidator(self);
      validators.push_back(self);
    }

    logger(logging::INFO) << "Validator joined network with ID " << self.id;
  }

  logger(logging::INFO) << "Validator ID: " << self.id
                        << " Public key: " << common::podToHex(self.publicKey);

  // Validator with BFT
  Sidechain::SidechainValidator validator(storage, self, validators, gossip, cfg.testnet);
  validator.start();
  logger(logging::INFO) << "Validator started with BFT threshold "
                        << SidechainConfig::BFT_BLOCK_THRESHOLD;

  // RPC server (BoltHttp — runs in its own threads, no dispatcher needed)
  Sidechain::SidechainRpcServer rpcServer(consoleLogger, storage, validator);
  rpcServer.setTestnet(cfg.testnet);
  rpcServer.start(cfg.bindIp, cfg.bindPort);
  logger(logging::INFO) << "RPC server listening on " << cfg.bindIp << ":" << cfg.bindPort;

  // Daemon connection (only when bridge watching is enabled)
  cn::Currency currency = cn::CurrencyBuilder(logManager).currency();

  bool daemonConnected = false;
  std::unique_ptr<platform_system::Dispatcher> daemonDispatcher;
  std::unique_ptr<cn::NodeRpcProxy> nodePtr;
  std::unique_ptr<NodeInitObserver> initObsPtr;

  if (cfg.watchBridge)
  {
    try
    {
      daemonDispatcher.reset(new platform_system::Dispatcher());
      nodePtr.reset(new cn::NodeRpcProxy(*daemonDispatcher, cfg.daemonHost, cfg.daemonPort, consoleLogger));
      initObsPtr.reset(new NodeInitObserver());
      nodePtr->init([&obs = *initObsPtr](std::error_code ec)
                    { obs.initCompleted(ec); });
      initObsPtr->waitForInitEnd();
      daemonConnected = true;
      logger(logging::INFO) << "Connected to daemon " << cfg.daemonHost << ":" << cfg.daemonPort;
    }
    catch (const std::exception &e)
    {
      logger(logging::WARNING) << "Daemon not reachable — bridge watching disabled: " << e.what();
      daemonConnected = false;
    }
    catch (...)
    {
      logger(logging::WARNING) << "Daemon not reachable — bridge watching disabled";
      daemonConnected = false;
    }
  }
  else
  {
    logger(logging::INFO) << "Daemon connection skipped (bridge watching not enabled)";
  }

  // Bridge watcher
  std::unique_ptr<Sidechain::BridgeWatcher> bridgeWatcher;

  if (cfg.watchBridge && !cfg.bridgeViewKeyHex.empty() && daemonConnected && nodePtr)
  {
    crypto::SecretKey bridgeViewKey;
    if (!BoltSync::hexToSecretKey(cfg.bridgeViewKeyHex, bridgeViewKey))
    {
      logger(logging::ERROR) << "Invalid bridge view key";
      return 1;
    }

    crypto::PublicKey bridgeViewPub;
    crypto::secret_key_to_public_key(bridgeViewKey, bridgeViewPub);

    crypto::PublicKey bridgeSpendPub;
    if (!cfg.bridgeSpendKeyHex.empty())
    {
      crypto::SecretKey bridgeSpendKey;
      if (BoltSync::hexToSecretKey(cfg.bridgeSpendKeyHex, bridgeSpendKey))
        crypto::secret_key_to_public_key(bridgeSpendKey, bridgeSpendPub);
    }

    bridgeWatcher.reset(new Sidechain::BridgeWatcher(
        storage, *nodePtr, bridgeViewPub, bridgeViewKey, bridgeSpendPub));

    bridgeWatcher->start([&](const Sidechain::Transaction &depositTx)
                         {
            logger(logging::INFO) << "Deposit detected: " << depositTx.amount << " CCX locked";
            validator.submitTransaction(depositTx); });

    logger(logging::INFO) << "Bridge watcher started";
  }

  // Status display
  logger(logging::INFO) << "═══════════════════════════════════════";
  logger(logging::INFO) << "Sidechain Validator Ready";
  logger(logging::INFO) << "═══════════════════════════════════════";
  logger(logging::INFO) << "Chain height: " << storage.topBlockHeight();
  logger(logging::INFO) << "RPC: " << cfg.bindIp << ":" << cfg.bindPort;
  logger(logging::INFO) << "Gossip: port " << gossipPort;
  logger(logging::INFO) << "BFT threshold: " << SidechainConfig::BFT_BLOCK_THRESHOLD;
  logger(logging::INFO) << "Network: " << (cfg.testnet ? "Testnet" : "Mainnet-Staging");

  if (!seedNodes.empty())
    logger(logging::INFO) << "Connected to seed: " << seedNodes[0];

  auto tokens = storage.getAllTokens();
  logger(logging::INFO) << "Registered tokens: " << tokens.size();
  logger(logging::INFO) << "═══════════════════════════════════════";

  // Main loop
  std::atomic<bool> stopRequested{false};
  tools::SignalHandler::install([&stopRequested]
                                { stopRequested = true; });

  logger(logging::INFO) << "Sidechain validator running — press Ctrl+C to stop";

  while (!stopRequested)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Shutdown
  logger(logging::INFO) << "Shutting down...";
  rpcServer.stop();
  validator.stop();
  gossip.stop();
  if (bridgeWatcher)
    bridgeWatcher->stop();
  if (nodePtr)
    nodePtr->shutdown();
  storage.flush();
  logger(logging::INFO) << "Sidechain validator stopped";

  return 0;
}