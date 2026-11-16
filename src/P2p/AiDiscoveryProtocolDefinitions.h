// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "AiProviderTypes.h"
#include "P2pProtocolDefinitions.h"

namespace cn {

struct COMMAND_AI_DISCOVERY_CAPABILITIES {
  enum { ID = AI_DISCOVERY_CAPABILITIES_ID };

  struct request {
    uint8_t protocolVersion;

    request() : protocolVersion(0) {}

    void serialize(ISerializer& s) { s(protocolVersion, "protocolVersion"); }
  };

  struct response {
    uint8_t protocolVersion;
    bool discoverySupported;
    uint16_t maxPageRecords;
    uint32_t maxMessageBytes;

    response() : protocolVersion(0), discoverySupported(false), maxPageRecords(0), maxMessageBytes(0) {}

    void serialize(ISerializer& s) {
      s(protocolVersion, "protocolVersion");
      s(discoverySupported, "discoverySupported");
      s(maxPageRecords, "maxPageRecords");
      s(maxMessageBytes, "maxMessageBytes");
    }
  };
};

struct NOTIFY_AI_PROVIDER_ADVERTISEMENTS {
  enum { ID = AI_DISCOVERY_ADVERTISEMENTS_ID };

  struct request {
    uint8_t protocolVersion;
    std::vector<AiProviderAdvertisement> advertisements;

    request() : protocolVersion(0) {}

    void serialize(ISerializer& s) {
      s(protocolVersion, "protocolVersion");
      serializeBoundedVector<AiProviderAdvertisement, AI_MAX_ADS_PER_MESSAGE>(advertisements, "advertisements", s);
    }
  };
};

struct COMMAND_REQUEST_AI_PROVIDERS {
  enum { ID = AI_DISCOVERY_REQUEST_PROVIDERS_ID };

  struct request {
    uint8_t protocolVersion;
    uint64_t requestId;
    uint64_t cursor;
    uint16_t maxRecords;

    request() : protocolVersion(0), requestId(0), cursor(0), maxRecords(0) {}

    void serialize(ISerializer& s) {
      s(protocolVersion, "protocolVersion");
      s(requestId, "requestId");
      s(cursor, "cursor");
      s(maxRecords, "maxRecords");
    }
  };

  struct response {
    uint8_t protocolVersion;
    uint64_t requestId;
    uint64_t nextCursor;
    bool done;
    uint8_t result;
    std::vector<AiProviderAdvertisement> advertisements;

    response() : protocolVersion(0), requestId(0), nextCursor(0), done(true), result(AI_DIRECTORY_INVALID) {}

    void serialize(ISerializer& s) {
      s(protocolVersion, "protocolVersion");
      s(requestId, "requestId");
      s(nextCursor, "nextCursor");
      s(done, "done");
      s(result, "result");
      serializeBoundedVector<AiProviderAdvertisement, AI_MAX_PAGE_RECORDS>(advertisements, "advertisements", s);
    }
  };
};

}
