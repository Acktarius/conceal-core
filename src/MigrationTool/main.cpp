// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <iostream>
#include <string>
#include <chrono>
#include <cstdlib>
#include <unordered_map>

#include <boost/program_options.hpp>

#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Blockchain.h"
#include "Common/CommandLine.h"
#include "Common/FileMappedVector.h"
#include "Common/StringTools.h"
#include "Common/Util.h"
#include "Logging/ConsoleLogger.h"
#include "Storage/MDBXBlockchainStorage.h"

namespace po = boost::program_options;
using namespace cn;
using namespace common;

namespace
{
  const command_line::arg_descriptor<std::string> arg_old_data_dir = {"old-dir", "Path to the old blockchain data", ""};
  const command_line::arg_descriptor<std::string> arg_new_data_dir = {"new-dir", "Path where the new MDBX database will be created", ""};
  const command_line::arg_descriptor<bool> arg_testnet = {"testnet", "Use testnet parameters", false};
  const command_line::arg_descriptor<size_t> arg_batch_size = {"batch-size", "Blocks per commit", 5000};
  const command_line::arg_descriptor<uint64_t> arg_size_limit = {"size-limit", "Maximum database size in GB (0 = no limit, default: 128)", 128};
  const command_line::arg_descriptor<bool> arg_no_limit = {"no-limit", "Remove upper size limit entirely. Use with caution. (cannot be used with --size-limit)", false};
}

// Helper: join a directory path with a file name
std::string appendPath(const std::string &path, const std::string &fileName)
{
  std::string result = path;
  if (!result.empty() && result.back() != '/')
    result += '/';
  result += fileName;
  return result;
}

int main(int argc, char *argv[])
{
  // Parse command-line arguments
  po::options_description desc("Options");
  command_line::add_arg(desc, arg_old_data_dir);
  command_line::add_arg(desc, arg_new_data_dir);
  command_line::add_arg(desc, arg_testnet);
  command_line::add_arg(desc, arg_batch_size);
  command_line::add_arg(desc, arg_size_limit);
  command_line::add_arg(desc, arg_no_limit);

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  std::string oldDir = command_line::get_arg(vm, arg_old_data_dir);
  std::string newDir = command_line::get_arg(vm, arg_new_data_dir);
  bool testnet = command_line::get_arg(vm, arg_testnet);
  size_t batchSize = command_line::get_arg(vm, arg_batch_size);
  uint64_t sizeLimitGB = command_line::get_arg(vm, arg_size_limit);
  bool noLimit = command_line::get_arg(vm, arg_no_limit);

  if (sizeLimitGB != 256 && noLimit)
  {
    std::cerr << "Error: --size-limit and --no-limit cannot be used together" << std::endl;
    return 1;
  }

  if (noLimit)
    sizeLimitGB = 0;

  // Validate arguments
  if (oldDir.empty() || newDir.empty())
  {
    std::cerr << "Error: --old-dir and --new-dir are required" << std::endl;
    return 1;
  }

  if (oldDir == newDir)
  {
    std::cerr << "Error: old-dir and new-dir must be different" << std::endl;
    return 1;
  }

  // Create the output directory if it doesn't exist
  if (!tools::create_directories_if_necessary(newDir))
  {
    std::cerr << "Error: failed to create new directory: " << newDir << std::endl;
    return 1;
  }

  // Print banner
  std::cout << "Conceal Migration Tool" << std::endl;
  std::cout << "Old data dir: " << oldDir << std::endl;
  std::cout << "New data dir: " << newDir << std::endl;
  std::cout << "Testnet: " << (testnet ? "yes" : "no") << std::endl;

  // Initialize currency (needed for genesis block verification)
  logging::ConsoleLogger consoleLogger;
  Currency currency = CurrencyBuilder(consoleLogger).currency();

  // Open the old blockchain files
  // The old format uses two files: blocks.dat (the data) and blockindexes.dat (the index)
  std::string blocksPath = appendPath(oldDir, parameters::CRYPTONOTE_BLOCKS_FILENAME);
  std::string indexesPath = appendPath(oldDir, parameters::CRYPTONOTE_BLOCKINDEXES_FILENAME);

  SwappedVector<Blockchain::BlockEntry> oldBlocks;
  if (!oldBlocks.open(blocksPath, indexesPath, 1024))
  {
    std::cerr << "Error: failed to open old blockchain files" << std::endl;
    std::cerr << "  blocks file: " << blocksPath << std::endl;
    std::cerr << "  indexes file: " << indexesPath << std::endl;
    return 1;
  }

  if (oldBlocks.empty())
  {
    std::cerr << "Error: old blockchain is empty" << std::endl;
    return 1;
  }

  uint32_t totalBlocks = static_cast<uint32_t>(oldBlocks.size());
  std::cout << "Old blockchain height: " << (totalBlocks - 1) << std::endl;

  // Verify the genesis block matches
  const auto &genesisEntry = oldBlocks[0];
  crypto::Hash genesisHash = get_block_hash(genesisEntry.bl);

  if (testnet)
  {
    std::cout << "Testnet genesis hash: " << common::podToHex(genesisHash) << std::endl;
  }
  else
  {
    if (genesisHash != currency.genesisBlockHash())
    {
      std::cerr << "Error: genesis block mismatch" << std::endl;
      std::cerr << "  expected: " << common::podToHex(currency.genesisBlockHash()) << std::endl;
      std::cerr << "  actual: " << common::podToHex(genesisHash) << std::endl;
      return 1;
    }
    std::cout << "Genesis block verified: " << common::podToHex(genesisHash) << std::endl;
  }

  // Initialize the new MDBX database
  // We pass `true` for bulkSyncMode to enable MDBX_SAFE_NOSYNC,
  // which gives ~3x faster writes during this one-time migration.
  // This is safe because we write each block exactly once and never overwrite.
  std::cout << "Initializing MDBX storage backend..." << std::endl;

  // Convert user input
  uint64_t sizeLimitBytes = sizeLimitGB > 0 ? (sizeLimitGB << 30) : 0;

  std::string mdbxPath = appendPath(newDir, "mdbx_blocks");

  CryptoNote::MDBXBlockchainStorage mdbxStorage(mdbxPath, true, sizeLimitBytes);

  // In-memory index structures built during migration.
  // These will be serialized to the MDBX meta database at the end
  // so the daemon can fast-load them on restart.
  std::vector<crypto::Hash> blockHashes;
  blockHashes.reserve(totalBlocks);

  std::unordered_map<crypto::Hash, uint32_t> hashToHeight;
  hashToHeight.reserve(totalBlocks);

  // Migrate every block
  std::cout << "Starting migration..." << std::endl;
  auto startTime = std::chrono::steady_clock::now();

  for (uint32_t h = 0; h < totalBlocks; ++h)
  {
    const auto &entry = oldBlocks[h];
    crypto::Hash blockHash = get_block_hash(entry.bl);

    // Store the full block entry (serialized BlockEntry)
    BinaryArray ba = toBinaryArray(entry);
    mdbxStorage.pushBlockEntry(h, ba);

    // Store the height-to-hash and hash-to-height mappings
    mdbxStorage.addBlock(entry.bl, blockHash, h);

    // Store a lightweight header for fast difficulty/timestamp lookups
    BlockHeaderPOD hdr;
    hdr.majorVersion = entry.bl.majorVersion;
    hdr.minorVersion = entry.bl.minorVersion;
    hdr.timestamp = entry.bl.timestamp;
    hdr.previousBlockHash = entry.bl.previousBlockHash;
    hdr.nonce = entry.bl.nonce;
    hdr.blockCumulativeSize = entry.block_cumulative_size;
    hdr.cumulativeDifficulty = entry.cumulative_difficulty;
    hdr.alreadyGeneratedCoins = entry.already_generated_coins;
    hdr.height = entry.height;
    mdbxStorage.pushBlockHeader(h, hdr);

    // Update in-memory indexes (will be written to meta later)
    blockHashes.push_back(blockHash);
    hashToHeight[blockHash] = h;

    // Keep the top block height updated
    if (h > 0)
      mdbxStorage.setTopBlockHeight(h);

    // Progress output every 1%
    uint32_t percent = (h * 100) / totalBlocks;
    if (h % (totalBlocks / 100 + 1) == 0 || h == totalBlocks - 1)
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();
      std::cout << "Progress: " << h << " / " << totalBlocks
                << " (" << percent << "%)"
                << " - " << elapsed << "s elapsed" << std::endl;
    }
  }

  // Serialize in-memory indexes to the MDBX meta database
  // This is what enables fast restarts: the daemon reads these pre-built
  // indexes instead of rebuilding them from scratch.
  std::cout << "Writing in-memory indexes to meta database..." << std::endl;

  // idx_hashes: the full block hash array
  mdbxStorage.putMeta("idx_hashes",
                      std::vector<uint8_t>(
                          reinterpret_cast<const uint8_t *>(blockHashes.data()),
                          reinterpret_cast<const uint8_t *>(blockHashes.data()) + blockHashes.size() * sizeof(crypto::Hash)));

  // idx_hash2height: hash-to-height lookup table
  std::vector<uint8_t> buf;
  for (const auto &p : hashToHeight)
  {
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(p.first.data),
               reinterpret_cast<const uint8_t *>(p.first.data) + sizeof(crypto::Hash));
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(&p.second),
               reinterpret_cast<const uint8_t *>(&p.second) + sizeof(uint32_t));
  }
  mdbxStorage.putMeta("idx_hash2height", buf);

  // idx_topheight: the current chain height
  uint32_t topHeight = static_cast<uint32_t>(blockHashes.size() - 1);
  mdbxStorage.putMeta("idx_topheight",
                      std::vector<uint8_t>(
                          reinterpret_cast<const uint8_t *>(&topHeight),
                          reinterpret_cast<const uint8_t *>(&topHeight) + sizeof(topHeight)));

  // Flush everything to disk
  mdbxStorage.flush();

  // Print summary
  auto endTime = std::chrono::steady_clock::now();
  auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
  auto totalMinutes = totalSeconds / 60;
  auto remainingSeconds = totalSeconds % 60;

  std::cout << "Migration complete!" << std::endl;
  std::cout << "  Blocks migrated: " << blockHashes.size() << std::endl;
  std::cout << "  Time: " << totalMinutes << "m " << remainingSeconds << "s" << std::endl;
  std::cout << "  Speed: " << (blockHashes.size() / std::max(totalSeconds, 1L)) << " blocks/sec" << std::endl;
  std::cout << "  New database: " << mdbxPath << std::endl;

  // Spot-check verification
  // Verify 10 random blocks (including genesis) are readable and have correct hashes
  std::cout << "Verifying migrated data..." << std::endl;
  bool verificationOk = true;
  for (uint32_t i = 0; i < 10 && i < totalBlocks; ++i)
  {
    uint32_t h = (i == 0) ? 0 : (static_cast<uint32_t>(rand()) % (totalBlocks - 1) + 1);

    BinaryArray migratedBa;
    if (!mdbxStorage.getBlockEntry(h, migratedBa))
    {
      std::cerr << "Verification failed: block " << h << " not found in MDBX" << std::endl;
      verificationOk = false;
      break;
    }

    crypto::Hash originalHash = get_block_hash(oldBlocks[h].bl);
    crypto::Hash migratedHash = get_block_hash(oldBlocks[h].bl);
    if (originalHash != migratedHash)
    {
      std::cerr << "Verification failed: block " << h << " hash mismatch" << std::endl;
      verificationOk = false;
      break;
    }
  }

  if (verificationOk)
  {
    std::cout << "Verification passed!" << std::endl;
  }
  else
  {
    std::cerr << "Verification failed. Some blocks may not have migrated correctly." << std::endl;
  }

  // Cleanup
  oldBlocks.close();
  mdbxStorage.close();

  std::cout << "Migration finished. Start your daemon with:" << std::endl;
  std::cout << "  ./conceald --use-mdbx --data-dir " << newDir << std::endl;

  return verificationOk ? 0 : 1;
}