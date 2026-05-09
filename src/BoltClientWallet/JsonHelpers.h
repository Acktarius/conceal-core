// JsonHelpers.h — JSON parsing utilities and token info cache
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>

namespace cn
{
  class Currency;
}

std::string extractJsonString(const std::string &json, const std::string &key);
uint64_t extractJsonNumber(const std::string &json, const std::string &key);
std::string sidechainCall(const std::string &host, uint16_t port,
                          const std::string &method, const std::string &params);
std::string formatAmount(uint64_t amount, uint8_t decimals = 6);
std::string formatAmountWithDecimals(uint64_t amount, uint64_t tokenId,
                                     const std::string &sidechainHost, uint16_t sidechainPort);
void clearScreen();
std::string formatHash(const std::string &hash, size_t len = 16);
std::string getTimestamp();
std::string addressToHexPubKey(const std::string &input, class cn::Currency &currency);

// Token info cache for decimal lookups
struct TokenInfoCache
{
  std::string symbol;
  std::string name;
  uint8_t decimals = 6;
};

const std::unordered_map<uint64_t, TokenInfoCache> &getTokenCache();
void loadTokenCache(const std::string &host, uint16_t port);
TokenInfoCache getTokenInfo(uint64_t tokenId);
std::string getTokenSymbol(uint64_t tokenId);
uint8_t getTokenDecimals(uint64_t tokenId);