// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "StateManager.h"
#include <fstream>
#include <iostream>
#include "Common/StringTools.h"

namespace BoltRPC
{

  StateManager::StateManager(const std::string &filePath)
      : m_filePath(filePath) {}

  bool StateManager::exists() const
  {
    std::ifstream f(m_filePath, std::ios::binary);
    return f.good();
  }

  bool StateManager::save(const std::vector<BoltCore::OutputInfo> &outputs,
                          uint32_t lastScannedHeight)
  {
    std::ofstream f(m_filePath, std::ios::binary | std::ios::trunc);
    if (!f)
      return false;

    // Header
    f.write(reinterpret_cast<const char *>(&MAGIC), sizeof(MAGIC));
    f.write(reinterpret_cast<const char *>(&VERSION), sizeof(VERSION));
    f.write(reinterpret_cast<const char *>(&lastScannedHeight), sizeof(lastScannedHeight));

    uint32_t count = static_cast<uint32_t>(outputs.size());
    f.write(reinterpret_cast<const char *>(&count), sizeof(count));

    for (const auto &out : outputs)
    {
      // Fixed-size fields
      f.write(reinterpret_cast<const char *>(&out.blockHeight), sizeof(out.blockHeight));
      f.write(reinterpret_cast<const char *>(&out.txHash), sizeof(out.txHash));
      f.write(reinterpret_cast<const char *>(&out.outputIndex), sizeof(out.outputIndex));
      f.write(reinterpret_cast<const char *>(&out.globalOutputIndex), sizeof(out.globalOutputIndex));
      f.write(reinterpret_cast<const char *>(&out.amount), sizeof(out.amount));
      f.write(reinterpret_cast<const char *>(&out.outputKey), sizeof(out.outputKey));
      f.write(reinterpret_cast<const char *>(&out.txPublicKey), sizeof(out.txPublicKey));
      f.write(reinterpret_cast<const char *>(&out.keyImage), sizeof(out.keyImage));
      f.write(reinterpret_cast<const char *>(&out.spent), sizeof(out.spent));
      f.write(reinterpret_cast<const char *>(&out.isDeposit), sizeof(out.isDeposit));
      f.write(reinterpret_cast<const char *>(&out.term), sizeof(out.term));

      // Variable-length subAddress string
      uint32_t addrLen = static_cast<uint32_t>(out.subAddress.size());
      f.write(reinterpret_cast<const char *>(&addrLen), sizeof(addrLen));
      f.write(out.subAddress.data(), addrLen);
    }

    return f.good();
  }

  bool StateManager::load(std::vector<BoltCore::OutputInfo> &outputs,
                          uint32_t &lastScannedHeight)
  {
    std::ifstream f(m_filePath, std::ios::binary);
    if (!f)
      return false;

    uint32_t magic, version;
    f.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    f.read(reinterpret_cast<char *>(&version), sizeof(version));

    if (magic != MAGIC || version != VERSION)
    {
      std::cerr << "StateManager: invalid or unsupported state file" << std::endl;
      return false;
    }

    f.read(reinterpret_cast<char *>(&lastScannedHeight), sizeof(lastScannedHeight));

    uint32_t count;
    f.read(reinterpret_cast<char *>(&count), sizeof(count));
    outputs.clear();
    outputs.reserve(count);

    for (uint32_t i = 0; i < count; ++i)
    {
      BoltCore::OutputInfo out;
      f.read(reinterpret_cast<char *>(&out.blockHeight), sizeof(out.blockHeight));
      f.read(reinterpret_cast<char *>(&out.txHash), sizeof(out.txHash));
      f.read(reinterpret_cast<char *>(&out.outputIndex), sizeof(out.outputIndex));
      f.read(reinterpret_cast<char *>(&out.globalOutputIndex), sizeof(out.globalOutputIndex));
      f.read(reinterpret_cast<char *>(&out.amount), sizeof(out.amount));
      f.read(reinterpret_cast<char *>(&out.outputKey), sizeof(out.outputKey));
      f.read(reinterpret_cast<char *>(&out.txPublicKey), sizeof(out.txPublicKey));
      f.read(reinterpret_cast<char *>(&out.keyImage), sizeof(out.keyImage));
      f.read(reinterpret_cast<char *>(&out.spent), sizeof(out.spent));
      f.read(reinterpret_cast<char *>(&out.isDeposit), sizeof(out.isDeposit));
      f.read(reinterpret_cast<char *>(&out.term), sizeof(out.term));

      uint32_t addrLen;
      f.read(reinterpret_cast<char *>(&addrLen), sizeof(addrLen));
      out.subAddress.resize(addrLen);
      f.read(&out.subAddress[0], addrLen);

      if (!f)
        return false;
      outputs.push_back(std::move(out));
    }

    return true;
  }

} // namespace BoltRPC