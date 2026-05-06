// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license


#pragma once

#include <string>

namespace BoltSync
{
  namespace PathHelpers
  {
    inline std::string appendPath(const std::string &path, const std::string &fileName)
    {
      std::string result = path;
      if (!result.empty() && result.back() != '/')
        result += '/';
      result += fileName;
      return result;
    }
  }
}