// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <cstdint>

namespace SPV
{
  struct HttpResult
  {
    bool success;
    int statusCode;
    std::string body;
    std::string error;
  };

  class RemoteNode
  {
  public:
    RemoteNode(const std::string &host, uint16_t port);
    ~RemoteNode();

    HttpResult post(const std::string &path, const std::string &body);
    HttpResult get(const std::string &path);

    bool isConnected() const { return m_socket != -1; }
    void disconnect();

  private:
    std::string m_host;
    uint16_t m_port;
    int m_socket;

    bool connect();
    std::string recvAll(int socket, int timeout_seconds);
  };
}