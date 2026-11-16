// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <string>

#include <boost/uuid/uuid.hpp>

#include "AiProviderTypes.h"

namespace cn {

struct AiRateWindow {
  int64_t windowStart;
  uint32_t count;

  AiRateWindow() : windowStart(0), count(0) {}

  bool allow(int64_t now, uint32_t limit, int64_t windowSeconds);
  bool allowMany(int64_t now, uint32_t amount, uint32_t limit, int64_t windowSeconds);
};

// Cheap structural checks. Does not verify the signature.
bool inspectAiAdvertisement(const AiProviderAdvertisement& ad, const boost::uuids::uuid& expectedNetwork, int64_t now,
                            std::string& reason);

}
