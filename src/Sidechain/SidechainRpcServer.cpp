// SidechainRpcServer implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainRpcServer.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include <sstream>

namespace Sidechain
{
  SidechainRpcServer::SidechainRpcServer(logging::ILogger &logger,
                                         SidechainStorage &storage,
                                         SidechainValidator &validator)
      : m_logger(logger, "SidechainRPC"),
        m_storage(storage),
        m_validator(validator)
  {
  }

  void SidechainRpcServer::start(const std::string &bindIp, uint16_t bindPort, size_t threadCount)
  {
    m_httpServer.reset(new BoltHttp::Server(threadCount));
    m_httpServer->onRequest([this](const BoltHttp::Request &req, BoltHttp::Response &resp)
                            {
        if (req.url == "/json_rpc" && req.method == "POST")
        {
            std::string body(req.body.begin(), req.body.end());
            resp.setBody(handleJsonRpc(body));
        }
        else
        {
            resp.status = 404;
            resp.setBody("Not found");
        } });
    m_httpServer->start(bindIp, bindPort);
    m_logger(logging::INFO) << "Sidechain RPC server started on " << bindIp << ":" << bindPort;
  }

  void SidechainRpcServer::stop()
  {
    if (m_httpServer)
      m_httpServer->stop();
    m_logger(logging::INFO) << "Sidechain RPC server stopped";
  }

  std::string SidechainRpcServer::handleJsonRpc(const std::string &body)
  {
    try
    {
      std::stringstream stream(body);
      common::JsonValue request;
      stream >> request;

      std::string method = request("method").getString();
      common::JsonValue params = request("params");

      std::string id = "null";
      const common::JsonValue &idVal = request("id");
      if (idVal.isInteger())
        id = std::to_string(idVal.getInteger());
      else if (idVal.isString())
        id = idVal.getString();

      std::stringstream paramsStream;
      paramsStream << params;
      m_logger(logging::INFO) << "Params: " << paramsStream.str();

      std::string result;
      if (method == "getBalance")
        result = methodGetBalance(params);
      else if (method == "getTokenBalance")
        result = methodGetTokenBalance(params);
      else if (method == "getTokens")
        result = methodGetTokens(params);
      else if (method == "transfer")
        result = methodTransfer(params);
      else if (method == "createToken")
        result = methodCreateToken(params);
      else if (method == "mintToken")
        result = methodMintToken(params);
      else if (method == "burnToken")
        result = methodBurnToken(params);
      else if (method == "getStatus")
        result = methodGetStatus(params);
      else if (method == "getPendingTransactions")
        result = methodGetPendingTransactions(params);
      else if (method == "getValidators")
        result = methodGetValidators(params);
      else if (method == "getTransactions")
        result = methodGetTransactions(params);
      else if (method == "faucet")
        result = methodFaucet(params);
      else
      {
        std::string error = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32601,\"message\":\"Method not found\"},\"id\":" + id + "}";
        return error;
      }

      std::string response = "{\"jsonrpc\":\"2.0\",\"result\":" + result + ",\"id\":" + id + "}";
      return response;
    }
    catch (const std::exception &e)
    {
      std::string error = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"" + std::string(e.what()) + "\"},\"id\":null}";
      return error;
    }
  }

  std::string SidechainRpcServer::methodGetBalance(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);
    uint64_t balance = 0;
    m_storage.getBalance(pubKey, 0, balance);
    return std::to_string(balance);
  }

  std::string SidechainRpcServer::methodGetTokenBalance(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    uint64_t tokenId = static_cast<uint64_t>(params("tokenId").getInteger());
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);
    uint64_t balance = 0;
    m_storage.getBalance(pubKey, tokenId, balance);
    return std::to_string(balance);
  }

  std::string SidechainRpcServer::methodGetTokens(const common::JsonValue &)
  {
    auto tokens = m_storage.getAllTokens();
    std::string result = "[";
    for (size_t i = 0; i < tokens.size(); ++i)
    {
      result += R"({"id":)" + std::to_string(tokens[i].id) +
                R"(,"name":")" + tokens[i].name +
                R"(","symbol":")" + tokens[i].symbol +
                R"(","totalSupply":)" + std::to_string(tokens[i].totalSupply) +
                R"(,"maxSupply":)" + std::to_string(tokens[i].maxSupply) +
                R"(,"decimals":)" + std::to_string(tokens[i].decimals) +
                R"(,"backingModel":)" + std::to_string(static_cast<uint8_t>(tokens[i].backingModel)) +
                R"(,"backingRatio":)" + std::to_string(tokens[i].backingRatio) + "}";
      if (i < tokens.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodTransfer(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::Transfer;
    common::podFromHex(params("from").getString(), tx.from);
    common::podFromHex(params("to").getString(), tx.to);
    tx.amount = static_cast<uint64_t>(params("amount").getInteger());
    tx.tokenId = static_cast<uint64_t>(params("tokenId").getInteger());

    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;

    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));
    tx.extra.clear();

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (m_validator.submitTransaction(tx))
      return "\"" + common::podToHex(tx.txHash) + "\"";
    return "false";
  }

  std::string SidechainRpcServer::methodCreateToken(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::CreateToken;

    try
    {
      const common::JsonValue &fromVal = params("from");
      const common::JsonValue &nameHexVal = params("nameHex");
      const common::JsonValue &symbolHexVal = params("symbolHex");
      const common::JsonValue &supplyVal = params("initialSupply");
      const common::JsonValue &modelVal = params("backingModel");

      std::string fromStr = fromVal.getString();
      common::podFromHex(fromStr, tx.from);
      tx.to = tx.from;
      tx.amount = static_cast<uint64_t>(supplyVal.getInteger());

      tx.fee = 0;
      tx.feeTokenId = 0;
      tx.tokenId = 0;

      uint8_t backingModel = static_cast<uint8_t>(static_cast<uint64_t>(modelVal.getInteger()));

      uint8_t decimals = 6;
      if (params.contains("decimals"))
        decimals = static_cast<uint8_t>(static_cast<uint64_t>(params("decimals").getInteger()));

      std::string nameHex = nameHexVal.getString();
      std::string symbolHex = symbolHexVal.getString();

      cn::BinaryArray nameBytes = common::fromHex(nameHex);
      cn::BinaryArray symbolBytes = common::fromHex(symbolHex);

      std::string name(nameBytes.begin(), nameBytes.end());
      std::string symbol(symbolBytes.begin(), symbolBytes.end());

      std::string combined = name + ":" + symbol + ":" + std::to_string(backingModel) + ":" + std::to_string(decimals);
      tx.extra.assign(combined.begin(), combined.end());

      cn::BinaryArray txBytes = cn::toBinaryArray(tx);
      crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

      if (m_validator.submitTransaction(tx))
        return "\"" + common::podToHex(tx.txHash) + "\"";
    }
    catch (const std::exception &e)
    {
      return "false";
    }
    return "false";
  }

  std::string SidechainRpcServer::methodMintToken(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::Mint;
    common::podFromHex(params("from").getString(), tx.from);
    common::podFromHex(params("to").getString(), tx.to);
    tx.amount = static_cast<uint64_t>(params("amount").getInteger());
    tx.tokenId = static_cast<uint64_t>(params("tokenId").getInteger());

    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = tx.tokenId;

    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));
    tx.extra.clear();

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (m_validator.submitTransaction(tx))
      return "\"" + common::podToHex(tx.txHash) + "\"";
    return "false";
  }

  std::string SidechainRpcServer::methodBurnToken(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::Burn;
    common::podFromHex(params("from").getString(), tx.from);
    tx.to = tx.from;
    tx.amount = static_cast<uint64_t>(params("amount").getInteger());
    tx.tokenId = static_cast<uint64_t>(params("tokenId").getInteger());

    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = tx.tokenId;

    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));
    tx.extra.clear();

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (m_validator.submitTransaction(tx))
      return "\"" + common::podToHex(tx.txHash) + "\"";
    return "false";
  }

  std::string SidechainRpcServer::methodGetStatus(const common::JsonValue &)
  {
    uint64_t height = m_storage.topBlockHeight();
    uint64_t tokenCount = m_storage.getAllTokens().size();
    uint64_t pendingCount = m_validator.getPendingTransactions().size();
    return R"({"height":)" + std::to_string(height) +
           R"(,"tokenCount":)" + std::to_string(tokenCount) +
           R"(,"pendingTransactions":)" + std::to_string(pendingCount) + "}";
  }

  std::string SidechainRpcServer::methodGetPendingTransactions(const common::JsonValue &)
  {
    auto pending = m_validator.getPendingTransactions();
    return std::to_string(pending.size());
  }

  std::string SidechainRpcServer::methodGetValidators(const common::JsonValue &)
  {
    auto validators = m_validator.getValidators();
    std::string result = "[";
    for (size_t i = 0; i < validators.size(); ++i)
    {
      result += R"({"id":)" + std::to_string(validators[i].id) +
                R"(,"host":")" + validators[i].host +
                R"(","port":)" + std::to_string(validators[i].port) +
                R"(,"stake":)" + std::to_string(validators[i].stake) +
                R"(,"active":)" + (validators[i].active ? "true" : "false") + "}";
      if (i < validators.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodGetTransactions(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);

    uint64_t topHeight = m_storage.topBlockHeight();
    std::string result = "[";

    bool first = true;
    for (uint64_t h = 1; h <= topHeight; ++h)
    {
      Block block;
      if (!m_storage.getBlock(h, block))
        continue;

      for (const auto &tx : block.transactions)
      {
        if (tx.from == pubKey || tx.to == pubKey)
        {
          if (!first)
            result += ",";
          first = false;

          std::string typeStr;
          switch (tx.type)
          {
          case TransactionType::Transfer:
            typeStr = "Transfer";
            break;
          case TransactionType::CreateToken:
            typeStr = "CreateToken";
            break;
          case TransactionType::Mint:
            typeStr = "Mint";
            break;
          case TransactionType::Burn:
            typeStr = "Burn";
            break;
          default:
            typeStr = "Unknown";
            break;
          }

          result += "{";
          result += R"("txHash":")" + common::podToHex(tx.txHash) + R"(")";
          result += R"(,"type":")" + typeStr + R"(")";
          result += R"(,"from":")" + common::podToHex(tx.from) + R"(")";
          result += R"(,"to":")" + common::podToHex(tx.to) + R"(")";
          result += R"(,"amount":)" + std::to_string(tx.amount);
          result += R"(,"tokenId":)" + std::to_string(tx.tokenId);
          result += R"(,"fee":)" + std::to_string(tx.fee);
          result += R"(,"blockHeight":)" + std::to_string(h);
          result += R"(,"timestamp":)" + std::to_string(tx.timestamp);
          result += "}";
        }
      }
    }

    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodFaucet(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);

    // Check if this address has already claimed
    std::string claimKey = "faucet_" + common::podToHex(pubKey);
    std::vector<uint8_t> claimed;
    if (m_storage.getMeta(claimKey, claimed) && !claimed.empty())
    {
      return R"({"status":"error","message":"already claimed"})";
    }

    // Verify the address has received a transaction before
    // This prevents bots from generating fresh keys to farm the faucet
    uint64_t topHeight = m_storage.topBlockHeight();
    bool hasHistory = false;
    for (uint64_t h = 1; h <= topHeight && !hasHistory; ++h)
    {
      Block block;
      if (!m_storage.getBlock(h, block))
        continue;
      for (const auto &tx : block.transactions)
      {
        if (tx.to == pubKey)
        {
          hasHistory = true;
          break;
        }
      }
    }

    if (!hasHistory)
    {
      return R"({"status":"error","message":"address has no transaction history, send a token or SCCX to this address first"})";
    }

    // Credit the address with enough SCCX for 2 transfers
    uint64_t faucetAmount = SidechainConfig::FAUCET_AMOUNT;
    uint64_t currentBalance = 0;
    m_storage.getBalance(pubKey, 0, currentBalance);
    m_storage.setBalance(pubKey, 0, currentBalance + faucetAmount);

    // Mark as claimed
    m_storage.putMeta(claimKey, {1});

    return R"({"status":"ok","amount":)" + std::to_string(faucetAmount) + R"(,"balance":)" + std::to_string(currentBalance + faucetAmount) + "}";
  }

  std::string SidechainRpcServer::methodGetSidechainStatus(const common::JsonValue &)
  {
    return R"({"sidechainEnabled":true,"sidechainHost":")" + m_sidechainHost + R"(","sidechainPort":)" + std::to_string(m_sidechainPort) + "}";
  }

  std::string SidechainRpcServer::methodGetSidechainTokens(const common::JsonValue &)
  {
    return methodGetTokens(common::JsonValue(common::JsonValue::OBJECT));
  }

  std::string SidechainRpcServer::methodSidechainTransfer(const common::JsonValue &params)
  {
    return methodTransfer(params);
  }

  std::string SidechainRpcServer::methodSidechainCreateToken(const common::JsonValue &params)
  {
    return methodCreateToken(params);
  }
}