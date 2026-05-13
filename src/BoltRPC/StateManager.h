// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <vector>
#include "BoltCore/BoltCore.h"

namespace BoltRPC
{
  static const uint32_t MAGIC = 0x424F4C54;           // "BOLT"
  static const uint32_t VERSION = 3;                  // v3: adds encrypted key blob support
  static const uint32_t ENCRYPTED_MAGIC = 0x424F4C55; // "BOLU" — encrypted state file

  // Persists wallet scan state and keys between restarts.
  // Format: binary dump of OutputInfo array with header + optional encrypted key blob.
  class StateManager
  {
  public:
    explicit StateManager(const std::string &filePath);

    // Save outputs, scan height, and wallet keys. If password is non-empty, the key blob is encrypted.
    bool save(const std::vector<BoltCore::OutputInfo> &outputs,
              uint32_t lastScannedHeight,
              const std::string &viewKeyHex = "",
              const std::string &spendKeyHex = "",
              const std::string &password = "");

    // Load outputs and scan height
    bool load(std::vector<BoltCore::OutputInfo> &outputs,
              uint32_t &lastScannedHeight);

    // Load stored keys (for unlock without re-entering hex). Password required if file is encrypted.
    bool loadKeys(std::string &viewKeyHex, std::string &spendKeyHex,
                  const std::string &password = "");

    // Check if the state file requires a password to unlock keys
    bool isEncrypted() const;

    bool exists() const;

  private:
    std::string m_filePath;

    // Derives a 32-byte AES key from a password using SHA-256
    std::string deriveKey(const std::string &password) const;

    // Encrypts or decrypts data using AES-256 in a simple XOR-based stream
    // (For production, replace with proper AES-CBC or ChaCha20-Poly1305)
    std::string cryptData(const std::string &data, const std::string &key) const;
  };

} // namespace BoltRPC