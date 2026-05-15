// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <chrono>
#include <fstream>

#include "Blockchain/Blockchain.h"
#include "Common/StdInputStream.h"
#include "Common/StdOutputStream.h"
#include "Logging/LoggerRef.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"

namespace cn
{

  class BlockCacheSerializer
  {
  public:
    BlockCacheSerializer(Blockchain &bs, const crypto::Hash &lastBlockHash,
                         logging::ILogger &logger)
        : m_bs(bs), m_lastBlockHash(lastBlockHash),
          logger(logger, "BlockCacheSerializer") {}

    void load(const std::string &filename)
    {
      std::ifstream stdStream;
      try
      {
        stdStream.open(filename, std::ios::binary);
        if (!stdStream)
          return;

        common::StdInputStream stream(stdStream);
        BinaryInputStreamSerializer s(stream);
        cn::serialize(*this, s);
        stdStream.close();
      }
      catch (const std::exception &e)
      {
        logger(logging::WARNING) << "loading failed: " << e.what();
      }
    }

    bool save(const std::string &filename)
    {
      std::ofstream file;
      try
      {
        file.open(filename, std::ios::binary);
        if (!file)
          return false;

        common::StdOutputStream stream(file);
        BinaryOutputStreamSerializer s(stream);
        cn::serialize(*this, s);

        file.flush();
        file.close();
        return true;
      }
      catch (const std::exception &)
      {
        if (file.is_open())
          file.close();
        return false;
      }
    }

    void serialize(ISerializer &s);

    bool loaded() const { return m_loaded; }

  private:
    Blockchain &m_bs;
    crypto::Hash m_lastBlockHash;
    logging::LoggerRef logger;
    bool m_loaded = false;
  };

} // namespace cn