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
                          uint32_t lastScannedHeight,
                          const std::string &viewKeyHex,
                          const std::string &spendKeyHex)
  {
    std::ofstream f(m_filePath, std::ios::binary | std::ios::trunc);
    if (!f)
      return false;

    // Header: magic, version, scanned height
    f.write(reinterpret_cast<const char *>(&MAGIC), sizeof(MAGIC));
    f.write(reinterpret_cast<const char *>(&VERSION), sizeof(VERSION));
    f.write(reinterpret_cast<const char *>(&lastScannedHeight), sizeof(lastScannedHeight));

    // Output count + output data
    uint32_t count = static_cast<uint32_t>(outputs.size());
    f.write(reinterpret_cast<const char *>(&count), sizeof(count));

    for (const auto &out : outputs)
    {
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

      uint32_t addrLen = static_cast<uint32_t>(out.subAddress.size());
      f.write(reinterpret_cast<const char *>(&addrLen), sizeof(addrLen));
      f.write(out.subAddress.data(), addrLen);
    }

    // Key blob (v2+): view key length + view key hex + spend key length + spend key hex
    uint32_t vkLen = static_cast<uint32_t>(viewKeyHex.size());
    f.write(reinterpret_cast<const char *>(&vkLen), sizeof(vkLen));
    if (vkLen > 0)
      f.write(viewKeyHex.data(), vkLen);

    uint32_t skLen = static_cast<uint32_t>(spendKeyHex.size());
    f.write(reinterpret_cast<const char *>(&skLen), sizeof(skLen));
    if (skLen > 0)
      f.write(spendKeyHex.data(), skLen);

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

    // Accept both v1 and v2 format (v2 just has extra key data at the end)
    if (magic != MAGIC || version < 1 || version > VERSION)
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

  bool StateManager::loadKeys(std::string &viewKeyHex, std::string &spendKeyHex)
  {
    std::ifstream f(m_filePath, std::ios::binary);
    if (!f)
      return false;

    uint32_t magic, version;
    f.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    f.read(reinterpret_cast<char *>(&version), sizeof(version));

    if (magic != MAGIC || version != VERSION)
      return false;

    // Skip past the header and all output data to reach the key blob
    uint32_t lastScannedHeight;
    f.read(reinterpret_cast<char *>(&lastScannedHeight), sizeof(lastScannedHeight));

    uint32_t count;
    f.read(reinterpret_cast<char *>(&count), sizeof(count));

    // Calculate size of one output entry to seek past
    for (uint32_t i = 0; i < count; ++i)
    {
      // Skip fixed fields
      f.seekg(4 + 32 + 4 + 4 + 8 + 32 + 32 + 32 + 1 + 1 + 4, std::ios::cur);
      // Skip variable-length subAddress
      uint32_t addrLen;
      f.read(reinterpret_cast<char *>(&addrLen), sizeof(addrLen));
      f.seekg(addrLen, std::ios::cur);
      if (!f)
        return false;
    }

    // Read key blob
    uint32_t vkLen;
    f.read(reinterpret_cast<char *>(&vkLen), sizeof(vkLen));
    if (vkLen > 0)
    {
      viewKeyHex.resize(vkLen);
      f.read(&viewKeyHex[0], vkLen);
    }

    uint32_t skLen;
    f.read(reinterpret_cast<char *>(&skLen), sizeof(skLen));
    if (skLen > 0)
    {
      spendKeyHex.resize(skLen);
      f.read(&spendKeyHex[0], skLen);
    }

    return !viewKeyHex.empty();
  }

} // namespace BoltRPC