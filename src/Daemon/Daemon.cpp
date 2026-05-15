// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2016-2022, The Karbo developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "version.h"

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "DaemonCommandsHandler.h"
#include "Blockchain/Checkpoints.h"
#include "Common/PathTools.h"
#include "Common/SignalHandler.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/CoreConfig.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/MinerConfig.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "P2p/NetNode.h"
#include "P2p/NetNodeConfig.h"
#include "Rpc/RpcServer.h"
#include "Rpc/RpcServerConfig.h"
#include "Storage/MDBXBlockchainStorage.h"
#include "crypto/hash.h"
#include "version.h"

#if defined(WIN32)
#include <crtdbg.h>
#endif

using common::JsonValue;
using namespace cn;
using namespace logging;

namespace po = boost::program_options;

namespace
{
  const command_line::arg_descriptor<std::string> arg_config_file = {
      "config-file", "Specify configuration file", "conceal.conf"};
  const command_line::arg_descriptor<bool> arg_os_version = {"os-version", ""};
  const command_line::arg_descriptor<std::string> arg_log_file = {"log-file", "", ""};
  const command_line::arg_descriptor<std::string> arg_set_fee_address = {
      "fee-address", "Set a fee address for remote nodes", ""};
  const command_line::arg_descriptor<std::string> arg_set_view_key = {
      "view-key", "Set secret view-key for remote node fee confirmation", ""};
  const command_line::arg_descriptor<int> arg_log_level = {"log-level", "", 2};
  const command_line::arg_descriptor<bool> arg_console = {
      "no-console", "Disable daemon console commands"};
  const command_line::arg_descriptor<bool> arg_print_genesis_tx = {
      "print-genesis-tx", "Prints genesis' block tx hex to insert it to config and exits"};
  const command_line::arg_descriptor<bool> arg_use_mdbx = {
      "use-mdbx", "Use MDBX database backend for faster sync", false};
  const command_line::arg_descriptor<bool> arg_export_snapshot = {
      "export-snapshot", "Export blockchain headers snapshot to file", false};
  const command_line::arg_descriptor<std::string> arg_import_snapshot = {
      "import-snapshot", "Import blockchain headers snapshot from file", ""};
  const command_line::arg_descriptor<uint32_t> arg_rollback_height = {
      "rollback-height", "Rollback blockchain to this height before starting (MDBX only, debug)", 0};
}

// ── Genesis tx printing ─────────────────────────────────────────────────

void print_genesis_tx_hex()
{
  logging::ConsoleLogger logger;
  cn::Transaction tx = cn::CurrencyBuilder(logger).generateGenesisTransaction();
  cn::BinaryArray txb = cn::toBinaryArray(tx);
  std::string tx_hex = common::toHex(txb);
  std::cout << "Random genesis hex: " << tx_hex << std::endl;
}

// ── Logger configuration ────────────────────────────────────────────────

JsonValue buildLoggerConfiguration(Level level, const std::string &logfile)
{
  JsonValue loggerConfiguration(JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  JsonValue &cfgLoggers = loggerConfiguration.insert("loggers", JsonValue::ARRAY);

  JsonValue &fileLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(TRACE));

  JsonValue &consoleLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(TRACE));
  consoleLogger.insert("pattern", "%T %L ");

  return loggerConfiguration;
}

// ── Snapshot export ─────────────────────────────────────────────────────

bool exportSnapshot(const std::string &dataDir, const std::string &outputFile,
                    logging::LoggerRef &logger)
{
  logger(INFO) << "Exporting blockchain snapshot to " << outputFile;

  std::string dbPath = dataDir;
  if (!dbPath.empty() && dbPath.back() != '/')
    dbPath += '/';
  dbPath += "mdbx_blocks";

  CryptoNote::MDBXBlockchainStorage storage(dbPath, false);
  uint32_t topHeight = storage.topBlockHeight();

  if (topHeight == 0)
  {
    logger(ERROR) << "Blockchain is empty, nothing to export";
    return false;
  }

  logger(INFO) << "Blockchain height: " << topHeight;

  std::ofstream file(outputFile, std::ios::binary);
  if (!file)
  {
    logger(ERROR) << "Failed to open output file: " << outputFile;
    return false;
  }

  const uint32_t magic = 0x43434E58;
  const uint32_t version = 1;
  file.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char *>(&version), sizeof(version));
  file.write(reinterpret_cast<const char *>(&topHeight), sizeof(topHeight));

  for (uint32_t h = 1; h <= topHeight; ++h)
  {
    if (h % 100000 == 0)
      logger(INFO) << "Exporting block " << h << "/" << topHeight;

    cn::BlockHeaderPOD hdr;
    if (!storage.getBlockHeader(h, hdr))
    {
      logger(WARNING) << "No header for block " << h << ", skipping";
      continue;
    }

    crypto::Hash blockHash = storage.getBlockHash(h);
    file.write(reinterpret_cast<const char *>(&hdr), sizeof(cn::BlockHeaderPOD));
    file.write(reinterpret_cast<const char *>(blockHash.data), sizeof(crypto::Hash));
  }

  auto checkpoints = storage.getCheckpoints();
  uint32_t checkpointCount = static_cast<uint32_t>(checkpoints.size());
  file.write(reinterpret_cast<const char *>(&checkpointCount), sizeof(checkpointCount));
  for (const auto &cp : checkpoints)
  {
    uint32_t height = cp.first;
    crypto::Hash hash = cp.second;
    file.write(reinterpret_cast<const char *>(&height), sizeof(height));
    file.write(reinterpret_cast<const char *>(hash.data), sizeof(crypto::Hash));
  }

  file.close();

  uint64_t fileSize = topHeight * (sizeof(cn::BlockHeaderPOD) + sizeof(crypto::Hash));
  logger(INFO, BRIGHT_GREEN) << "Snapshot exported: " << outputFile
                             << " (" << (fileSize / 1024 / 1024) << " MB approx, "
                             << checkpointCount << " checkpoints)";

  return true;
}

// ── Snapshot import ─────────────────────────────────────────────────────

bool importSnapshot(const std::string &dataDir, const std::string &inputFile,
                    logging::LoggerRef &logger)
{
  logger(INFO) << "Importing blockchain snapshot from " << inputFile;

  std::ifstream file(inputFile, std::ios::binary);
  if (!file)
  {
    logger(ERROR) << "Failed to open input file: " << inputFile;
    return false;
  }

  uint32_t magic, version, topHeight;
  file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
  file.read(reinterpret_cast<char *>(&version), sizeof(version));
  file.read(reinterpret_cast<char *>(&topHeight), sizeof(topHeight));

  if (magic != 0x43434E58)
  {
    logger(ERROR) << "Invalid snapshot file (bad magic)";
    return false;
  }
  if (version != 1)
  {
    logger(ERROR) << "Unsupported snapshot version: " << version;
    return false;
  }

  logger(INFO) << "Snapshot contains " << topHeight << " blocks";

  if (!tools::directoryExists(dataDir))
  {
    if (!tools::create_directories_if_necessary(dataDir))
    {
      logger(ERROR) << "Failed to create data directory: " << dataDir;
      return false;
    }
  }

  std::string dbPath = dataDir;
  if (!dbPath.empty() && dbPath.back() != '/')
    dbPath += '/';
  dbPath += "mdbx_blocks";

  boost::system::error_code ec;
  boost::filesystem::remove_all(dbPath, ec);

  CryptoNote::MDBXBlockchainStorage storage(dbPath, true, 0);

  for (uint32_t h = 1; h <= topHeight; ++h)
  {
    if (h % 100000 == 0)
      logger(INFO) << "Importing block " << h << "/" << topHeight;

    cn::BlockHeaderPOD hdr;
    crypto::Hash blockHash;

    file.read(reinterpret_cast<char *>(&hdr), sizeof(cn::BlockHeaderPOD));
    file.read(reinterpret_cast<char *>(blockHash.data), sizeof(crypto::Hash));

    if (!file)
    {
      logger(ERROR) << "Failed to read block at height " << h << " from snapshot";
      return false;
    }

    storage.pushBlockHeader(h, hdr);
    storage.addBlock(cn::Block(), blockHash, h);
  }

  storage.setTopBlockHeight(topHeight);

  uint32_t checkpointCount;
  file.read(reinterpret_cast<char *>(&checkpointCount), sizeof(checkpointCount));

  for (uint32_t i = 0; i < checkpointCount; ++i)
  {
    uint32_t height;
    crypto::Hash hash;
    file.read(reinterpret_cast<char *>(&height), sizeof(height));
    file.read(reinterpret_cast<char *>(hash.data), sizeof(crypto::Hash));
    storage.storeCheckpoint(height, hash);
  }

  storage.setInitialized();
  storage.flush();
  storage.close();
  file.close();

  logger(INFO, BRIGHT_GREEN) << "Snapshot imported successfully!";
  logger(INFO) << "  Blocks: " << topHeight;
  logger(INFO) << "  Checkpoints: " << checkpointCount;
  logger(INFO) << "  Database created at: " << dbPath;
  logger(INFO) << "  Start daemon normally to begin syncing remaining blocks.";

  return true;
}

// ── MDBX recovery ───────────────────────────────────────────────────────

void recoverMdbxDatabase(const std::string &configFolder, uint32_t rollbackHeight,
                         logging::LoggerRef &logger)
{
  std::string dbPath = configFolder;
  if (!dbPath.empty() && dbPath.back() != '/')
    dbPath += '/';
  dbPath += "mdbx_blocks";

  if (!boost::filesystem::exists(dbPath))
    return;

  CryptoNote::MDBXBlockchainStorage storage(dbPath, false);
  uint32_t topHeight = storage.topBlockHeight();

  // Manual rollback requested via flag
  if (rollbackHeight > 0 && rollbackHeight < topHeight)
  {
    logger(INFO, BRIGHT_YELLOW) << "Manual rollback: removing blocks above height "
                                << rollbackHeight;

    for (uint32_t h = rollbackHeight + 1; h <= topHeight; ++h)
    {
      crypto::Hash blockHash = storage.getBlockHash(h);
      if (blockHash != NULL_HASH)
        storage.removeBlock(blockHash);
      storage.popBlockEntry(h);
      storage.removeBlockHeader(h);
    }

    storage.setTopBlockHeight(rollbackHeight);
    storage.putMeta("idx_hashes", std::vector<uint8_t>());
    storage.putMeta("idx_topheight", std::vector<uint8_t>());
    storage.flush();

    logger(INFO, BRIGHT_GREEN) << "Manual rollback complete! Removed "
                               << (topHeight - rollbackHeight)
                               << " blocks. Chain height is now " << rollbackHeight;
    topHeight = rollbackHeight;
  }

  if (topHeight > 0)
  {
    bool needsRecovery = false;

    cn::BinaryArray ba;
    if (!storage.getBlockEntry(topHeight, ba) || ba.empty())
    {
      logger(WARNING, BRIGHT_YELLOW) << "Missing/corrupt block entry at height "
                                     << topHeight;
      needsRecovery = true;
    }

    if (needsRecovery)
    {
      uint32_t recoverTo = topHeight > 100 ? topHeight - 100 : 0;
      logger(INFO, BRIGHT_YELLOW) << "Auto-recovering: removing blocks above height "
                                  << recoverTo;

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

      logger(INFO, BRIGHT_GREEN) << "Auto-recovery complete! Removed "
                                 << (topHeight - recoverTo)
                                 << " blocks. Will rebuild indexes from height "
                                 << recoverTo;
    }
  }

  storage.close();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
#ifdef _WIN32
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  LoggerManager logManager;
  LoggerRef logger(logManager, "daemon");

  try
  {
    po::options_description desc_cmd_only("Command line options");
    po::options_description desc_cmd_sett("Command line options and settings options");

    desc_cmd_sett.add_options()("enable-blockchain-indexes,i", po::bool_switch()->default_value(false),
                                "Enable blockchain indexes")("enable-autosave,a", po::bool_switch()->default_value(true),
                                                             "Enable blockchain autosave every 720 blocks");

    command_line::add_arg(desc_cmd_only, command_line::arg_help);
    command_line::add_arg(desc_cmd_only, command_line::arg_version);
    command_line::add_arg(desc_cmd_only, arg_os_version);
    command_line::add_arg(desc_cmd_only, command_line::arg_data_dir,
                          tools::getDefaultDataDirectory());
    command_line::add_arg(desc_cmd_only, arg_config_file);
    command_line::add_arg(desc_cmd_sett, arg_set_fee_address);
    command_line::add_arg(desc_cmd_sett, arg_log_file);
    command_line::add_arg(desc_cmd_sett, arg_log_level);
    command_line::add_arg(desc_cmd_sett, arg_console);
    command_line::add_arg(desc_cmd_sett, arg_set_view_key);
    command_line::add_arg(desc_cmd_sett, command_line::arg_testnet_on);
    command_line::add_arg(desc_cmd_sett, arg_use_mdbx);
    command_line::add_arg(desc_cmd_sett, arg_print_genesis_tx);
    command_line::add_arg(desc_cmd_sett, arg_export_snapshot);
    command_line::add_arg(desc_cmd_sett, arg_import_snapshot);
    command_line::add_arg(desc_cmd_sett, arg_rollback_height);

    RpcServerConfig::initOptions(desc_cmd_sett);
    NetNodeConfig::initOptions(desc_cmd_sett);
    MinerConfig::initOptions(desc_cmd_sett);

    po::options_description desc_options("Allowed options");
    desc_options.add(desc_cmd_only).add(desc_cmd_sett);

    po::variables_map vm;
    CoreConfig coreConfig;

    bool r = command_line::handle_error_helper(desc_options, [&]()
                                               {
      po::store(po::parse_command_line(argc, argv, desc_options), vm);
      coreConfig.init(vm);
      coreConfig.useMdbx = command_line::get_arg(vm, arg_use_mdbx);

      if (command_line::get_arg(vm, command_line::arg_help))
      {
        std::cout << CCX_RELEASE_VERSION << std::endl << std::endl;
        std::cout << desc_options;
        return false;
      }
      else if (command_line::get_arg(vm, command_line::arg_version))
      {
        std::cout << CCX_RELEASE_VERSION << std::endl;
        return false;
      }
      else if (command_line::get_arg(vm, arg_os_version))
      {
        std::cout << "OS " << tools::get_os_version_string() << std::endl;
        return false;
      }
      else if (command_line::get_arg(vm, arg_print_genesis_tx))
      {
        print_genesis_tx_hex();
        return false;
      }

      std::string data_dir = command_line::get_arg(vm, command_line::arg_data_dir);
      std::string config = command_line::get_arg(vm, arg_config_file);

      boost::filesystem::path data_dir_path(data_dir);
      boost::filesystem::path config_path(config);
      if (!config_path.has_parent_path())
        config_path = data_dir_path / config_path;

      boost::system::error_code ec;
      if (boost::filesystem::exists(config_path, ec))
        po::store(po::parse_config_file<char>(config_path.string<std::string>().c_str(),
                                               desc_cmd_sett), vm);

      po::notify(vm);
      return true; });

    if (!r)
      return 1;

    auto modulePath = common::NativePathToGeneric(argv[0]);
    auto cfgLogFile = common::NativePathToGeneric(command_line::get_arg(vm, arg_log_file));

    if (cfgLogFile.empty())
      cfgLogFile = common::ReplaceExtenstion(modulePath, ".log");
    else if (!common::HasParentPath(cfgLogFile))
      cfgLogFile = common::CombinePath(common::GetPathDirectory(modulePath), cfgLogFile);

    auto cfgLogLevel = static_cast<Level>(static_cast<int>(logging::ERROR) +
                                          command_line::get_arg(vm, arg_log_level));
    logManager.configure(buildLoggerConfiguration(cfgLogLevel, cfgLogFile));

    logger(INFO, BRIGHT_YELLOW) << CCX_RELEASE_VERSION;
    logger(INFO) << "Module folder: " << argv[0];

    // Handle snapshot export (doesn't require full init)
    if (command_line::get_arg(vm, arg_export_snapshot))
    {
      std::string dataDir = command_line::get_arg(vm, command_line::arg_data_dir);
      boost::filesystem::path snapPath = boost::filesystem::current_path();
      snapPath /= "conceal-snapshot.dat";
      bool ok = exportSnapshot(dataDir, snapPath.string(), logger);
      return ok ? 0 : 1;
    }

    // Handle snapshot import (doesn't require full init)
    std::string importFile = command_line::get_arg(vm, arg_import_snapshot);
    if (!importFile.empty())
    {
      std::string dataDir = command_line::get_arg(vm, command_line::arg_data_dir);
      bool ok = importSnapshot(dataDir, importFile, logger);
      return ok ? 0 : 1;
    }

    logger(INFO) << "Blockchain and configuration folder: " << coreConfig.configFolder;
    if (coreConfig.testnet)
      logger(INFO, MAGENTA) << "/!\\ Starting in testnet mode /!\\";

    // Currency validation
    cn::CurrencyBuilder currencyBuilder(logManager);
    currencyBuilder.testnet(coreConfig.testnet);

    try
    {
      currencyBuilder.currency();
    }
    catch (const std::exception &)
    {
      logger(ERROR) << "Incorrect genesis hash! Do not change the genesis hash: "
                    << cn::GENESIS_COINBASE_TX_HEX;
      return 1;
    }

    cn::Currency currency = currencyBuilder.currency();

    cn::core ccore(currency, nullptr, logManager,
                   vm["enable-blockchain-indexes"].as<bool>(),
                   vm["enable-autosave"].as<bool>(),
                   coreConfig.useMdbx);

    cn::Checkpoints checkpoints(logManager);
    checkpoints.set_testnet(coreConfig.testnet);
    checkpoints.load_checkpoints();
    checkpoints.load_checkpoints_from_dns();
    ccore.set_checkpoints(std::move(checkpoints));

    NetNodeConfig netNodeConfig;
    netNodeConfig.init(vm);
    netNodeConfig.setTestnet(coreConfig.testnet);
    netNodeConfig.setConfigFolder(coreConfig.configFolder);

    MinerConfig minerConfig;
    minerConfig.init(vm);

    RpcServerConfig rpcConfig;
    rpcConfig.init(vm);

    if (!coreConfig.configFolderDefaulted)
    {
      if (!tools::directoryExists(coreConfig.configFolder))
        throw std::runtime_error("Directory does not exist: " + coreConfig.configFolder);
    }
    else
    {
      if (!tools::create_directories_if_necessary(coreConfig.configFolder))
        throw std::runtime_error("Can't create directory: " + coreConfig.configFolder);
    }

    platform_system::Dispatcher dispatcher;

    cn::CryptoNoteProtocolHandler cprotocol(currency, dispatcher, ccore, nullptr, logManager);
    cn::NodeServer p2psrv(dispatcher, cprotocol, logManager);
    cn::RpcServer rpcServer(dispatcher, logManager, ccore, p2psrv, cprotocol);

    cprotocol.set_p2p_endpoint(&p2psrv);
    ccore.set_cryptonote_protocol(&cprotocol);

    DaemonCommandsHandler dch(ccore, p2psrv, logManager);

    // MDBX recovery before core init
    if (coreConfig.useMdbx)
    {
      uint32_t rollbackHeight = command_line::get_arg(vm, arg_rollback_height);
      recoverMdbxDatabase(coreConfig.configFolder, rollbackHeight, logger);
    }

    // Initialize P2P
    logger(INFO) << "Initializing p2p server...";
    if (!p2psrv.init(netNodeConfig))
    {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize p2p server.";
      return 1;
    }
    logger(INFO) << "P2p server initialized OK";

    // Initialize core
    logger(INFO) << "Initializing core...";
    if (!ccore.init(coreConfig, minerConfig, true))
    {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize core";
      return 1;
    }
    logger(INFO) << "Core initialized OK";

    // Start console handler
    if (!command_line::has_arg(vm, arg_console))
      dch.start_handling();

    // Configure RPC
    logger(INFO) << "Starting core rpc server on address " << rpcConfig.getBindAddress();

    if (command_line::has_arg(vm, arg_set_fee_address))
    {
      std::string addr_str = command_line::get_arg(vm, arg_set_fee_address);
      if (!addr_str.empty())
      {
        AccountPublicAddress acc = boost::value_initialized<AccountPublicAddress>();
        if (!currency.parseAccountAddressString(addr_str, acc))
        {
          logger(ERROR, BRIGHT_RED) << "Bad fee address: " << addr_str;
          return 1;
        }
        rpcServer.setFeeAddress(addr_str, acc);
        logger(INFO, BRIGHT_YELLOW) << "Remote node fee address set: " << addr_str;
      }
    }

    if (command_line::has_arg(vm, arg_set_view_key))
    {
      std::string vk_str = command_line::get_arg(vm, arg_set_view_key);
      if (!vk_str.empty())
      {
        rpcServer.setViewKey(vk_str);
        logger(INFO, BRIGHT_YELLOW) << "Secret view key set: " << vk_str;
      }
    }

    rpcServer.start(rpcConfig.bindIp, rpcConfig.bindPort);
    rpcServer.enableCors(rpcConfig.enableCors);
    logger(INFO) << "Core rpc server started ok";

    // Signal handling
    tools::SignalHandler::install([&dch, &p2psrv]
                                  {
      dch.stop_handling();
      p2psrv.sendStopSignal(); });

    // Run
    logger(INFO) << "Starting p2p net loop...";
    p2psrv.run();
    logger(INFO) << "p2p net loop stopped";

    // Shutdown
    dch.stop_handling();
    logger(INFO) << "Stopping core rpc server...";
    rpcServer.stop();
    logger(INFO) << "Deinitializing core...";
    ccore.deinit();
    logger(INFO) << "Deinitializing p2p...";
    p2psrv.deinit();

    ccore.set_cryptonote_protocol(nullptr);
    cprotocol.set_p2p_endpoint(nullptr);
  }
  catch (const std::exception &e)
  {
    logger(ERROR, BRIGHT_RED) << "Exception: " << e.what();
    return 1;
  }

  logger(INFO) << "Node stopped.";
  return 0;
}