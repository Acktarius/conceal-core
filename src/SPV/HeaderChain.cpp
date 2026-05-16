// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "HeaderChain.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>

namespace SPV
{
  HeaderChain::HeaderChain() : m_height(0) {}

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

    // Read genesis hash (skip, we don't need it)
    crypto::Hash genesisHash;
    file.read(reinterpret_cast<char *>(&genesisHash), sizeof(genesisHash));

    m_headers.clear();
    m_headers.reserve(count);
    m_height = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
      BlockHeader h;

      // Read stored height but don't use it (use index i instead)
      uint32_t stored_height;
      file.read(reinterpret_cast<char *>(&stored_height), sizeof(stored_height));
      h.height = i; // Use index, ignore stored height

      file.read(reinterpret_cast<char *>(&h.major_version), sizeof(h.major_version));
      file.read(reinterpret_cast<char *>(&h.minor_version), sizeof(h.minor_version));
      file.read(reinterpret_cast<char *>(&h.timestamp), sizeof(h.timestamp));
      file.read(reinterpret_cast<char *>(&h.nonce), sizeof(h.nonce));

      uint64_t cumulativeDifficulty;
      file.read(reinterpret_cast<char *>(&cumulativeDifficulty), sizeof(cumulativeDifficulty));
      h.difficulty = cumulativeDifficulty;

      uint64_t alreadyGeneratedCoins;
      file.read(reinterpret_cast<char *>(&alreadyGeneratedCoins), sizeof(alreadyGeneratedCoins));

      file.read(reinterpret_cast<char *>(&h.prev_hash), sizeof(h.prev_hash));
      file.read(reinterpret_cast<char *>(&h.hash), sizeof(h.hash));

      if (!file.good())
      {
        std::cerr << "Failed to read header at index " << i << std::endl;
        return false;
      }

      h.merkle_root = crypto::Hash();

      m_headers.push_back(h);
      m_height = i; // Set to index, not stored height

      if (i % 100000 == 0 && i > 0)
      {
        std::cout << "Loaded " << i << " headers..." << std::endl;
      }
    }

    file.close();

    std::cout << "Loaded " << m_headers.size() << " headers, synced to height " << m_height << std::endl;
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