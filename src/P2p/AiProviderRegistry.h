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

  // Classifies without verifying signatures. A same-sequence conflict is rejected here:
  // the stored record was already verified, so another body at that sequence cannot be accepted.
  AiRecordAssessment assess(const AiProviderAdvertisement& ad, int64_t now) const;
  bool verifyRecord(const AiProviderAdvertisement& ad, std::vector<uint8_t>& canonical, std::string& reason) const;
  AiIngestResult acceptVerified(const AiProviderAdvertisement& ad, const std::vector<uint8_t>& canonical, int64_t now,
                                uint8_t source);
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
  uint32_t peerSlotLimit() const;
  uint32_t peerRecordCount() const;
  bool evictClosestPeerRecord(int64_t now);

  boost::uuids::uuid m_networkId;
  uint32_t m_maxEntries;
  std::map<crypto::PublicKey, Record, AiPublicKeyLess> m_records;
};

}
