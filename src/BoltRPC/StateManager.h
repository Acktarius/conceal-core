// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once
#include <string>
#include <vector>
#include "BoltCore/BoltCore.h"

namespace BoltRPC
{
  static const uint32_t MAGIC = 0x424F4C54; // "BOLT"
  static const uint32_t VERSION = 1;

  // Persists wallet scan state between restarts so --skip-scan works.
  // Format: simple binary dump of OutputInfo array with a header.
  class StateManager
  {
  public:
    explicit StateManager(const std::string &filePath);

    bool save(const std::vector<BoltCore::OutputInfo> &outputs,
              uint32_t lastScannedHeight);

    bool load(std::vector<BoltCore::OutputInfo> &outputs,
              uint32_t &lastScannedHeight);

    bool exists() const;

  private:
    std::string m_filePath;
  };

} // namespace BoltRPC