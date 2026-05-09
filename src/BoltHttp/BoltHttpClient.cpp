// BoltHttpClient.cpp — Simple HTTP client implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltHttpClient.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

namespace BoltHttp
{
  HttpClient::HttpClient(const std::string &host, uint16_t port)
      : m_host(host), m_port(port)
  {
  }

  HttpClient::~HttpClient()
  {
    if (m_socket >= 0)
    {
      shutdown(m_socket, SHUT_RDWR);
      close(m_socket);
    }
  }

  bool HttpClient::connect()
  {
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0)
      return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

    if (::connect(m_socket, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
      close(m_socket);
      m_socket = -1;
      return false;
    }

    return true;
  }

  HttpClientResponse HttpClient::post(const std::string &path, const std::string &body)
  {
    HttpClientResponse resp;

    if (!connect())
    {
      resp.error = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
      return resp;
    }

    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << m_host << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;

    std::string reqStr = request.str();
    ssize_t sent = send(m_socket, reqStr.c_str(), reqStr.size(), 0);
    if (sent <= 0)
    {
      resp.error = "Failed to send request";
      return resp;
    }

    // Read response
    char buf[4096];
    std::string raw;
    while (true)
    {
      ssize_t n = recv(m_socket, buf, sizeof(buf) - 1, 0);
      if (n <= 0)
        break;
      buf[n] = '\0';
      raw += buf;
    }

    // Parse response
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
      resp.error = "Invalid HTTP response";
      return resp;
    }

    // Parse status line
    std::string headers = raw.substr(0, headerEnd);
    std::istringstream headerStream(headers);
    std::string statusLine;
    std::getline(headerStream, statusLine);

    size_t codeStart = statusLine.find(' ');
    if (codeStart != std::string::npos)
    {
      std::string codeStr = statusLine.substr(codeStart + 1, 3);
      resp.statusCode = std::stoi(codeStr);
    }

    resp.body = raw.substr(headerEnd + 4);
    resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);

    return resp;
  }

  HttpClientResponse HttpClient::get(const std::string &path)
  {
    HttpClientResponse resp;

    if (!connect())
    {
      resp.error = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
      return resp;
    }

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << m_host << "\r\n"
            << "Connection: close\r\n"
            << "\r\n";

    std::string reqStr = request.str();
    ssize_t sent = send(m_socket, reqStr.c_str(), reqStr.size(), 0);
    if (sent <= 0)
    {
      resp.error = "Failed to send request";
      return resp;
    }

    char buf[4096];
    std::string raw;
    while (true)
    {
      ssize_t n = recv(m_socket, buf, sizeof(buf) - 1, 0);
      if (n <= 0)
        break;
      buf[n] = '\0';
      raw += buf;
    }

    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
      resp.error = "Invalid HTTP response";
      return resp;
    }

    std::string headers = raw.substr(0, headerEnd);
    std::istringstream headerStream(headers);
    std::string statusLine;
    std::getline(headerStream, statusLine);

    size_t codeStart = statusLine.find(' ');
    if (codeStart != std::string::npos)
    {
      std::string codeStr = statusLine.substr(codeStart + 1, 3);
      resp.statusCode = std::stoi(codeStr);
    }

    resp.body = raw.substr(headerEnd + 4);
    resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);

    return resp;
  }
}