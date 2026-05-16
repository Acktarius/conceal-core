// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SPVWallet.h"
#include "Common/JsonValue.h"
#include "Common/StringTools.h"
#include <iostream>
#include <chrono>
#include <unistd.h>

using namespace common;

namespace SPV
{
  SPVWallet::SPVWallet(const std::string &remoteHost, uint16_t remotePort)
      : m_remoteHost(remoteHost), m_remotePort(remotePort), m_node(NULL)
  {
    m_node = new RemoteNode(remoteHost, remotePort);
  }

  SPVWallet::~SPVWallet()
  {
    delete m_node;
  }

  bool SPVWallet::initFromBootstrap(const std::string &headersFile)
  {
    if (!m_chain.load(headersFile))
    {
      std::cerr << "Failed to load headers from " << headersFile << std::endl;
      return false;
    }
    std::cout << "Loaded " << m_chain.getHeight() + 1 << " headers from " << headersFile << std::endl;
    return true;
  }

  bool SPVWallet::initFromDownload(const std::string &url)
  {
    std::cerr << "Download bootstrap from " << url << " - not yet implemented" << std::endl;
    return false;
  }

  bool SPVWallet::initFromSync()
  {
    return syncNewHeaders();
  }

  bool SPVWallet::sendJsonRpc(const std::string &method, const std::string &params, std::string &response)
  {
    if (!m_node)
      return false;

    std::string body = R"({"jsonrpc":"2.0","method":")" + method + R"(","params":)" + params + R"(,"id":1})";
    HttpResult resp = m_node->post("/json_rpc", body);

    if (!resp.success || resp.statusCode != 200)
      return false;

    response = resp.body;
    return true;
  }

  uint32_t SPVWallet::getNodeHeight()
  {
    std::string response;
    if (!sendJsonRpc("getblockcount", "{}", response))
      return 0;

    try
    {
      JsonValue val = JsonValue::fromString(response);
      if (val.contains("result") && val("result").contains("count"))
        return static_cast<uint32_t>(val("result")("count").getInteger());
    }
    catch (...)
    {
      return 0;
    }
    return 0;
  }

  bool SPVWallet::getBlockHashByHeight(uint32_t height, crypto::Hash &hash)
  {
    std::string params = R"([)" + std::to_string(height) + R"(])";
    std::string response;
    if (!sendJsonRpc("getblockhash", params, response))
      return false;

    try
    {
      JsonValue val = JsonValue::fromString(response);
      std::string hash_str;
      if (val.contains("result"))
        hash_str = val("result").getString();
      else
        hash_str = val.getString();
      return podFromHex(hash_str, hash);
    }
    catch (...)
    {
      return false;
    }
  }

  bool SPVWallet::getBlockHeaderByHeight(uint32_t height, BlockHeader &header)
  {
    std::string params = R"({"height":)" + std::to_string(height) + R"(})";
    std::string response;
    if (!sendJsonRpc("getblockheaderbyheight", params, response))
      return false;

    try
    {
      JsonValue val = JsonValue::fromString(response);
      if (!val.contains("result") || !val("result").contains("block_header"))
        return false;

      JsonValue &bh = val("result")("block_header");
      header.height = height;

      std::string hash_str = bh("hash").getString();
      std::string prev_hash_str = bh("prev_hash").getString();

      if (!podFromHex(hash_str, header.hash))
        return false;
      if (!podFromHex(prev_hash_str, header.prev_hash))
        return false;

      header.timestamp = bh("timestamp").getInteger();
      header.nonce = static_cast<uint32_t>(bh("nonce").getInteger());
      header.difficulty = bh("difficulty").getInteger();

      if (bh.contains("merkle_root"))
      {
        std::string merkle_root_str = bh("merkle_root").getString();
        podFromHex(merkle_root_str, header.merkle_root);
      }
      else
      {
        header.merkle_root = crypto::Hash();
      }

      header.major_version = static_cast<uint8_t>(bh("major_version").getInteger());
      header.minor_version = static_cast<uint8_t>(bh("minor_version").getInteger());

      return true;
    }
    catch (const std::exception &e)
    {
      std::cerr << "Error getting block header: " << e.what() << std::endl;
      return false;
    }
  }

  bool SPVWallet::syncNewHeaders()
  {
    uint32_t localHeight = m_chain.getHeight();
    uint32_t remoteHeight = getNodeHeight();

    if (remoteHeight <= localHeight)
      return true;

    std::cout << "Syncing headers from " << localHeight + 1 << " to " << remoteHeight << std::endl;

    for (uint32_t h = localHeight + 1; h <= remoteHeight; ++h)
    {
      BlockHeader header;
      if (!getBlockHeaderByHeight(h, header))
        return false;

      if (!verifyPoW(header))
        return false;

      if (!m_chain.addHeader(header))
        return false;

      if (h % 1000 == 0)
        std::cout << "Synced to height " << h << std::endl;
    }

    return true;
  }

  bool SPVWallet::getTransactionProof(const crypto::Hash &tx_hash, TransactionProof &proof)
  {
    std::string params = R"({"tx_hash":")" + podToHex(tx_hash) + R"("})";
    std::string response;
    if (!sendJsonRpc("get_merkle_proof", params, response))
      return false;

    try
    {
      JsonValue val = JsonValue::fromString(response);
      if (!val.contains("result"))
        return false;

      JsonValue &res = val("result");
      proof.tx_hash = tx_hash;
      proof.block_height = static_cast<uint32_t>(res("block_height").getInteger());

      std::string block_hash_str = res("block_hash").getString();
      std::string merkle_root_str = res("merkle_root").getString();

      if (!podFromHex(block_hash_str, proof.block_hash))
        return false;
      if (!podFromHex(merkle_root_str, proof.merkle_root))
        return false;

      proof.tx_index = static_cast<uint32_t>(res("tx_index").getInteger());

      proof.merkle_branch.clear();
      if (res.contains("merkle_branch"))
      {
        JsonValue &branch = res("merkle_branch");
        for (size_t i = 0; i < branch.size(); ++i)
        {
          std::string branch_str = branch[i].getString();
          crypto::Hash branch_hash;
          if (!podFromHex(branch_str, branch_hash))
            return false;
          proof.merkle_branch.push_back(branch_hash);
        }
      }

      proof.verified = true;
      return true;
    }
    catch (const std::exception &e)
    {
      std::cerr << "Error getting transaction proof: " << e.what() << std::endl;
      return false;
    }
  }

  bool SPVWallet::verifyTransaction(const TransactionProof &proof) const
  {
    return m_chain.verifyTransaction(proof);
  }

  bool SPVWallet::verifyPoW(const BlockHeader &header)
  {
    return header.difficulty > 0 && header.hash != cn::NULL_HASH;
  }

  bool SPVWallet::scanOutputs(const crypto::PublicKey &viewKey,
                              uint32_t fromHeight,
                              uint32_t toHeight,
                              std::vector<std::pair<crypto::Hash, uint64_t>> &outputs)
  {
    std::string params = R"({"view_pub_key":")" + podToHex(viewKey) + R"(")"
                                                                      R"(,"from_height":)" +
                         std::to_string(fromHeight) +
                         R"(,"to_height":)" + std::to_string(toHeight) + R"(})";

    std::string response;
    if (!sendJsonRpc("get_outputs_for_address", params, response))
      return false;

    try
    {
      JsonValue val = JsonValue::fromString(response);
      if (!val.contains("result"))
        return false;

      JsonValue &outs = val("result")("outputs");
      for (size_t i = 0; i < outs.size(); ++i)
      {
        std::string tx_hash_str = outs[i]("tx_hash").getString();
        crypto::Hash tx_hash;
        if (!podFromHex(tx_hash_str, tx_hash))
          continue;

        uint64_t amount = outs[i]("amount").getInteger();
        outputs.push_back(std::make_pair(tx_hash, amount));
      }
      return true;
    }
    catch (const std::exception &e)
    {
      std::cerr << "Error scanning outputs: " << e.what() << std::endl;
      return false;
    }
  }
}