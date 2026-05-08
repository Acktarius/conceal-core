// conceal-dex — BoltDex order book and matching engine with settlement
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <cstring>

#include <boost/program_options.hpp>

#include "BoltDex.h"
#include "BoltDexRpcServer.h"
#include "Rpc/HttpClient.h"

#include "Common/Util.h"
#include "Common/StringTools.h"
#include "Common/SignalHandler.h"
#include "BoltSync/CryptoHelpers.h"
#include "crypto/crypto.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include <System/Dispatcher.h>

namespace po = boost::program_options;

struct Config
{
  std::string bindIp = "127.0.0.1";
  uint16_t bindPort = 8090;
  std::string sidechainHost = "127.0.0.1";
  uint16_t sidechainPort = 8080;
  size_t rpcThreads = 1;
  std::string dexKeyHex;
  double dexFee = 0.0; // trading fee percentage, e.g. 0.5 = 0.5%
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Conceal BoltDex");
  desc.add_options()("help,h", "Show help")("bind-ip", po::value<std::string>()->default_value("127.0.0.1"), "DEX RPC bind IP")("bind-port", po::value<uint16_t>()->default_value(8090), "DEX RPC bind port")("sidechain-host", po::value<std::string>()->default_value("127.0.0.1"), "Sidechain RPC host")("sidechain-port", po::value<uint16_t>()->default_value(8080), "Sidechain RPC port")("rpc-threads", po::value<size_t>()->default_value(1), "RPC server thread count")("dex-key", po::value<std::string>()->default_value(""), "Hex public key for DEX deposits (optional, generates one if not set)")("dex-fee", po::value<double>()->default_value(0.0), "Trading fee percentage (e.g. 0.5 = 0.5%, 0.25 = 0.25%)");

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

  cfg.bindIp = vm["bind-ip"].as<std::string>();
  cfg.bindPort = vm["bind-port"].as<uint16_t>();
  cfg.sidechainHost = vm["sidechain-host"].as<std::string>();
  cfg.sidechainPort = vm["sidechain-port"].as<uint16_t>();
  cfg.rpcThreads = vm["rpc-threads"].as<size_t>();
  cfg.dexKeyHex = vm["dex-key"].as<std::string>();
  cfg.dexFee = vm["dex-fee"].as<double>();

  if (cfg.dexFee < 0.0 || cfg.dexFee > 100.0)
  {
    std::cerr << "Error: dex-fee must be between 0.0 and 100.0" << std::endl;
    return false;
  }

  return true;
}

int main(int argc, char *argv[])
{
  Config cfg;
  if (!parseArgs(argc, argv, cfg))
    return 1;

  logging::LoggerManager logManager;
  logging::ConsoleLogger consoleLogger;
  logging::LoggerRef logger(logManager, "BoltDex");

  logger(logging::INFO) << "BoltDex starting";
  logger(logging::INFO) << "Trading fee: " << cfg.dexFee << "%";

  BoltDex::Engine engine;

  // Set the DEX trading fee
  engine.setTradingFee(cfg.dexFee);

  // Wire up RPC caller to communicate with sidechain
  engine.setRpcCaller([&cfg](const std::string &method, const std::string &params) -> BoltDex::RpcResult
                      {
        BoltDex::RpcResult result;
        platform_system::Dispatcher dispatcher;
        cn::HttpClient client(dispatcher, cfg.sidechainHost, cfg.sidechainPort);

        cn::HttpRequest req;
        cn::HttpResponse res;

        std::string body = R"({"jsonrpc":"2.0","id":1,"method":")" + method +
                           R"(","params":)" + params + "}";

        req.setUrl("/json_rpc");
        req.setBody(body);
        req.addHeader("Content-Type", "application/json");

        try
        {
            client.request(req, res);
            result.success = true;
            result.response = res.getBody();
        }
        catch (...)
        {
            result.success = false;
            result.response = "{}";
        }

        return result; });

  engine.onTrade([](const BoltDex::Trade &trade)
                 { std::cout << "Trade executed: #" << trade.id
                             << " amount=" << trade.amount
                             << " price=" << trade.price << std::endl; });

  // DEX keypair
  crypto::PublicKey dexPubKey;
  crypto::SecretKey dexSecKey;

  if (!cfg.dexKeyHex.empty() && cfg.dexKeyHex.size() == 64)
  {
    common::podFromHex(cfg.dexKeyHex, dexPubKey);
    std::cout << "Using provided DEX address: " << cfg.dexKeyHex << std::endl;

    const char *envSec = std::getenv("DEX_SECRET_KEY");
    if (envSec && strlen(envSec) == 64)
      BoltSync::hexToSecretKey(std::string(envSec), dexSecKey);
    else
      std::cout << "WARNING: No DEX_SECRET_KEY set. Withdrawals will fail." << std::endl;
  }
  else
  {
    crypto::generate_keys(dexPubKey, dexSecKey);
    std::cout << "Generated DEX deposit address: " << common::podToHex(dexPubKey) << std::endl;
    std::cout << "Save this address to receive deposits." << std::endl;
  }

  engine.setDexKeys(dexPubKey, dexSecKey);

  BoltDex::RpcServer rpcServer(consoleLogger, engine);
  rpcServer.start(cfg.bindIp, cfg.bindPort, cfg.rpcThreads);

  logger(logging::INFO) << "BoltDex ready on " << cfg.bindIp << ":" << cfg.bindPort;

  std::atomic<bool> stopRequested{false};
  tools::SignalHandler::install([&stopRequested]
                                { stopRequested = true; });

  logger(logging::INFO) << "Press Ctrl+C to stop";

  while (!stopRequested)
  {
    engine.watchDeposits();
    engine.processSettlements();
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
  }

  logger(logging::INFO) << "Shutting down...";
  rpcServer.stop();
  logger(logging::INFO) << "BoltDex stopped";

  return 0;
}