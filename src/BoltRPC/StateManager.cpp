// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "StateManager.h"
#include <fstream>
#include <iostream>
#include "Common/StringTools.h"
#include "crypto/crypto.h"

namespace BoltRPC
{

  StateManager::StateManager(const std::string &filePath)
      : m_filePath(filePath) {}

  bool StateManager::exists() const
  {
    std::ifstream f(m_filePath, std::ios::binary);
    return f.good();
  }

  bool StateManager::isEncrypted() const
  {
    std::ifstream f(m_filePath, std::ios::binary);
    if (!f)
      return false;

    uint32_t magic;
    f.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    return magic == ENCRYPTED_MAGIC;
  }

  std::string StateManager::deriveKey(const std::string &password) const
  {
    // Derive a 32-byte key from the password using the slow hash
    crypto::chacha8_key key;
    memset(&key, 0, sizeof(key));

    crypto::Hash pwd_hash;
    crypto::cn_fast_hash(password.data(), password.size(), pwd_hash);
    memcpy(&key, &pwd_hash, sizeof(key));

    return std::string(reinterpret_cast<const char *>(&key), sizeof(key));
  }

  std::string StateManager::cryptData(const std::string &data, const std::string &key) const
  {
    // Use ChaCha8 with a fixed IV of zeros. The key is derived from the password.
    // The IV doesn't need to be random because the key is unique per password.
    crypto::chacha8_iv iv;
    memset(&iv, 0, sizeof(iv));

    std::string result(data.size(), '\0');
    crypto::chacha8(data.data(), data.size(),
                    reinterpret_cast<const crypto::chacha8_key &>(*key.data()),
                    iv, &result[0]);

    return result;
  }

  bool StateManager::save(const std::vector<BoltCore::OutputInfo> &outputs,
                          uint32_t lastScannedHeight,
                          const std::string &viewKeyHex,
                          const std::string &spendKeyHex,
                          const std::string &password)
  {
    std::ofstream f(m_filePath, std::ios::binary | std::ios::trunc);
    if (!f)
      return false;

    bool encrypt = !password.empty();

    // Header: magic, version, scanned height
    uint32_t magic = encrypt ? ENCRYPTED_MAGIC : MAGIC;
    f.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const char *>(&VERSION), sizeof(VERSION));
    f.write(reinterpret_cast<const char *>(&lastScannedHeight), sizeof(lastScannedHeight));

    // Output count + output data (always unencrypted)
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

    // Build key blob
    std::string keyBlob;
    uint32_t vkLen = static_cast<uint32_t>(viewKeyHex.size());
    keyBlob.append(reinterpret_cast<const char *>(&vkLen), sizeof(vkLen));
    if (vkLen > 0)
      keyBlob.append(viewKeyHex);

    uint32_t skLen = static_cast<uint32_t>(spendKeyHex.size());
    keyBlob.append(reinterpret_cast<const char *>(&skLen), sizeof(skLen));
    if (skLen > 0)
      keyBlob.append(spendKeyHex);

    // Encrypt the key blob if password is provided
    if (encrypt)
    {
      std::string derivedKey = deriveKey(password);
      keyBlob = cryptData(keyBlob, derivedKey);
    }

    // Write key blob length and data
    uint32_t blobLen = static_cast<uint32_t>(keyBlob.size());
    f.write(reinterpret_cast<const char *>(&blobLen), sizeof(blobLen));
    f.write(keyBlob.data(), blobLen);

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

    // Accept v1, v2, and v3 formats
    if ((magic != MAGIC && magic != ENCRYPTED_MAGIC) || version < 1 || version > VERSION)
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

    // The key blob is at the end. We don't parse it here.
    // loadKeys() handles that separately.
    return true;
  }

  bool StateManager::loadKeys(std::string &viewKeyHex, std::string &spendKeyHex,
                              const std::string &password)
  {
    std::ifstream f(m_filePath, std::ios::binary);
    if (!f)
      return false;

    uint32_t magic, version;
    f.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    f.read(reinterpret_cast<char *>(&version), sizeof(version));

    if ((magic != MAGIC && magic != ENCRYPTED_MAGIC) || version < 1 || version > VERSION)
      return false;

    bool encrypted = (magic == ENCRYPTED_MAGIC);

    // Skip past the header and all output data to reach the key blob
    uint32_t lastScannedHeight;
    f.read(reinterpret_cast<char *>(&lastScannedHeight), sizeof(lastScannedHeight));

    uint32_t count;
    f.read(reinterpret_cast<char *>(&count), sizeof(count));

    // Seek past all output entries
    for (uint32_t i = 0; i < count; ++i)
    {
      f.seekg(4 + 32 + 4 + 4 + 8 + 32 + 32 + 32 + 1 + 1 + 4, std::ios::cur);
      uint32_t addrLen;
      f.read(reinterpret_cast<char *>(&addrLen), sizeof(addrLen));
      f.seekg(addrLen, std::ios::cur);
      if (!f)
        return false;
    }

    // Read key blob length
    uint32_t blobLen;
    f.read(reinterpret_cast<char *>(&blobLen), sizeof(blobLen));
    if (blobLen == 0)
      return false;

    // Read key blob
    std::string keyBlob(blobLen, '\0');
    f.read(&keyBlob[0], blobLen);

    // Decrypt if needed
    if (encrypted)
    {
      if (password.empty())
      {
        std::cerr << "StateManager: encrypted state file requires a password" << std::endl;
        return false;
      }
      std::string derivedKey = deriveKey(password);
      keyBlob = cryptData(keyBlob, derivedKey);
    }

    // Parse key blob
    const char *ptr = keyBlob.data();
    const char *end = ptr + keyBlob.size();

    if (ptr + sizeof(uint32_t) > end)
      return false;
    uint32_t vkLen;
    memcpy(&vkLen, ptr, sizeof(vkLen));
    ptr += sizeof(vkLen);

    if (vkLen > 0)
    {
      if (ptr + vkLen > end)
        return false;
      viewKeyHex.assign(ptr, vkLen);
      ptr += vkLen;
    }

    if (ptr + sizeof(uint32_t) > end)
      return !viewKeyHex.empty();
    uint32_t skLen;
    memcpy(&skLen, ptr, sizeof(skLen));
    ptr += sizeof(skLen);

    if (skLen > 0)
    {
      if (ptr + skLen > end)
        return !viewKeyHex.empty();
      spendKeyHex.assign(ptr, skLen);
      ptr += skLen;
    }

    return !viewKeyHex.empty();
  }

} // namespace BoltRPC