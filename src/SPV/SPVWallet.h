// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "HeaderChain.h"
#include "RemoteNode.h"
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

namespace SPV
{
  class SPVWallet
  {
  public:
    SPVWallet(const std::string &remoteHost, uint16_t remotePort);
    ~SPVWallet();

    // Initialization
    bool initFromBootstrap(const std::string &headersFile);
    bool initFromDownload(const std::string &url);
    bool initFromSync();

    // Node info
    uint32_t getNodeHeight();
    uint32_t getSyncedHeight() const { return m_chain.getHeight(); }

    // SPV verification
    bool getTransactionProof(const crypto::Hash &tx_hash, TransactionProof &proof);
    bool verifyTransaction(const TransactionProof &proof) const;

    // Header sync
    bool syncNewHeaders();

    // Server-aided scanning
    bool scanOutputs(const crypto::PublicKey &viewKey,
                     uint32_t fromHeight,
                     uint32_t toHeight,
                     std::vector<std::pair<crypto::Hash, uint64_t>> &outputs);

  private:
    RemoteNode *m_node; // Raw pointer for C++11
    HeaderChain m_chain;
    std::string m_remoteHost;
    uint16_t m_remotePort;
    std::string m_dataDir;

    bool sendJsonRpc(const std::string &method, const std::string &params, std::string &response);
    bool getBlockHeaderByHeight(uint32_t height, BlockHeader &header);
    bool getBlockHashByHeight(uint32_t height, crypto::Hash &hash);
    bool verifyPoW(const BlockHeader &header);
  };
}