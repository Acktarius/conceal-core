// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto/crypto.h"
#include "BoltCore/BoltCoreTypes.h"

namespace cn
{
  class INode;
  class Currency;
}
namespace BoltCore
{
  class Wallet;
}

namespace ClientWallet
{

  class SyncEngine;
  class Screen;

  struct SyncWalletUpdate
  {
    std::vector<BoltCore::OutputInfo> outputs;
    std::vector<crypto::KeyImage> spentKeyImages;
    std::vector<std::pair<crypto::Hash, uint32_t>> depositSpends;
    uint32_t scannedHeight = 0;
    bool updateHeight = false;
  };

  enum class EventType
  {
    KeyPress,
    HeightChanged,
    SyncProgress,
    OutputsReceived,
    BalanceChanged,
    Timer,
    Quit
  };

  struct Event
  {
    Event() = default;
    Event(EventType t) : type(t) {}
    Event(EventType t, int k) : type(t), keyCode(k) {}

    EventType type = EventType::KeyPress;
    int keyCode = 0;
    uint32_t height = 0;
    std::string message;
  };

  class EventLoop
  {
  public:
    EventLoop();
    ~EventLoop();

    void setWallet(std::shared_ptr<BoltCore::Wallet> wallet);
    void setSyncEngine(std::shared_ptr<SyncEngine> sync);
    void setNode(cn::INode *node);

    void run();
    void stop();

    void pushScreen(std::shared_ptr<Screen> screen);
    void popScreen();

    void setStatePath(const std::string &path);

    void setCurrency(const cn::Currency &currency) { m_currency = &currency; }

    void checkMempool();

  private:
    void processEvent(const Event &event);
    void checkKeyboard();
    void checkDaemonHeight();
    void renderCurrentScreen();
    void updateStatusBar();
    void timerTick();
    void enqueueSyncUpdate(SyncWalletUpdate update);
    void applyPendingSyncUpdates();

    std::string m_statePath;

    std::shared_ptr<BoltCore::Wallet> m_wallet;
    std::shared_ptr<SyncEngine> m_sync;

    cn::INode *m_node = nullptr;
    const cn::Currency *m_currency = nullptr;

    std::vector<std::shared_ptr<Screen>> m_screens;
    std::atomic<bool> m_running{false};

    std::mutex m_syncUpdateMutex;
    std::deque<SyncWalletUpdate> m_pendingSyncUpdates;

    std::chrono::steady_clock::time_point m_lastRender;
    std::chrono::steady_clock::time_point m_lastHeightCheck;
    std::chrono::steady_clock::time_point m_lastMempoolCheck;

    uint32_t m_lastKnownHeight = 0;
    bool m_needsRedraw = true;

    static constexpr int RENDER_INTERVAL_MS = 50;
    static constexpr int HEIGHT_CHECK_INTERVAL_SEC = 5;
    static constexpr int MEMPOOL_CHECK_INTERVAL_SEC = 10;
  };

} // namespace ClientWallet
