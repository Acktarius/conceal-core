// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "P2p/AiProviderTypes.h"
#include "Serialization/ISerializer.h"

namespace cn {

struct COMMAND_RPC_REGISTER_AI_PROVIDER {
  struct request {
    AiProviderAdvertisement advertisement;

    void serialize(ISerializer& s) { s(advertisement, "advertisement"); }
  };

  struct response {
    std::string status;
    std::string reason;

    void serialize(ISerializer& s) {
      s(status, "status");
      s(reason, "reason");
    }
  };
};

struct COMMAND_RPC_GET_AI_PROVIDERS {
  struct request {
    std::string modelId;
    uint8_t status;
    uint32_t minContextTokens;
    uint8_t billingUnit;
    uint64_t cursor;
    uint16_t limit;

    request() : status(0), minContextTokens(0), billingUnit(0), cursor(0), limit(0) {}

    void serialize(ISerializer& s) {
      s(modelId, "modelId");
      s(status, "status");
      s(minContextTokens, "minContextTokens");
      s(billingUnit, "billingUnit");
      s(cursor, "cursor");
      s(limit, "limit");
    }
  };

  struct response {
    std::string status;
    uint64_t nextCursor;
    bool pageDone;
    bool directoryComplete;
    std::vector<AiProviderListing> providers;

    response() : nextCursor(0), pageDone(true), directoryComplete(false) {}

    void serialize(ISerializer& s) {
      s(status, "status");
      s(nextCursor, "nextCursor");
      s(pageDone, "pageDone");
      s(directoryComplete, "directoryComplete");
      serializeBoundedVector<AiProviderListing, AI_MAX_PAGE_RECORDS>(providers, "providers", s);
    }
  };
};

}
