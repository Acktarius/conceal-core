// BoltHttpServer.cpp — High-performance HTTP server implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltHttpServer.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <chrono>

namespace BoltHttp
{
  namespace
  {
    void setNonBlocking(int fd)
    {
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void addEpoll(int epollFd, int fd)
    {
      epoll_event ev{};
      ev.events = EPOLLIN | EPOLLET;
      ev.data.fd = fd;
      epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev);
    }

    bool parseRequest(const std::string &raw, Request &req)
    {
      std::istringstream stream(raw);
      std::string line;
      std::getline(stream, line);
      if (line.empty() || line.back() == '\r')
        line.pop_back();
      if (line.empty())
        return false;

      std::istringstream requestLine(line);
      requestLine >> req.method >> req.url;

      while (std::getline(stream, line) && line != "\r" && !line.empty())
      {
        if (line.back() == '\r')
          line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
          std::string key = line.substr(0, colon);
          std::string value = line.substr(colon + 1);
          if (!value.empty() && value[0] == ' ')
            value = value.substr(1);
          req.headers[key] = value;
        }
      }

      // Parse body from Content-Length
      auto it = req.headers.find("Content-Length");
      if (it != req.headers.end())
      {
        size_t contentLength = std::stoul(it->second);
        size_t bodyStart = raw.find("\r\n\r\n");
        if (bodyStart != std::string::npos)
        {
          bodyStart += 4;
          if (raw.size() >= bodyStart + contentLength)
          {
            std::string body = raw.substr(bodyStart, contentLength);
            req.body.assign(body.begin(), body.end());
          }
        }
      }

      return true;
    }
  }

  Server::Server(size_t threadCount)
      : m_threadCount(threadCount)
  {
  }

  Server::~Server()
  {
    stop();
  }

  void Server::start(const std::string &bindIp, uint16_t port)
  {
    m_serverSocket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (m_serverSocket < 0)
    {
      std::cerr << "BoltHttp: Failed to create socket" << std::endl;
      return;
    }

    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr);

    if (bind(m_serverSocket, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
      std::cerr << "BoltHttp: Failed to bind to " << bindIp << ":" << port << std::endl;
      close(m_serverSocket);
      return;
    }

    listen(m_serverSocket, SOMAXCONN);

    // Create epoll instance shared by all workers
    m_epollFd = epoll_create1(0);
    if (m_epollFd < 0)
    {
      std::cerr << "BoltHttp: Failed to create epoll" << std::endl;
      close(m_serverSocket);
      return;
    }

    addEpoll(m_epollFd, m_serverSocket);

    m_running = true;

    // Start worker threads
    for (size_t i = 0; i < m_threadCount; ++i)
      m_workers.emplace_back(&Server::workerLoop, this, m_epollFd);

    // Start accept thread
    m_acceptThread = std::thread(&Server::acceptLoop, this);

    std::cout << "BoltHttp: Listening on " << bindIp << ":" << port
              << " (" << m_threadCount << " workers)" << std::endl;
  }

  void Server::stop()
  {
    m_running = false;

    if (m_serverSocket >= 0)
    {
      shutdown(m_serverSocket, SHUT_RDWR);
      close(m_serverSocket);
      m_serverSocket = -1;
    }

    if (m_epollFd >= 0)
    {
      close(m_epollFd);
      m_epollFd = -1;
    }

    if (m_acceptThread.joinable())
      m_acceptThread.join();

    // Detach workers — they will exit on process termination
    for (auto &t : m_workers)
    {
      if (t.joinable())
        t.detach();
    }
    m_workers.clear();
  }

  void Server::onRequest(RequestHandler handler)
  {
    m_handler = handler;
  }

  void Server::acceptLoop()
  {
    while (m_running)
    {
      sockaddr_in clientAddr{};
      socklen_t clientLen = sizeof(clientAddr);
      int clientFd = accept4(m_serverSocket, (sockaddr *)&clientAddr, &clientLen, SOCK_NONBLOCK);

      if (clientFd < 0)
        continue;

      setNonBlocking(clientFd);

      {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientBuffers[clientFd] = "";
      }

      addEpoll(m_epollFd, clientFd);
    }
  }

  void Server::workerLoop(int epollFd)
  {
    epoll_event events[64];

    while (m_running)
    {
      int nfds = epoll_wait(epollFd, events, 64, 100);
      if (nfds < 0)
      {
        if (errno == EINTR)
          continue;
        break;
      }

      for (int i = 0; i < nfds; ++i)
      {
        int fd = events[i].data.fd;

        if (fd == m_serverSocket)
          continue; // accept thread handles this

        handleClient(fd);
      }
    }
  }

  void Server::handleClient(int clientFd)
  {
    char buffer[4096];
    std::string &accumulator = m_clientBuffers[clientFd];

    // Read all available data
    while (true)
    {
      ssize_t n = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
      if (n > 0)
      {
        buffer[n] = '\0';
        accumulator += buffer;
      }
      else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
      {
        // Connection closed or error
        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientBuffers.erase(clientFd);
        return;
      }
      else
      {
        break; // No more data available right now
      }
    }

    // Check if we have a complete request
    if (accumulator.find("\r\n\r\n") != std::string::npos)
    {
      Request req;
      if (parseRequest(accumulator, req))
      {
        Response resp;
        resp.headers["Content-Type"] = "application/json";
        resp.headers["Access-Control-Allow-Origin"] = "*";

        if (m_handler)
          m_handler(req, resp);

        std::vector<uint8_t> rawResp = resp.toBytes();
        send(clientFd, rawResp.data(), rawResp.size(), MSG_NOSIGNAL);
      }

      // Close after response — no keep-alive
      shutdown(clientFd, SHUT_RDWR);
      close(clientFd);
      std::lock_guard<std::mutex> lock(m_clientsMutex);
      m_clientBuffers.erase(clientFd);
    }
  }
}