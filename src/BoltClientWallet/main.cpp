// conceal-bolt – Terminal UI wallet powered by BoltSync + BoltCore + Sidechain
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltCore/BoltCore.h"

#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include <System/Dispatcher.h>
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"
#include "Rpc/HttpClient.h"

using namespace cn;
namespace po = boost::program_options;

// Configuration
struct Config
{
  std::string dataDir;
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string viewKeyHex;
  std::string spendKeyHex;
  unsigned int scanThreads = 0;
  bool skipScan = false;
  std::string sidechainHost = "127.0.0.1";
  uint16_t sidechainPort = 8080;
  bool sidechainTestnet = false;
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Conceal Bolt - Terminal Wallet");
  desc.add_options()("help,h", "Show help")("data-dir", po::value<std::string>(), "Path to blockchain data directory (for scanning)")("daemon-host", po::value<std::string>()->default_value("127.0.0.1"), "Daemon RPC host")("daemon-port", po::value<uint16_t>()->default_value(16000), "Daemon RPC port")("view-key", po::value<std::string>(), "64-char hex private view key")("spend-key", po::value<std::string>(), "64-char hex private spend key (optional for view-only)")("threads", po::value<unsigned int>()->default_value(2), "Scan threads (0=auto)")("skip-scan", po::bool_switch(), "Skip blockchain scan (use if already scanned)")("sidechain-host", po::value<std::string>()->default_value("127.0.0.1"), "Sidechain RPC host")("sidechain-port", po::value<uint16_t>()->default_value(8080), "Sidechain RPC port")("sidechain-testnet", po::bool_switch(), "Connect to sidechain in testnet mode");

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

    if (vm.count("data-dir"))
      cfg.dataDir = vm["data-dir"].as<std::string>();
    cfg.daemonHost = vm["daemon-host"].as<std::string>();
    cfg.daemonPort = vm["daemon-port"].as<uint16_t>();
    if (vm.count("view-key"))
      cfg.viewKeyHex = vm["view-key"].as<std::string>();
    if (vm.count("spend-key"))
      cfg.spendKeyHex = vm["spend-key"].as<std::string>();
    cfg.scanThreads = vm["threads"].as<unsigned int>();
    cfg.skipScan = vm["skip-scan"].as<bool>();
    cfg.sidechainHost = vm["sidechain-host"].as<std::string>();
    cfg.sidechainPort = vm["sidechain-port"].as<uint16_t>();
    cfg.sidechainTestnet = vm["sidechain-testnet"].as<bool>();
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    return false;
  }

  if (cfg.viewKeyHex.size() != 64)
  {
    std::cerr << "Error: view-key must be 64 hex characters" << std::endl;
    return false;
  }
  if (!cfg.spendKeyHex.empty() && cfg.spendKeyHex.size() != 64)
  {
    std::cerr << "Error: spend-key must be 64 hex characters" << std::endl;
    return false;
  }
  return true;
}

// Sidechain RPC helper
std::string sidechainCall(const std::string &host, uint16_t port,
                          const std::string &method, const std::string &params)
{
  platform_system::Dispatcher dispatcher;
  HttpClient client(dispatcher, host, port);

  HttpRequest req;
  HttpResponse res;

  std::string body = R"({"jsonrpc":"2.0","id":1,"method":")" + method +
                     R"(","params":)" + params + "}";

  req.setUrl("/json_rpc");
  req.setBody(body);
  req.addHeader("Content-Type", "application/json");

  try
  {
    client.request(req, res);
    return res.getBody();
  }
  catch (...)
  {
    return "{}";
  }
}

// Sidechain summary helper
struct SidechainSummary
{
  uint64_t height = 0;
  uint64_t tokenCount = 0;
  uint64_t pendingTx = 0;
  uint64_t totalBackedCCX = 0;
  bool connected = false;
};

SidechainSummary getSidechainSummary(const Config &cfg)
{
  SidechainSummary summary;
  std::string status = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getStatus", "{}");
  if (status.empty() || status == "{}")
    return summary;
  summary.connected = true;

  auto extractNumber = [](const std::string &json, const std::string &key) -> uint64_t
  {
    size_t pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos)
      return 0;
    pos += key.length() + 3;
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t'))
      pos++;
    std::string num;
    while (pos < json.length() && json[pos] >= '0' && json[pos] <= '9')
    {
      num += json[pos];
      pos++;
    }
    if (num.empty())
      return 0;
    try
    {
      return std::stoull(num);
    }
    catch (...)
    {
      return 0;
    }
  };

  summary.height = extractNumber(status, "height");
  summary.tokenCount = extractNumber(status, "tokenCount");
  summary.pendingTx = extractNumber(status, "pendingTransactions");
  summary.totalBackedCCX = 0;
  return summary;
}

// Menu helpers
void clearScreen() { std::cout << "\033[2J\033[1;1H" << std::flush; }

std::string formatAmount(uint64_t amount, uint8_t decimals = 6)
{
  if (decimals == 0)
    return std::to_string(amount);
  std::string s = std::to_string(amount);
  while (s.length() <= decimals)
    s = "0" + s;
  size_t dotPos = s.length() - decimals;
  std::string result = s.substr(0, dotPos) + "." + s.substr(dotPos);
  while (result.back() == '0' && result[result.size() - 2] != '.')
    result.pop_back();
  return result;
}

// Address conversion helper
std::string addressToHexPubKey(const std::string &input, cn::Currency &currency)
{
  if (input.size() == 64 && input.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
    return input;

  try
  {
    cn::AccountPublicAddress addr;
    if (currency.parseAccountAddressString(input, addr))
    {
      return common::podToHex(addr.spendPublicKey);
    }
  }
  catch (...)
  {
  }

  return input;
}

// Sidechain menus
void sidechainTokensMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== Sidechain Tokens ===" << std::endl;
  std::cout << std::endl;
  auto summary = getSidechainSummary(cfg);
  std::cout << "Network Summary:" << std::endl;
  std::cout << "  Height: " << summary.height << std::endl;
  std::cout << "  Total unique tokens: " << summary.tokenCount << std::endl;
  std::cout << "  Total CCX backing tokens: " << summary.totalBackedCCX << std::endl;
  std::cout << "  Pending transactions: " << summary.pendingTx << std::endl;
  std::cout << std::endl;
  std::cout << "Token List:" << std::endl;
  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokens", "{}");
  std::cout << result << std::endl;
  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainCreateTokenMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Create Token ===" << std::endl;
  std::cout << std::endl;
  std::cout << "Token models:" << std::endl;
  std::cout << "  0 = Unbacked (meme coins, no CCX required, free to create)" << std::endl;
  std::cout << "  1 = Fully Backed (stablecoins, CCX must be locked 1:1)" << std::endl;
  std::cout << "  2 = Hybrid (serious projects, CCX-backed with flexible fees)" << std::endl;
  std::cout << std::endl;

  std::string name, symbol;
  uint64_t initialSupply;
  int backingModel;
  int decimals = 6;

  std::cout << "Step 1/6: Token name (e.g., \"MyToken\"): ";
  std::getline(std::cin, name);
  if (name.empty())
  {
    std::cout << "Token name cannot be empty. Cancelled." << std::endl;
    std::cout << "Press enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Step 2/6: Token symbol (e.g., \"MYT\"): ";
  std::getline(std::cin, symbol);
  if (symbol.empty())
  {
    std::cout << "Token symbol cannot be empty. Cancelled." << std::endl;
    std::cout << "Press enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Step 3/6: Initial supply (e.g., 1000000): ";
  std::cin >> initialSupply;
  if (initialSupply == 0)
  {
    std::cout << "Supply must be greater than 0. Cancelled." << std::endl;
    std::cin.ignore();
    std::cout << "Press enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Step 4/6: Decimals (default 6, e.g., 6 for 0.000001): ";
  std::cin >> decimals;
  if (decimals < 0 || decimals > 18)
  {
    std::cout << "Decimals must be between 0 and 18. Using default 6." << std::endl;
    decimals = 6;
  }

  std::cout << "Step 5/6: Backing model (0=Unbacked, 1=Fully Backed, 2=Hybrid): ";
  std::cin >> backingModel;
  std::cin.ignore();

  if (backingModel < 0 || backingModel > 2)
  {
    std::cout << "Invalid backing model. Cancelled." << std::endl;
    std::cout << "Press enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << std::endl;
  std::cout << "Step 6/6: Confirm token creation:" << std::endl;
  std::cout << "  Name: " << name << std::endl;
  std::cout << "  Symbol: " << symbol << std::endl;
  std::cout << "  Initial supply: " << initialSupply << std::endl;
  std::cout << "  Decimals: " << decimals << std::endl;
  std::cout << "  Backing model: ";
  if (backingModel == 0)
    std::cout << "Unbacked (no CCX needed)";
  else if (backingModel == 1)
    std::cout << "Fully Backed (CCX locked 1:1)";
  else
    std::cout << "Hybrid (CCX-backed, flexible fees)";
  std::cout << std::endl;
  std::cout << std::endl;
  std::cout << "Create this token? (y/n): ";

  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cout << "Press enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  std::string nameHex = common::toHex(common::asBinaryArray(name));
  std::string symbolHex = common::toHex(common::asBinaryArray(symbol));

  std::ostringstream params;
  params << "{"
         << R"("from":")" << spendPubHex << R"(")"
         << R"(,"nameHex":")" << nameHex << R"(")"
         << R"(,"symbolHex":")" << symbolHex << R"(")"
         << R"(,"initialSupply":)" << initialSupply
         << R"(,"backingModel":)" << backingModel
         << R"(,"decimals":)" << decimals
         << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createToken", params.str());

  std::cout << std::endl;
  std::cout << "Result: " << result << std::endl;
  std::cout << std::endl;

  if (result.find("true") != std::string::npos)
    std::cout << "Token $" << symbol << " created successfully!" << std::endl;
  else if (result.find("error") != std::string::npos)
    std::cout << "Error creating token. Sidechain may need to be reset." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainTransferMenu(const Config &cfg, const std::string &spendPubHex, cn::Currency &currency)
{
  clearScreen();
  std::cout << "=== Send Token ===" << std::endl;
  std::cout << std::endl;

  std::string to;
  uint64_t amount, tokenId;

  std::cout << "Step 1/3: Recipient address (base58 or hex): ";
  std::getline(std::cin, to);
  if (to.empty())
  {
    std::cout << "Address cannot be empty. Cancelled." << std::endl;
    std::cout << "Press enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  std::string toPubHex = addressToHexPubKey(to, currency);

  std::cout << "Step 2/3: Amount: ";
  std::cin >> amount;
  if (amount == 0)
  {
    std::cout << "Amount must be greater than 0. Cancelled." << std::endl;
    std::cin.ignore();
    std::cout << "Press enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Step 3/3: Token ID (0=SCCX native, check S2 for others): ";
  std::cin >> tokenId;
  std::cin.ignore();

  std::ostringstream params;
  params << R"({"from":")" << spendPubHex << R"(")"
         << R"(,"to":")" << toPubHex << R"(")"
         << R"(,"amount":)" << amount
         << R"(,"tokenId":)" << tokenId << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "transfer", params.str());
  std::cout << std::endl;
  std::cout << "Result: " << result << std::endl;
  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainBalanceMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Token Balances ===" << std::endl;
  std::cout << std::endl;

  std::ostringstream nativeParams;
  nativeParams << R"({"address":")" << spendPubHex << R"(","tokenId":0})";
  std::string nativeBal = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokenBalance", nativeParams.str());
  std::cout << "Native SCCX: " << nativeBal << std::endl;
  std::cout << std::endl;

  std::string tokensResult = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokens", "{}");
  std::cout << "Token balances:" << std::endl;
  std::cout << tokensResult << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainStatusMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== Sidechain Status ===" << std::endl;
  std::cout << std::endl;
  auto summary = getSidechainSummary(cfg);
  if (!summary.connected)
  {
    std::cout << "Sidechain unreachable." << std::endl;
  }
  else
  {
    std::cout << "Network Status:" << std::endl;
    std::cout << "  Height: " << summary.height << std::endl;
    std::cout << "  Unique tokens: " << summary.tokenCount << std::endl;
    std::cout << "  CCX backing tokens: " << summary.totalBackedCCX << std::endl;
    std::cout << "  Pending transactions: " << summary.pendingTx << std::endl;
    std::cout << std::endl;
    std::cout << "Connection: " << cfg.sidechainHost << ":" << cfg.sidechainPort << std::endl;
    std::cout << "Mode: " << (cfg.sidechainTestnet ? "TESTNET" : "MAINNET-STAGING") << std::endl;
  }
  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainQuickCreateMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Quick Create Token ===" << std::endl;
  std::cout << std::endl;

  std::string name = "test" + std::to_string(time(nullptr) % 10000);
  std::string symbol = "T" + std::to_string(time(nullptr) % 1000);
  uint64_t initialSupply = 1000;
  int backingModel = 0;
  uint8_t decimals = 6;

  std::cout << "Creating token with defaults:" << std::endl;
  std::cout << "  Name: " << name << std::endl;
  std::cout << "  Symbol: " << symbol << std::endl;
  std::cout << "  Supply: " << initialSupply << std::endl;
  std::cout << "  Decimals: " << (int)decimals << std::endl;
  std::cout << "  Model: Unbacked" << std::endl;
  std::cout << std::endl;

  std::string nameHex = common::toHex(common::asBinaryArray(name));
  std::string symbolHex = common::toHex(common::asBinaryArray(symbol));

  std::ostringstream params;
  params << "{"
         << R"("from":")" << spendPubHex << R"(")"
         << R"(,"nameHex":")" << nameHex << R"(")"
         << R"(,"symbolHex":")" << symbolHex << R"(")"
         << R"(,"initialSupply":)" << initialSupply
         << R"(,"backingModel":)" << backingModel
         << R"(,"decimals":)" << (int)decimals
         << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createToken", params.str());

  std::cout << "Result: " << result << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Token $" << symbol << " created!" << std::endl;
  else
    std::cout << "Failed to create token." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

int main(int argc, char *argv[])
{
  Config cfg;
  if (!parseArgs(argc, argv, cfg))
    return 1;

  crypto::SecretKey viewKey;
  if (!BoltSync::hexToSecretKey(cfg.viewKeyHex, viewKey))
  {
    std::cerr << "Invalid view key" << std::endl;
    return 1;
  }
  crypto::SecretKey spendKey;
  bool hasSpendKey = !cfg.spendKeyHex.empty();
  if (hasSpendKey && !BoltSync::hexToSecretKey(cfg.spendKeyHex, spendKey))
  {
    std::cerr << "Invalid spend key" << std::endl;
    return 1;
  }

  crypto::PublicKey viewPub, spendPub;
  crypto::secret_key_to_public_key(viewKey, viewPub);
  if (hasSpendKey)
    crypto::secret_key_to_public_key(spendKey, spendPub);

  std::string spendPubHex = common::podToHex(spendPub);

  logging::LoggerManager logManager;
  logging::ConsoleLogger logger;
  Currency currency = CurrencyBuilder(logManager).currency();
  std::string addressStr = currency.accountAddressAsString({spendPub, viewPub});

  std::cout << "\nConceal Bolt Wallet" << std::endl;
  std::cout << "Address: " << addressStr << std::endl;

  if (cfg.sidechainTestnet)
  {
    std::cout << "\nTestnet mode — connect wallet to running sidechain validator" << std::endl;
  }

  platform_system::Dispatcher dispatcher;
  NodeRpcProxy node(dispatcher, cfg.daemonHost, cfg.daemonPort, logger);

  bool daemonConnected = false;
  if (!cfg.sidechainTestnet)
  {
    try
    {
      NodeInitObserver initObs;
      node.init([&initObs](std::error_code ec)
                { initObs.initCompleted(ec); });
      initObs.waitForInitEnd();
      daemonConnected = true;
    }
    catch (...)
    {
      daemonConnected = false;
    }
  }

  std::cout << "Sidechain: " << cfg.sidechainHost << ":" << cfg.sidechainPort
            << " [" << (cfg.sidechainTestnet ? "TESTNET" : "MAINNET") << "]" << std::endl;

  // Only scan and build wallet if daemon is connected
  std::unique_ptr<BoltCore::Wallet> walletPtr;
  std::vector<BoltSync::FoundOutput> allOutputs;
  std::vector<BoltCore::OutputInfo> outputInfos;

  if (daemonConnected && !cfg.skipScan && !cfg.dataDir.empty())
  {
    std::cout << "\nScanning main chain... This may take a few minutes.\n"
              << std::endl;
    BoltSync::Scanner scanner(viewKey, viewPub, hasSpendKey ? &spendKey : nullptr);
    BoltSync::ScanConfig scanCfg;
    scanCfg.dataDir = cfg.dataDir;
    scanCfg.numThreads = cfg.scanThreads;
    BoltSync::ScanState state;
    if (scanner.scan(scanCfg, state))
    {
      allOutputs = std::move(state.results);
      std::cout << "Scan complete. Found " << allOutputs.size() << " outputs.\n"
                << std::endl;
    }
  }

  if (daemonConnected)
  {
    for (const auto &fo : allOutputs)
    {
      BoltCore::OutputInfo info;
      info.blockHeight = fo.blockHeight;
      info.txHash = fo.txHash;
      info.outputIndex = fo.outputIndex;
      info.globalOutputIndex = fo.outputIndex;
      info.amount = fo.amount;
      info.outputKey = fo.outputKey;
      info.txPublicKey = fo.txPublicKey;
      info.keyImage = fo.keyImage;
      info.spent = fo.spent;
      info.isDeposit = false;
      info.term = 0;
      info.subAddress = addressStr;
      outputInfos.push_back(info);
    }

    walletPtr.reset(new BoltCore::Wallet(viewKey, spendKey, viewPub, spendPub, node, currency));
    walletPtr->loadOutputs(outputInfos);
  }

  std::string input;
  while (true)
  {
    clearScreen();
    auto summary = getSidechainSummary(cfg);
    BoltCore::Balance balance{0, 0, 0, 0};

    if (walletPtr)
    {
      balance = walletPtr->getBalance();
    }

    bool daemonLive = false;
    if (daemonConnected)
    {
      try
      {
        uint32_t testHeight = node.getLastLocalBlockHeight();
        daemonLive = (testHeight > 0);
      }
      catch (...)
      {
        daemonLive = false;
      }
    }

    std::cout << "=== Conceal Bolt Wallet ============" << std::endl;
    std::cout << "Address: " << addressStr << std::endl;
    std::cout << "Main Chain: [" << (daemonLive ? "ONLINE" : "OFFLINE") << "]" << std::endl;
    std::cout << "Sidechain: [" << cfg.sidechainHost << ":" << cfg.sidechainPort
              << " " << (cfg.sidechainTestnet ? "TESTNET" : "STAGING") << "]"
              << " [" << (summary.connected ? "CONNECTED" : "UNREACHABLE") << "]" << std::endl;
    std::cout << "  Height: " << summary.height << " | Tokens: " << summary.tokenCount
              << " | Backed CCX: " << summary.totalBackedCCX << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "CCX Balance: " << formatAmount(balance.actual)
              << " (pending " << formatAmount(balance.pending) << ")" << std::endl;
    std::cout << "Deposits locked: " << formatAmount(balance.lockedDeposit)
              << " unlocked: " << formatAmount(balance.unlockedDeposit) << std::endl;
    std::cout << "------------------------------------" << std::endl;

    if (walletPtr)
    {
      std::cout << "1. Send CCX transfer" << std::endl;
      std::cout << "2. Create deposit" << std::endl;
      std::cout << "3. Withdraw deposit" << std::endl;
      std::cout << "4. Optimize (fusion)" << std::endl;
      std::cout << "5. Generate sub-address" << std::endl;
      std::cout << "------------------------------------" << std::endl;
    }
    std::cout << "S1. Sidechain status" << std::endl;
    std::cout << "S2. List sidechain tokens" << std::endl;
    std::cout << "S3. Create sidechain token" << std::endl;
    std::cout << "S4. Send sidechain token" << std::endl;
    std::cout << "S5. Token balances" << std::endl;
    std::cout << "S6. Quick create token" << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "0. Exit" << std::endl;
    std::cout << "Choice: ";
    std::getline(std::cin, input);

    if (walletPtr && input == "1")
    {
      std::string destAddr;
      uint64_t amount;
      std::cout << "Destination address: ";
      std::getline(std::cin, destAddr);
      std::cout << "Amount: ";
      std::cin >> amount;
      std::cin.ignore();
      auto res = walletPtr->transfer(destAddr, amount);
      std::cout << (res.success ? "Sent! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (walletPtr && input == "2")
    {
      uint64_t amount;
      uint32_t term;
      std::cout << "Amount: ";
      std::cin >> amount;
      std::cout << "Term (blocks): ";
      std::cin >> term;
      std::cin.ignore();
      auto res = walletPtr->createDeposit(amount, term);
      std::cout << (res.success ? "Deposit created! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (walletPtr && input == "3")
    {
      uint64_t depositId;
      std::cout << "Deposit ID: ";
      std::cin >> depositId;
      std::cin.ignore();
      auto res = walletPtr->withdrawDeposit(depositId);
      std::cout << (res.success ? "Withdrawn! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (walletPtr && input == "4")
    {
      auto est = walletPtr->estimateFusion(1000000);
      std::cout << "Fusion ready: " << est.fusionReadyCount << " outputs (total: " << est.totalOutputCount << ")" << std::endl;
      if (est.fusionReadyCount > 0)
      {
        std::cout << "Create fusion? (y/n): ";
        std::string yn;
        std::getline(std::cin, yn);
        if (yn == "y")
        {
          auto res = walletPtr->createFusion(1000000, cn::parameters::MINIMUM_MIXIN);
          std::cout << (res.success ? "Fusion tx: " + res.txHash : "Error: " + res.error) << std::endl;
        }
      }
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (walletPtr && input == "5")
    {
      auto sub = walletPtr->generateSubAddress();
      std::cout << "New sub-address: " << sub.address << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (input == "S1" || input == "s1")
    {
      sidechainStatusMenu(cfg);
    }
    else if (input == "S2" || input == "s2")
    {
      sidechainTokensMenu(cfg);
    }
    else if (input == "S3" || input == "s3")
    {
      sidechainCreateTokenMenu(cfg, spendPubHex);
    }
    else if (input == "S4" || input == "s4")
    {
      sidechainTransferMenu(cfg, spendPubHex, currency);
    }
    else if (input == "S5" || input == "s5")
    {
      sidechainBalanceMenu(cfg, spendPubHex);
    }
    else if (input == "S6" || input == "s6")
    {
      sidechainQuickCreateMenu(cfg, spendPubHex);
    }
    else if (input == "0")
    {
      break;
    }
  }

  return 0;
}