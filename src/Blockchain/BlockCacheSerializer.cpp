// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain/BlockCacheSerializer.h"
#include "Common/PathHelpers.h"

#include "parallel_hashmap/phmap_dump.h"

namespace cn
{

  void BlockCacheSerializer::serialize(ISerializer &s)
  {
    auto start = std::chrono::steady_clock::now();

    uint8_t version = CURRENT_BLOCKCACHE_STORAGE_ARCHIVE_VER;
    s(version, "version");

    if (version < CURRENT_BLOCKCACHE_STORAGE_ARCHIVE_VER)
      return;

    std::string operation;
    if (s.type() == ISerializer::INPUT)
    {
      operation = "loading ";
      crypto::Hash blockHash;
      s(blockHash, "last_block");

      if (blockHash != m_lastBlockHash)
        return;
    }
    else
    {
      operation = "- saving ";
      s(m_lastBlockHash, "last_block");
    }

    logger(logging::INFO) << operation << "block index";
    s(m_bs.m_blockIndex, "block_index");

    logger(logging::INFO) << operation << "transaction map";
    if (s.type() == ISerializer::INPUT)
    {
      phmap::BinaryInputArchive ar_in(
          PathHelpers::appendPath(m_bs.m_config_folder, "transactionsmap.dat").c_str());
      m_bs.m_transactionMap.phmap_load(ar_in);
    }
    else
    {
      phmap::BinaryOutputArchive ar_out(
          PathHelpers::appendPath(m_bs.m_config_folder, "transactionsmap.dat").c_str());
      m_bs.m_transactionMap.phmap_dump(ar_out);
    }

    logger(logging::INFO) << operation << "spent keys";
    if (s.type() == ISerializer::INPUT)
    {
      phmap::BinaryInputArchive ar_in(
          PathHelpers::appendPath(m_bs.m_config_folder, "spentkeys.dat").c_str());
      m_bs.m_spent_keys.phmap_load(ar_in);
    }
    else
    {
      phmap::BinaryOutputArchive ar_out(
          PathHelpers::appendPath(m_bs.m_config_folder, "spentkeys.dat").c_str());
      m_bs.m_spent_keys.phmap_dump(ar_out);
    }

    logger(logging::INFO) << operation << "outputs";
    s(m_bs.m_outputs, "outputs");

    logger(logging::INFO) << operation << "multi-signature outputs";
    s(m_bs.m_multisignatureOutputs, "multisig_outputs");

    logger(logging::INFO) << operation << "deposit index";
    s(m_bs.m_depositIndex, "deposit_index");

    auto dur = std::chrono::steady_clock::now() - start;
    logger(logging::INFO) << "Serialization time: "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(dur).count()
                          << "ms";

    m_loaded = true;
  }

} // namespace cn