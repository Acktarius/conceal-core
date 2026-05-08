// JsonHelpers.cpp — JSON parsing utilities
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "JsonHelpers.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "Rpc/HttpClient.h"
#include <System/Dispatcher.h>
#include <sstream>
#include <ctime>

std::string extractJsonString(const std::string &json, const std::string &key)
{
  std::string search = "\"" + key + "\":\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos)
  {
    search = "\"" + key + "\":";
    pos = json.find(search);
    if (pos == std::string::npos)
      return "";
    pos += search.length();
    size_t end = json.find_first_of(",}]", pos);
    if (end == std::string::npos)
      return "";
    std::string val = json.substr(pos, end - pos);
    if (!val.empty() && val.front() == '"' && val.back() == '"')
      val = val.substr(1, val.size() - 2);
    return val;
  }
  pos += search.length();
  size_t end = json.find("\"", pos);
  if (end == std::string::npos)
    return "";
  return json.substr(pos, end - pos);
}

uint64_t extractJsonNumber(const std::string &json, const std::string &key)
{
  std::string search = "\"" + key + "\":";
  size_t pos = json.find(search);
  if (pos == std::string::npos)
    return 0;
  pos += search.length();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    pos++;
  std::string num;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
  {
    num += json[pos];
    pos++;
  }
  if (num.empty())
    return 0;
  try
  {
    return std::stoull(num);
  }
  catch (...)
  {
    return 0;
  }
}

std::string sidechainCall(const std::string &host, uint16_t port,
                          const std::string &method, const std::string &params)
{
  platform_system::Dispatcher dispatcher;
  cn::HttpClient client(dispatcher, host, port);

  cn::HttpRequest req;
  cn::HttpResponse res;

  std::string body = R"({"jsonrpc":"2.0","id":1,"method":")" + method +
                     R"(","params":)" + params + "}";

  req.setUrl("/json_rpc");
  req.setBody(body);
  req.addHeader("Content-Type", "application/json");

  try
  {
    client.request(req, res);
    return res.getBody();
  }
  catch (...)
  {
    return "{}";
  }
}

std::string formatAmount(uint64_t amount, uint8_t decimals)
{
  if (decimals == 0)
    return std::to_string(amount);
  std::string s = std::to_string(amount);
  while (s.length() <= decimals)
    s = "0" + s;
  size_t dotPos = s.length() - decimals;
  std::string result = s.substr(0, dotPos) + "." + s.substr(dotPos);
  while (result.back() == '0' && result[result.size() - 2] != '.')
    result.pop_back();
  return result;
}

void clearScreen() { std::cout << "\033[2J\033[1;1H" << std::flush; }

std::string formatHash(const std::string &hash, size_t len)
{
  if (hash.size() <= len)
    return hash;
  return hash.substr(0, len) + "...";
}

std::string getTimestamp()
{
  auto t = std::time(nullptr);
  std::string ts = std::ctime(&t);
  if (!ts.empty() && ts.back() == '\n')
    ts.pop_back();
  return ts;
}

std::string addressToHexPubKey(const std::string &input, cn::Currency &currency)
{
  if (input.size() == 64 && input.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
    return input;

  try
  {
    cn::AccountPublicAddress addr;
    if (currency.parseAccountAddressString(input, addr))
      return common::podToHex(addr.spendPublicKey);
  }
  catch (...)
  {
  }

  return input;
}