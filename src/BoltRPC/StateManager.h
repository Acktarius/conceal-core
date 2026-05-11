// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <vector>
#include "BoltCore/BoltCore.h"

namespace BoltRPC
{
  static const uint32_t MAGIC = 0x424F4C54; // "BOLT"
  static const uint32_t VERSION = 2;        // Bumped: v2 adds key storage for unlock

  // Persists wallet scan state and keys between restarts.
  // Format: binary dump of OutputInfo array with header + optional key blob.
  class StateManager
  {
  public:
    explicit StateManager(const std::string &filePath);

    // Save outputs, scan height, and wallet keys
    bool save(const std::vector<BoltCore::OutputInfo> &outputs,
              uint32_t lastScannedHeight,
              const std::string &viewKeyHex = "",
              const std::string &spendKeyHex = "");

    // Load outputs and scan height
    bool load(std::vector<BoltCore::OutputInfo> &outputs,
              uint32_t &lastScannedHeight);

    // Load stored keys (for unlock without re-entering hex)
    bool loadKeys(std::string &viewKeyHex, std::string &spendKeyHex);

    bool exists() const;

  private:
    std::string m_filePath;
  };

} // namespace BoltRPC