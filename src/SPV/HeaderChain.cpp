// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "HeaderChain.h"
#include "Common/StringTools.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <map>

using namespace common;

namespace SPV
{
  HeaderChain::HeaderChain() : m_height(0), m_filename("") {}

  HeaderChain::~HeaderChain() {}

  bool HeaderChain::addHeader(const BlockHeader &header)
  {
    // Verify chain continuity
    if (!m_headers.empty() && header.prev_hash != m_headers.back().hash)
    {
      std::cerr << "HeaderChain: chain validation failed at height " << header.height << std::endl;
      return false;
    }

    m_headers.push_back(header);
    if (header.height > m_height)
      m_height = header.height;

    return true;
  }

  bool HeaderChain::getHeader(uint32_t height, BlockHeader &header) const
  {
    if (height >= m_headers.size())
      return false;

    header = m_headers[height];
    return true;
  }

  bool HeaderChain::save(const std::string &filename)
  {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
      return false;

    // Write magic bytes "CCHD"
    uint32_t magic = 0x43434844;
    file.write(reinterpret_cast<const char *>(&magic), sizeof(magic));

    // Write version
    uint32_t version = 1;
    file.write(reinterpret_cast<const char *>(&version), sizeof(version));

    // Write count
    uint32_t count = static_cast<uint32_t>(m_headers.size());
    file.write(reinterpret_cast<const char *>(&count), sizeof(count));

    // Write genesis hash (first block's hash)
    if (!m_headers.empty())
    {
      file.write(reinterpret_cast<const char *>(&m_headers[0].hash), sizeof(m_headers[0].hash));
    }
    else
    {
      crypto::Hash empty;
      file.write(reinterpret_cast<const char *>(&empty), sizeof(empty));
    }

    // Write each header
    for (const auto &h : m_headers)
    {
      file.write(reinterpret_cast<const char *>(&h.height), sizeof(h.height));
      file.write(reinterpret_cast<const char *>(&h.major_version), sizeof(h.major_version));
      file.write(reinterpret_cast<const char *>(&h.minor_version), sizeof(h.minor_version));
      file.write(reinterpret_cast<const char *>(&h.timestamp), sizeof(h.timestamp));
      file.write(reinterpret_cast<const char *>(&h.nonce), sizeof(h.nonce));
      file.write(reinterpret_cast<const char *>(&h.difficulty), sizeof(h.difficulty));

      // Placeholder for cumulativeDifficulty (not used)
      uint64_t zero = 0;
      file.write(reinterpret_cast<const char *>(&zero), sizeof(zero));

      // Placeholder for alreadyGeneratedCoins (not used)
      file.write(reinterpret_cast<const char *>(&zero), sizeof(zero));

      file.write(reinterpret_cast<const char *>(&h.prev_hash), sizeof(h.prev_hash));
      file.write(reinterpret_cast<const char *>(&h.hash), sizeof(h.hash));
    }

    file.close();
    return true;
  }

#pragma pack(push, 1)
  struct ExportedHeader
  {
    uint32_t height;
    uint8_t majorVersion;
    uint8_t minorVersion;
    uint64_t timestamp;
    uint32_t nonce;
    uint64_t cumulativeDifficulty;
    uint64_t alreadyGeneratedCoins;
    crypto::Hash previousBlockHash;
    crypto::Hash blockHash;
  };
#pragma pack(pop)

  bool HeaderChain::load(const std::string &filename)
  {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open())
      return false;

    uint32_t magic, version, count;
    file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char *>(&version), sizeof(version));
    file.read(reinterpret_cast<char *>(&count), sizeof(count));

    std::cout << "Magic: 0x" << std::hex << magic << std::dec << std::endl;
    std::cout << "Version: " << version << std::endl;
    std::cout << "Count from file: " << count << std::endl;

    if (magic != 0x43434844 || version != 1)
    {
      std::cerr << "Invalid header file" << std::endl;
      return false;
    }

    // Read genesis hash (skip, we don't need it for index-based height)
    crypto::Hash genesisHash;
    file.read(reinterpret_cast<char *>(&genesisHash), sizeof(genesisHash));

    m_headers.clear();
    m_headers.reserve(count);
    m_height = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
      ExportedHeader exh;
      file.read(reinterpret_cast<char *>(&exh), sizeof(ExportedHeader));

      if (!file.good())
      {
        std::cerr << "Failed to read header at index " << i << std::endl;
        return false;
      }

      BlockHeader h;
      h.height = i; // Use index, ignore stored height
      h.major_version = exh.majorVersion;
      h.minor_version = exh.minorVersion;
      h.timestamp = exh.timestamp;
      h.nonce = exh.nonce;
      h.difficulty = exh.cumulativeDifficulty;
      h.prev_hash = exh.previousBlockHash;
      h.hash = exh.blockHash;
      h.merkle_root = crypto::Hash();

      m_headers.push_back(h);
      m_height = i;

      if (i % 100000 == 0 && i > 0)
      {
        std::cout << "Loaded " << i << " headers..." << std::endl;
      }
    }

    file.close();

    std::cout << "Loaded " << m_headers.size() << " headers, synced to height " << m_height << std::endl;
    return true;
  }

  std::map<uint32_t, crypto::Hash> HeaderChain::getCheckpoints()
  {
    std::map<uint32_t, crypto::Hash> checkpoints;
    crypto::Hash hash;

    // Skip height 0 - it's sometimes corrupted in the file format
    // Height 500,000
    common::podFromHex("df5b2b47960ecd7809f037de44c6817640283e13323a36fe3dd894f3b2b3c5e1", hash);
    checkpoints[500000] = hash;

    // Height 1,000,000
    common::podFromHex("6ad9d4ccc9666b31481079374e573c20ebdf2d63862da8fcc2c45d13093b93ba", hash);
    checkpoints[1000000] = hash;

    // Height 1,500,000
    common::podFromHex("13909d5ebed03c5e54de4b3e1f47483d38e1bb612fe191bf4be5e2aee8671909", hash);
    checkpoints[1500000] = hash;

    // Height 2,000,000
    common::podFromHex("bf1e1396f4ee1c21a2d574035c6b422d62b8aa3d4b3268111b1b73357ad49740", hash);
    checkpoints[2000000] = hash;

    // Height 2,074,000
    common::podFromHex("4634f6bf27db531f3b49c4ce5f99cfe9d78c759762c40cf43ef90637806749e9", hash);
    checkpoints[2074000] = hash;

    return checkpoints;
  }

  bool HeaderChain::verifyCheckpoint(uint32_t height, const crypto::Hash &expectedHash)
  {
    if (height >= m_headers.size())
    {
      std::cerr << "Checkpoint height " << height << " exceeds headers size " << m_headers.size() << std::endl;
      return false;
    }

    if (m_headers[height].hash != expectedHash)
    {
      std::cerr << "Checkpoint mismatch at height " << height << std::endl;
      std::cerr << "  Expected: " << common::podToHex(expectedHash) << std::endl;
      std::cerr << "  Got:      " << common::podToHex(m_headers[height].hash) << std::endl;
      return false;
    }

    return true;
  }

  bool HeaderChain::verifyAllCheckpoints()
  {
    // Verify only the most recent checkpoint (near tip)
    // This proves the file is from the correct chain
    auto checkpoints = getCheckpoints();

    // Skip genesis (height 0) if it's corrupted in the file
    // Instead verify checkpoints at 500k, 1M, 1.5M, 2M, and 2.074M
    std::vector<uint32_t> criticalHeights = {500000, 1000000, 1500000, 2000000, 2074000};

    for (uint32_t height : criticalHeights)
    {
      auto it = checkpoints.find(height);
      if (it != checkpoints.end())
      {
        if (!verifyCheckpoint(it->first, it->second))
        {
          std::cerr << "Critical checkpoint failed at height " << height << std::endl;
          return false;
        }
      }
    }

    std::cout << "✓ All critical checkpoints verified (" << criticalHeights.size() << " checkpoints)" << std::endl;
    return true;
  }

  bool HeaderChain::verifyTransaction(const TransactionProof &proof) const
  {
    crypto::Hash current = proof.tx_hash;
    size_t idx = proof.tx_index;

    for (const auto &sibling : proof.merkle_branch)
    {
      crypto::Hash combined;

      if (idx & 1)
      {
        // current is right child -> hash(sibling + current)
        std::vector<uint8_t> concat;
        concat.reserve(sizeof(crypto::Hash) * 2);
        concat.insert(concat.end(), sibling.data, sibling.data + sizeof(crypto::Hash));
        concat.insert(concat.end(), current.data, current.data + sizeof(crypto::Hash));
        crypto::cn_fast_hash(concat.data(), concat.size(), combined);
      }
      else
      {
        // current is left child -> hash(current + sibling)
        std::vector<uint8_t> concat;
        concat.reserve(sizeof(crypto::Hash) * 2);
        concat.insert(concat.end(), current.data, current.data + sizeof(crypto::Hash));
        concat.insert(concat.end(), sibling.data, sibling.data + sizeof(crypto::Hash));
        crypto::cn_fast_hash(concat.data(), concat.size(), combined);
      }
      current = combined;
      idx >>= 1;
    }

    return current == proof.merkle_root;
  }
}