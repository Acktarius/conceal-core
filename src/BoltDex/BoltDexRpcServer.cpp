// BoltDexRpcServer.cpp — JSON-RPC server for DEX
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltDexRpcServer.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include <sstream>

namespace BoltDex
{
  RpcServer::RpcServer(logging::ILogger &logger, Engine &engine)
      : m_logger(logger, "BoltDexRPC"), m_engine(engine)
  {
  }

  void RpcServer::start(const std::string &bindIp, uint16_t bindPort, size_t threadCount)
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
    m_logger(logging::INFO) << "BoltDex RPC server started on " << bindIp << ":" << bindPort;
  }

  void RpcServer::stop()
  {
    if (m_httpServer)
      m_httpServer->stop();
  }

  std::string RpcServer::handleJsonRpc(const std::string &body)
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

      std::string result;
      if (method == "getOrders")
        result = methodGetOrders(params);
      else if (method == "getTrades")
        result = methodGetTrades(params);
      else if (method == "getAllTrades")
        result = methodGetAllTrades(params);
      else if (method == "submitOrder")
        result = methodSubmitOrder(params);
      else if (method == "cancelOrder")
        result = methodCancelOrder(params);
      else if (method == "deposit")
        result = methodDeposit(params);
      else if (method == "withdraw")
        result = methodWithdraw(params);
      else if (method == "getEscrowBalance")
        result = methodGetEscrowBalance(params);
      else if (method == "getStatus")
        result = methodGetStatus(params);
      else
      {
        return R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"Method not found"},"id":)" + id + "}";
      }

      return R"({"jsonrpc":"2.0","result":)" + result + R"(,"id":)" + id + "}";
    }
    catch (const std::exception &e)
    {
      return R"({"jsonrpc":"2.0","error":{"code":-32603,"message":")" + std::string(e.what()) + R"("},"id":null})";
    }
  }

  std::string RpcServer::methodGetOrders(const common::JsonValue &params)
  {
    uint64_t baseTokenId = static_cast<uint64_t>(params("baseTokenId").getInteger());
    uint64_t quoteTokenId = static_cast<uint64_t>(params("quoteTokenId").getInteger());

    auto orders = m_engine.getOrders(baseTokenId, quoteTokenId);
    std::string result = "[";
    for (size_t i = 0; i < orders.size(); ++i)
    {
      result += "{";
      result += R"("id":)" + std::to_string(orders[i].id) + ",";
      result += R"("type":")" + std::string(orders[i].type == OrderType::Buy ? "buy" : "sell") + "\",";
      result += R"("owner":")" + common::podToHex(orders[i].owner) + "\",";
      result += R"("amount":)" + std::to_string(orders[i].amount) + ",";
      result += R"("price":)" + std::to_string(orders[i].price) + ",";
      result += R"("filled":)" + std::to_string(orders[i].filled) + ",";
      result += R"("status":")" + std::string(orders[i].status == OrderStatus::Open ? "open" : (orders[i].status == OrderStatus::Filled ? "filled" : "cancelled")) + "\"";
      result += "}";
      if (i < orders.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string RpcServer::methodGetTrades(const common::JsonValue &params)
  {
    uint64_t baseTokenId = static_cast<uint64_t>(params("baseTokenId").getInteger());
    uint64_t quoteTokenId = static_cast<uint64_t>(params("quoteTokenId").getInteger());
    size_t limit = params.contains("limit") ? static_cast<size_t>(params("limit").getInteger()) : 50;

    auto trades = m_engine.getTrades(baseTokenId, quoteTokenId, limit);
    std::string result = "[";
    for (size_t i = 0; i < trades.size(); ++i)
    {
      result += "{";
      result += R"("id":)" + std::to_string(trades[i].id) + ",";
      result += R"("buyer":")" + common::podToHex(trades[i].buyer) + "\",";
      result += R"("seller":")" + common::podToHex(trades[i].seller) + "\",";
      result += R"("amount":)" + std::to_string(trades[i].amount) + ",";
      result += R"("price":)" + std::to_string(trades[i].price) + ",";
      result += R"("settled":)" + std::string(trades[i].settled ? "true" : "false") + ",";
      result += R"("timestamp":)" + std::to_string(trades[i].timestamp);
      result += "}";
      if (i < trades.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string RpcServer::methodGetAllTrades(const common::JsonValue &params)
  {
    size_t limit = params.contains("limit") ? static_cast<size_t>(params("limit").getInteger()) : 100;
    auto trades = m_engine.getAllTrades(limit);
    std::string result = "[";
    for (size_t i = 0; i < trades.size(); ++i)
    {
      result += "{";
      result += R"("id":)" + std::to_string(trades[i].id) + ",";
      result += R"("buyer":")" + common::podToHex(trades[i].buyer) + "\",";
      result += R"("seller":")" + common::podToHex(trades[i].seller) + "\",";
      result += R"("baseTokenId":)" + std::to_string(trades[i].baseTokenId) + ",";
      result += R"("quoteTokenId":)" + std::to_string(trades[i].quoteTokenId) + ",";
      result += R"("amount":)" + std::to_string(trades[i].amount) + ",";
      result += R"("price":)" + std::to_string(trades[i].price) + ",";
      result += R"("settled":)" + std::string(trades[i].settled ? "true" : "false") + ",";
      result += R"("timestamp":)" + std::to_string(trades[i].timestamp);
      result += "}";
      if (i < trades.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string RpcServer::methodSubmitOrder(const common::JsonValue &params)
  {
    Order order;
    order.type = params("type").getString() == "buy" ? OrderType::Buy : OrderType::Sell;
    common::podFromHex(params("owner").getString(), order.owner);
    order.baseTokenId = static_cast<uint64_t>(params("baseTokenId").getInteger());
    order.quoteTokenId = static_cast<uint64_t>(params("quoteTokenId").getInteger());
    order.amount = static_cast<uint64_t>(params("amount").getInteger());
    order.price = static_cast<uint64_t>(params("price").getInteger());

    if (m_engine.submitOrder(order))
      return "true";
    return "false";
  }

  std::string RpcServer::methodCancelOrder(const common::JsonValue &params)
  {
    uint64_t orderId = static_cast<uint64_t>(params("orderId").getInteger());
    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);

    if (m_engine.cancelOrder(orderId, owner))
      return "true";
    return "false";
  }

  std::string RpcServer::methodDeposit(const common::JsonValue &params)
  {
    // Users deposit by sending tokens to the DEX address via sidechain transfer.
    // The DEX watches for incoming transfers and credits escrow automatically.
    // This RPC returns the DEX deposit address.
    crypto::PublicKey dexPub = m_engine.getDexPublicKey();
    return R"({"dexAddress":")" + common::podToHex(dexPub) + R"("})";
  }

  std::string RpcServer::methodWithdraw(const common::JsonValue &params)
  {
    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);
    uint64_t tokenId = static_cast<uint64_t>(params("tokenId").getInteger());
    uint64_t amount = static_cast<uint64_t>(params("amount").getInteger());

    if (m_engine.withdraw(owner, tokenId, amount))
      return "true";
    return "false";
  }

  std::string RpcServer::methodGetEscrowBalance(const common::JsonValue &params)
  {
    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);
    uint64_t tokenId = static_cast<uint64_t>(params("tokenId").getInteger());

    uint64_t balance = m_engine.getEscrowBalance(owner, tokenId);
    return std::to_string(balance);
  }

  std::string RpcServer::methodGetStatus(const common::JsonValue &)
  {
    return R"({"dex":"BoltDex","status":"running"})";
  }
}