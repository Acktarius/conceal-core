// main.cpp — Conceal unified client entry point
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "CLI.h"
#include "StatusPanel.h"
#include "TuiRenderer.h"
#include "Components/MainchainThread.h"
#include "Components/SidechainThread.h"
#include "Components/WalletThread.h"
#include "Commands/CommandHandler.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "version.h"

using namespace ftxui;

namespace
{
  // Write a JSON status snapshot to file for GUI/external tool consumption
  void writeStatusFile(const std::string &path,
                       const Conceal::MainchainStatus &mainchain,
                       const Conceal::SidechainStatus &sidechain,
                       const Conceal::WalletStatus &wallet)
  {
    std::ofstream f(path);
    if (!f)
      return;
    f << "{"
      << R"("mainchain":{)"
      << R"("localHeight":)" << mainchain.localHeight << ","
      << R"("networkHeight":)" << mainchain.networkHeight << ","
      << R"("peerCount":)" << mainchain.peerCount << ","
      << R"("synced":)" << (mainchain.synced ? "true" : "false")
      << "},"
      << R"("sidechain":{)"
      << R"("height":)" << sidechain.height << ","
      << R"("validatorCount":)" << sidechain.validatorCount << ","
      << R"("tokenCount":)" << sidechain.tokenCount << ","
      << R"("dexFee":)" << sidechain.dexFee << ","
      << R"("bridgeWatching":)" << (sidechain.bridgeWatching ? "true" : "false")
      << "},"
      << R"("wallet":{)"
      << R"("availableBalance":)" << wallet.availableBalance << ","
      << R"("lockedBalance":)" << wallet.lockedBalance << ","
      << R"("walletHeight":)" << wallet.walletHeight << ","
      << R"("synced":)" << (wallet.synced ? "true" : "false") << ","
      << R"("address":")" << wallet.getAddress() << R"(")"
      << "}"
      << "}";
  }
}

int main(int argc, char *argv[])
{
  // Parse command line
  Conceal::Config cfg;
  if (!Conceal::parseArgs(argc, argv, cfg))
    return 1;

  // Bridge wallet reuses bridge keys automatically
  if (cfg.useBridgeWallet)
  {
    cfg.runWallet = true;
  }

  // Redirect logging to file if requested
  if (!cfg.logFile.empty())
  {
    // freopen redirects stdout to the log file for all C library output
    freopen(cfg.logFile.c_str(), "a", stdout);
    freopen(cfg.logFile.c_str(), "a", stderr);
  }

  std::cout << CCX_RELEASE_VERSION << std::endl;
  std::cout << "Starting Conceal unified client..." << std::endl;

  // Shared status structs updated by worker threads, read by TUI/status writer
  Conceal::MainchainStatus mainchainStatus;
  Conceal::SidechainStatus sidechainStatus;
  Conceal::WalletStatus walletStatus;

  // Single stop flag shared with all worker threads for clean coordinated shutdown
  std::atomic<bool> stopRequested{false};

  // Launch each subsystem as a background thread
  std::vector<std::thread> workers;

  if (cfg.runMainchain)
  {
    workers.emplace_back(Conceal::runMainchain,
                         std::ref(cfg), std::ref(mainchainStatus), std::ref(stopRequested));
  }

  if (cfg.runSidechain)
  {
    workers.emplace_back(Conceal::runSidechain,
                         std::ref(cfg), std::ref(sidechainStatus), std::ref(stopRequested));
  }

  if (cfg.runWallet)
  {
    workers.emplace_back(Conceal::runWallet,
                         std::ref(cfg), std::ref(walletStatus), std::ref(stopRequested));
  }

  // Allow workers to bind ports and initialize before accepting external connections
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Write ready signal sentinel file so GUI/automation knows services are up
  if (!cfg.readySignal.empty())
  {
    std::ofstream readyFile(cfg.readySignal);
    readyFile << "ready" << std::endl;
  }

  // Headless mode: no TUI, just status file and signal handling
  if (cfg.noTui)
  {
    std::cout << "Running in headless mode. Press Ctrl+C to stop." << std::endl;

    // Periodically write JSON status snapshot for GUI consumption
    std::thread statusWriter([&]()
                             {
      while (!stopRequested)
      {
        if (!cfg.statusFile.empty())
          writeStatusFile(cfg.statusFile, mainchainStatus, sidechainStatus, walletStatus);
        std::this_thread::sleep_for(std::chrono::seconds(2));
      } });

    // Block until shutdown signal
    while (!stopRequested)
      std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (statusWriter.joinable())
      statusWriter.join();
  }
  // TUI mode: interactive terminal dashboard
  else
  {
    auto screen = ScreenInteractive::TerminalOutput();

    // Build the dashboard from live status structs
    auto renderer = Renderer([&]
                             { return Conceal::renderDashboard(mainchainStatus, sidechainStatus, walletStatus, stopRequested); });

    // Quit on 'q' or Escape
    renderer |= CatchEvent([&](Event event)
                           {
      if (event == Event::Character('q') || event == Event::Escape)
      {
        stopRequested = true;
        screen.ExitLoopClosure()();
        return true;
      }
      return false; });

    // Refresh the screen every 500ms by posting a custom event
    std::thread refreshThread([&]
                              {
      while (!stopRequested)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!stopRequested)
          screen.Post(Event::Custom);
      } });

    // Run the FTXUI event loop with exception safety to restore terminal
    std::thread tuiThread([&]
                          {
      try {
        screen.Loop(renderer);
      } catch (const std::exception& e) {
        screen.ExitLoopClosure()();
        std::cerr << std::endl << "TUI error: " << e.what() << std::endl;
        stopRequested = true;
      } catch (...) {
        screen.ExitLoopClosure()();
        std::cerr << std::endl << "TUI: unknown error" << std::endl;
        stopRequested = true;
      } });

    // Write JSON status file periodically even in TUI mode
    std::thread statusWriter([&]()
                             {
      while (!stopRequested)
      {
        if (!cfg.statusFile.empty())
          writeStatusFile(cfg.statusFile, mainchainStatus, sidechainStatus, walletStatus);
        std::this_thread::sleep_for(std::chrono::seconds(2));
      } });

    // Block until shutdown
    while (!stopRequested)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Restore terminal state before printing shutdown messages
    screen.ExitLoopClosure()();

    if (refreshThread.joinable())
      refreshThread.join();
    if (tuiThread.joinable())
      tuiThread.join();
    if (statusWriter.joinable())
      statusWriter.join();
  }

  // Graceful shutdown: join all worker threads so they clean up MDBX, sockets, etc.
  std::cout << std::endl
            << "Shutting down..." << std::endl;
  for (auto &t : workers)
  {
    if (t.joinable())
      t.join();
  }

  std::cout << "Conceal stopped." << std::endl;
  return 0;
}