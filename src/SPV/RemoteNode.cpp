// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "RemoteNode.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace SPV
{
  RemoteNode::RemoteNode(const std::string &host, uint16_t port)
      : m_host(host), m_port(port), m_socket(-1)
  {
  }

  RemoteNode::~RemoteNode()
  {
    disconnect();
  }

  bool RemoteNode::connect()
  {
    if (m_socket != -1)
    {
      close(m_socket);
      m_socket = -1;
    }

    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(m_port);
    int s = getaddrinfo(m_host.c_str(), port_str.c_str(), &hints, &result);
    if (s != 0)
      return false;

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
      m_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (m_socket == -1)
        continue;

      struct timeval timeout;
      timeout.tv_sec = 30;
      timeout.tv_usec = 0;
      setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

      if (::connect(m_socket, rp->ai_addr, rp->ai_addrlen) != -1)
        break;

      close(m_socket);
      m_socket = -1;
    }

    freeaddrinfo(result);
    return m_socket != -1;
  }

  void RemoteNode::disconnect()
  {
    if (m_socket != -1)
    {
      close(m_socket);
      m_socket = -1;
    }
  }

  std::string RemoteNode::recvAll(int socket, int timeout_seconds)
  {
    std::string result;
    char buffer[8192];
    fd_set set;
    struct timeval timeout;
    int empty_count = 0;

    while (true)
    {
      FD_ZERO(&set);
      FD_SET(socket, &set);
      timeout.tv_sec = timeout_seconds;
      timeout.tv_usec = 0;

      int select_result = select(socket + 1, &set, NULL, NULL, &timeout);
      if (select_result < 0 || select_result == 0)
        break;

      ssize_t bytes = recv(socket, buffer, sizeof(buffer) - 1, 0);
      if (bytes < 0)
        break;
      if (bytes == 0)
      {
        empty_count++;
        if (empty_count > 2)
          break;
        continue;
      }

      empty_count = 0;
      buffer[bytes] = '\0';
      result += buffer;

      size_t header_end = result.find("\r\n\r\n");
      if (header_end != std::string::npos)
      {
        std::string headers = result.substr(0, header_end);
        size_t content_length_pos = headers.find("Content-Length:");
        if (content_length_pos != std::string::npos)
        {
          size_t value_start = headers.find(":", content_length_pos) + 1;
          size_t value_end = headers.find("\r\n", value_start);
          int content_length = std::stoi(headers.substr(value_start, value_end - value_start));
          size_t body_start = header_end + 4;
          if (result.size() - body_start >= (size_t)content_length)
            break;
        }
        else if (headers.find("Connection: close") != std::string::npos)
        {
          break;
        }
      }
    }

    return result;
  }

  HttpResult RemoteNode::post(const std::string &path, const std::string &body)
  {
    HttpResult result;
    result.success = false;
    result.statusCode = 0;

    for (int attempt = 0; attempt < 3; attempt++)
    {
      if (!connect())
      {
        if (attempt == 2)
        {
          result.error = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
          return result;
        }
        usleep(100000 * (attempt + 1));
        continue;
      }

      std::ostringstream request;
      request << "POST " << path << " HTTP/1.1\r\n";
      request << "Host: " << m_host << ":" << m_port << "\r\n";
      request << "Content-Type: application/json\r\n";
      request << "Content-Length: " << body.size() << "\r\n";
      request << "Connection: close\r\n";
      request << "\r\n";
      request << body;

      std::string request_str = request.str();
      ssize_t sent = send(m_socket, request_str.c_str(), request_str.size(), 0);
      if (sent != (ssize_t)request_str.size())
      {
        disconnect();
        if (attempt == 2)
        {
          result.error = "Failed to send request";
          return result;
        }
        usleep(100000 * (attempt + 1));
        continue;
      }

      std::string response = recvAll(m_socket, 30);
      disconnect();

      if (response.empty())
      {
        if (attempt == 2)
        {
          result.error = "Empty response";
          return result;
        }
        usleep(100000 * (attempt + 1));
        continue;
      }

      size_t status_pos = response.find(" ");
      if (status_pos == std::string::npos)
      {
        if (attempt == 2)
        {
          result.error = "Invalid response format";
          return result;
        }
        continue;
      }

      size_t status_end = response.find(" ", status_pos + 1);
      if (status_end == std::string::npos)
        status_end = response.find("\r\n", status_pos);
      if (status_end == std::string::npos)
      {
        if (attempt == 2)
        {
          result.error = "Invalid response format";
          return result;
        }
        continue;
      }

      std::string status_code_str = response.substr(status_pos + 1, status_end - status_pos - 1);
      result.statusCode = std::stoi(status_code_str);

      size_t header_end = response.find("\r\n\r\n");
      if (header_end == std::string::npos)
      {
        if (attempt == 2)
        {
          result.error = "Invalid response format (no headers)";
          return result;
        }
        continue;
      }

      result.body = response.substr(header_end + 4);
      result.success = (result.statusCode == 200);

      if (!result.success)
        result.error = "HTTP " + std::to_string(result.statusCode);

      return result;
    }

    return result;
  }

  HttpResult RemoteNode::get(const std::string &path)
  {
    HttpResult result;
    result.success = false;

    if (!connect())
    {
      result.error = "Failed to connect";
      return result;
    }

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n";
    request << "Host: " << m_host << ":" << m_port << "\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";

    std::string request_str = request.str();
    send(m_socket, request_str.c_str(), request_str.size(), 0);

    std::string response = recvAll(m_socket, 30);
    disconnect();

    if (response.empty())
    {
      result.error = "Empty response";
      return result;
    }

    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
      result.error = "Invalid response format";
      return result;
    }

    result.body = response.substr(header_end + 4);
    result.success = true;
    result.statusCode = 200;

    return result;
  }
}