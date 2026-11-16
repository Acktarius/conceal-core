// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "AiProviderRegistry.h"

#include "AiProviderValidation.h"

namespace cn {
namespace {

bool sameSignature(const crypto::Signature& left, const crypto::Signature& right) {
  return memcmp(left.data, right.data, sizeof(left.data)) == 0;
}

}

AiProviderRegistry::AiProviderRegistry(const boost::uuids::uuid& networkId, uint32_t maxEntries)
    : m_networkId(networkId), m_maxEntries(maxEntries == 0 ? 1 : maxEntries) {}

void AiProviderRegistry::setNetworkId(const boost::uuids::uuid& networkId) {
  m_networkId = networkId;
}

AiIngestResult AiProviderRegistry::accept(const AiProviderAdvertisement& ad, int64_t now, uint8_t source) {
  AiIngestResult result;
  if (!inspectAiAdvertisement(ad, m_networkId, now, result.reason)) {
    result.result = AiAcceptResult::invalid;
    return result;
  }

  std::vector<uint8_t> canonical;
  if (!verifyAiAdvertisementSignature(ad, canonical)) {
    result.result = AiAcceptResult::invalid;
    result.reason = "invalid signature";
    return result;
  }

  expire(now);

  std::map<crypto::PublicKey, Record, AiPublicKeyLess>::iterator it = m_records.find(ad.providerPublicKey);
  if (it == m_records.end()) {
    if (m_records.size() >= m_maxEntries) {
      result.result = AiAcceptResult::capacityRejected;
      result.reason = "registry full";
      return result;
    }

    Record record;
    record.advertisement = ad;
    record.canonical = canonical;
    record.observation.firstSeen = now;
    record.observation.lastSeen = now;
    record.observation.source = source;
    m_records.insert(std::make_pair(ad.providerPublicKey, record));
    result.result = AiAcceptResult::accepted;
    result.relay = true;
    result.reason = "accepted";
    return result;
  }

  Record& current = it->second;
  if (ad.sequence < current.advertisement.sequence) {
    result.result = AiAcceptResult::stale;
    result.reason = "stale sequence";
    return result;
  }
  if (ad.sequence == current.advertisement.sequence) {
    if (canonical == current.canonical && sameSignature(ad.signature, current.advertisement.signature)) {
      result.result = AiAcceptResult::duplicate;
      result.reason = "duplicate";
      return result;
    }
    result.result = AiAcceptResult::invalid;
    result.reason = "conflicting record at the same sequence";
    return result;
  }

  const int64_t firstSeen = current.observation.firstSeen;
  current.advertisement = ad;
  current.canonical = canonical;
  current.observation.firstSeen = firstSeen == 0 ? now : firstSeen;
  current.observation.lastSeen = now;
  current.observation.source = source;
  result.result = AiAcceptResult::accepted;
  result.relay = true;
  result.reason = "accepted";
  return result;
}

void AiProviderRegistry::expire(int64_t now) {
  for (std::map<crypto::PublicKey, Record, AiPublicKeyLess>::iterator it = m_records.begin(); it != m_records.end();) {
    if (it->second.advertisement.expiresAt <= now) {
      it = m_records.erase(it);
    } else {
      ++it;
    }
  }
}

bool AiProviderRegistry::offerMatches(const AiProviderAdvertisement& ad, const AiProviderQuery& query) {
  if (ad.models.empty()) {
    return query.modelId.empty() && query.billingUnit == 0 && query.minContextTokens == 0;
  }

  for (size_t i = 0; i < ad.models.size(); ++i) {
    const AiModelOffer& model = ad.models[i];
    if (!query.modelId.empty() && model.modelId != query.modelId) {
      continue;
    }
    if (query.billingUnit != 0 && model.billingUnit != query.billingUnit) {
      continue;
    }
    if (model.maxContextTokens < query.minContextTokens) {
      continue;
    }
    return true;
  }
  return false;
}

bool AiProviderRegistry::listingVisible(const AiProviderAdvertisement& ad, const AiProviderQuery& query, int64_t now) const {
  if (ad.expiresAt <= now) {
    return false;
  }
  if (query.status == 0) {
    if (ad.status == AI_STATUS_OFFLINE) {
      return false;
    }
  } else if (ad.status != query.status) {
    return false;
  }
  return offerMatches(ad, query);
}

AiProviderPage AiProviderRegistry::query(const AiProviderQuery& query, int64_t now) const {
  AiProviderPage page;
  uint16_t limit = query.limit == 0 ? AI_MAX_PAGE_RECORDS : query.limit;
  if (limit > AI_MAX_PAGE_RECORDS) {
    limit = AI_MAX_PAGE_RECORDS;
  }

  uint64_t index = 0;
  uint64_t emitted = 0;
  bool sawPastCursor = false;
  for (std::map<crypto::PublicKey, Record, AiPublicKeyLess>::const_iterator it = m_records.begin(); it != m_records.end();
       ++it) {
    if (!listingVisible(it->second.advertisement, query, now)) {
      continue;
    }
    if (index < query.cursor) {
      ++index;
      continue;
    }
    sawPastCursor = true;
    if (emitted < limit) {
      AiProviderListing listing;
      listing.advertisement = it->second.advertisement;
      listing.observation = it->second.observation;
      page.providers.push_back(listing);
      ++emitted;
      ++index;
      continue;
    }
    page.done = false;
    page.nextCursor = index;
    return page;
  }

  page.done = true;
  page.nextCursor = query.cursor + emitted;
  if (!sawPastCursor && query.cursor > 0 && emitted == 0) {
    page.nextCursor = query.cursor;
  }
  return page;
}

void AiProviderRegistry::directoryPage(uint64_t cursor, uint16_t limit, int64_t now, std::vector<AiProviderAdvertisement>& out,
                                       uint64_t& nextCursor, bool& done) const {
  out.clear();
  if (limit == 0 || limit > AI_MAX_PAGE_RECORDS) {
    limit = AI_MAX_PAGE_RECORDS;
  }

  uint64_t index = 0;
  for (std::map<crypto::PublicKey, Record, AiPublicKeyLess>::const_iterator it = m_records.begin(); it != m_records.end();
       ++it) {
    if (it->second.advertisement.expiresAt <= now) {
      continue;
    }
    if (index < cursor) {
      ++index;
      continue;
    }
    if (out.size() >= limit) {
      nextCursor = index;
      done = false;
      return;
    }
    out.push_back(it->second.advertisement);
    ++index;
  }

  nextCursor = index;
  done = true;
}

uint64_t AiProviderRegistry::listedCount(int64_t now) const {
  AiProviderQuery query;
  AiProviderPage page = this->query(query, now);
  uint64_t count = page.providers.size();
  uint32_t guard = 0;
  while (!page.done && guard < m_maxEntries) {
    ++guard;
    query.cursor = page.nextCursor;
    const uint64_t previous = query.cursor;
    page = this->query(query, now);
    count += page.providers.size();
    if (page.nextCursor == previous && !page.done) {
      break;
    }
  }
  return count;
}

}
