// JsonHelpers.h — JSON parsing utilities
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <cstdint>

namespace cn
{
  class Currency;
}

std::string extractJsonString(const std::string &json, const std::string &key);
uint64_t extractJsonNumber(const std::string &json, const std::string &key);
std::string sidechainCall(const std::string &host, uint16_t port,
                          const std::string &method, const std::string &params);
std::string formatAmount(uint64_t amount, uint8_t decimals = 6);
void clearScreen();
std::string formatHash(const std::string &hash, size_t len = 16);
std::string getTimestamp();
std::string addressToHexPubKey(const std::string &input, cn::Currency &currency);