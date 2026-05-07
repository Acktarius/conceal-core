// BoltHttpServer.h — High-performance HTTP server with thread pool and epoll
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltHttpRequest.h"
#include "BoltHttpResponse.h"

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace BoltHttp
{
  using RequestHandler = std::function<void(const Request &, Response &)>;

  class Server
  {
  public:
    explicit Server(size_t threadCount = 4);
    ~Server();

    void start(const std::string &bindIp, uint16_t port);
    void stop();

    void onRequest(RequestHandler handler);

  private:
    void acceptLoop();
    void workerLoop(int epollFd);
    void handleClient(int clientFd);

    RequestHandler m_handler;
    int m_serverSocket = -1;
    int m_epollFd = -1;
    std::atomic<bool> m_running{false};
    size_t m_threadCount;
    std::thread m_acceptThread;
    std::vector<std::thread> m_workers;
    std::mutex m_clientsMutex;
    std::unordered_map<int, std::string> m_clientBuffers;
  };
}