// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <cstring>
#include <map>

#include <boost/uuid/uuid.hpp>

#include "AiProviderTypes.h"
#include "crypto/crypto.h"

namespace cn {

struct AiPublicKeyLess {
  bool operator()(const crypto::PublicKey& left, const crypto::PublicKey& right) const {
    return memcmp(left.data, right.data, sizeof(left.data)) < 0;
  }
};

class AiProviderRegistry {
public:
  explicit AiProviderRegistry(const boost::uuids::uuid& networkId, uint32_t maxEntries = AI_MAX_REGISTRY_ENTRIES);

  void setNetworkId(const boost::uuids::uuid& networkId);
  const boost::uuids::uuid& networkId() const { return m_networkId; }

  // Signature is verified inside. Callers that rate-limit must do so before this call.
  AiIngestResult accept(const AiProviderAdvertisement& ad, int64_t now, uint8_t source);

  void expire(int64_t now);
  AiProviderPage query(const AiProviderQuery& query, int64_t now) const;
  void directoryPage(uint64_t cursor, uint16_t limit, int64_t now, std::vector<AiProviderAdvertisement>& out,
                     uint64_t& nextCursor, bool& done) const;

  uint64_t listedCount(int64_t now) const;
  size_t size() const { return m_records.size(); }

private:
  struct Record {
    AiProviderAdvertisement advertisement;
    std::vector<uint8_t> canonical;
    AiProviderObservation observation;
  };

  bool listingVisible(const AiProviderAdvertisement& ad, const AiProviderQuery& query, int64_t now) const;
  static bool offerMatches(const AiProviderAdvertisement& ad, const AiProviderQuery& query);

  boost::uuids::uuid m_networkId;
  uint32_t m_maxEntries;
  std::map<crypto::PublicKey, Record, AiPublicKeyLess> m_records;
};

}
