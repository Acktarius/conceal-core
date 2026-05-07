// SidechainTypes.h — all data structures for blocks, transactions, tokens
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "crypto/crypto.h"
#include "Serialization/ISerializer.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"

namespace Sidechain
{
  // Tokens
  enum class TokenBackingModel : uint8_t
  {
    Unbacked = 0,
    Backed = 1,
    Hybrid = 2
  };

  struct TokenInfo
  {
    uint64_t id = 0;
    std::string name;
    std::string symbol;
    uint64_t totalSupply = 0;
    uint64_t maxSupply = 0;
    uint8_t decimals = 6;
    TokenBackingModel backingModel = TokenBackingModel::Unbacked;
    uint64_t backingRatio = 0;
    crypto::Hash backingTxHash;
    uint64_t lockedCCXAmount = 0;

    void serialize(cn::ISerializer &s)
    {
      s(id, "id");
      s(name, "name");
      s(symbol, "symbol");
      s(totalSupply, "totalSupply");
      s(maxSupply, "maxSupply");
      s(decimals, "decimals");
      uint8_t model = static_cast<uint8_t>(backingModel);
      s(model, "backingModel");
      backingModel = static_cast<TokenBackingModel>(model);
      s(backingRatio, "backingRatio");
      s(backingTxHash, "backingTxHash");
      s(lockedCCXAmount, "lockedCCXAmount");
    }
  };

  // Transactions
  enum class TransactionType : uint8_t
  {
    Transfer = 0,
    Mint = 1,
    Burn = 2,
    CreateToken = 3,
    Stake = 4,
    Unstake = 5,
    ClaimReward = 6,
  };

  struct Transaction
  {
    uint64_t id = 0;
    TransactionType type = TransactionType::Transfer;
    crypto::PublicKey from;
    crypto::PublicKey to;
    uint64_t amount = 0;
    uint64_t fee = 0;
    uint64_t tokenId = 0;
    uint64_t feeTokenId = 0;
    uint64_t timestamp = 0;
    crypto::Hash txHash;
    std::vector<uint8_t> extra;
    crypto::Signature signature;

    void serialize(cn::ISerializer &s)
    {
      s(id, "id");
      uint8_t typeVal = static_cast<uint8_t>(type);
      s(typeVal, "type");
      type = static_cast<TransactionType>(typeVal);
      s(from, "from");
      s(to, "to");
      s(amount, "amount");
      s(fee, "fee");
      s(tokenId, "tokenId");
      s(feeTokenId, "feeTokenId");
      s(timestamp, "timestamp");
      s(extra, "extra");
      s(signature, "signature");
    }
  };

  // Blocks
  struct BlockHeader
  {
    uint64_t height = 0;
    crypto::Hash previousBlockHash;
    crypto::Hash blockHash;
    uint64_t timestamp = 0;
    uint32_t validatorId = 0;
    crypto::Signature validatorSignature;

    void serialize(cn::ISerializer &s)
    {
      s(height, "height");
      s(previousBlockHash, "previousBlockHash");
      s(blockHash, "blockHash");
      s(timestamp, "timestamp");
      s(validatorId, "validatorId");
      s(validatorSignature, "validatorSignature");
    }
  };

  struct Block
  {
    BlockHeader header;
    std::vector<Transaction> transactions;

    void serialize(cn::ISerializer &s)
    {
      s(header, "header");
      s(transactions, "transactions");
    }
  };

  // Validator
  struct ValidatorInfo
  {
    uint32_t id = 0;
    crypto::PublicKey publicKey;
    crypto::SecretKey secretKey;
    std::string host;
    uint16_t port = 0;
    uint64_t stake = 0;
    bool active = true;

    void serialize(cn::ISerializer &s)
    {
      s(id, "id");
      s(publicKey, "publicKey");
      s(host, "host");
      s(port, "port");
      s(stake, "stake");
      s(active, "active");
    }
  };

  // State
  struct AccountState
  {
    crypto::PublicKey address;
    std::unordered_map<uint64_t, uint64_t> balances;
  };
}