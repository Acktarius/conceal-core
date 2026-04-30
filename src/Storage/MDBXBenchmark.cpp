#include "MDBXBlockchainStorage.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include <iostream>
#include <chrono>
#include <cassert>
#include <random>
#include <sys/stat.h>

using namespace CryptoNote;
using namespace std::chrono;

class Timer
{
  high_resolution_clock::time_point start;
  std::string name;

public:
  Timer(const std::string &n) : name(n), start(high_resolution_clock::now()) {}
  ~Timer()
  {
    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
    std::cout << "  " << name << ": " << elapsed << " ms" << std::endl;
  }
};

crypto::KeyImage makeKeyImage(uint32_t seed)
{
  crypto::KeyImage ki;
  memset(&ki, 0, 0);
  ki.data[0] = (seed >> 24) & 0xFF;
  ki.data[1] = (seed >> 16) & 0xFF;
  ki.data[2] = (seed >> 8) & 0xFF;
  ki.data[3] = seed & 0xFF;
  return ki;
}

// Get directory size for comparison with old file-based storage
uint64_t getDirSize(const std::string &path)
{
  uint64_t size = 0;
  // Simple: just check the data.mdb file
  std::string dbFile = path + "/data.mdb";
  struct stat st;
  if (stat(dbFile.c_str(), &st) == 0)
  {
    size = st.st_size;
  }
  return size;
}

int main()
{
  std::cout << "=== MDBX Blockchain Storage Benchmark ===" << std::endl;

  const std::string testDir = "./mdbx_benchmark_db";
  system(("rm -rf " + testDir).c_str());

  const uint32_t NUM_KEY_IMAGES = 2000000; // 2M spent keys (realistic for a blockchain)
  const uint32_t NUM_LOOKUPS = 500000;     // 500k random lookups

  MDBXBlockchainStorage storage(testDir);

  // ---- BENCHMARK 1: Bulk Key Image Writing ----
  // This simulates processing transactions during sync
  std::cout << "\n1. Writing " << NUM_KEY_IMAGES << " spent key images..." << std::endl;
  {
    auto start = high_resolution_clock::now();

    // Write in batches of 5000 (simulates block-by-block processing)
    const uint32_t BATCH_SIZE = 5000;
    for (uint32_t batch = 0; batch < NUM_KEY_IMAGES / BATCH_SIZE; ++batch)
    {
      for (uint32_t i = 0; i < BATCH_SIZE; ++i)
      {
        storage.markKeyImageSpent(makeKeyImage(batch * BATCH_SIZE + i));
      }
      storage.flush(); // Commit each batch like real block processing
    }

    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
    std::cout << "  Time: " << elapsed << " ms" << std::endl;
    std::cout << "  Rate: " << (NUM_KEY_IMAGES * 1000.0 / elapsed) << " key images/sec" << std::endl;
  }

  // ---- BENCHMARK 2: Key Image Lookups ----
  // This is the CRITICAL path during sync - validating each transaction input
  std::cout << "\n2. Random key image lookups (" << NUM_LOOKUPS << " queries)..." << std::endl;
  {
    auto start = high_resolution_clock::now();

    uint32_t foundExisting = 0;
    uint32_t foundMissing = 0;
    std::mt19937 rng(42);

    for (uint32_t i = 0; i < NUM_LOOKUPS; ++i)
    {
      uint32_t idx = rng() % (NUM_KEY_IMAGES * 2); // 50% hit rate
      if (storage.isSpentKeyImage(makeKeyImage(idx)))
      {
        foundExisting++;
      }
      else
      {
        foundMissing++;
      }
    }

    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
    std::cout << "  Time: " << elapsed << " ms" << std::endl;
    std::cout << "  Rate: " << (NUM_LOOKUPS * 1000.0 / elapsed) << " lookups/sec" << std::endl;
    std::cout << "  Existing: " << foundExisting << " | Missing: " << foundMissing << std::endl;
  }

  // ---- BENCHMARK 3: Height Operations ----
  std::cout << "\n3. Height tracking..." << std::endl;
  {
    auto start = high_resolution_clock::now();
    for (uint32_t i = 0; i < 100000; ++i)
    {
      storage.setTopBlockHeight(2000000 + i);
    }
    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
    std::cout << "  100k height updates: " << elapsed << " ms" << std::endl;
  }

  {
    auto start = high_resolution_clock::now();
    for (uint32_t i = 0; i < NUM_LOOKUPS; ++i)
    {
      volatile uint32_t h = storage.topBlockHeight();
      (void)h;
    }
    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
    std::cout << "  " << NUM_LOOKUPS << " height reads: " << elapsed << " ms" << std::endl;
  }

  // ---- BENCHMARK 4: Database Size ----
  std::cout << "\n4. Database size..." << std::endl;
  {
    uint64_t dbSize = getDirSize(testDir);
    std::cout << "  Database file size: " << (dbSize / (1024.0 * 1024.0)) << " MB" << std::endl;
    std::cout << "  Per key image: " << (dbSize / (double)NUM_KEY_IMAGES) << " bytes" << std::endl;
  }

  // ---- BENCHMARK 5: Restart Simulation ----
  std::cout << "\n5. Restart simulation (close & reopen)..." << std::endl;
  {
    auto start = high_resolution_clock::now();
    storage.close();
    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
    std::cout << "  Close: " << elapsed << " ms" << std::endl;
  }
  {
    auto start = high_resolution_clock::now();
    MDBXBlockchainStorage storage2(testDir);
    auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();

    // Verify data survived
    uint32_t height = storage2.topBlockHeight();
    bool hasKey = storage2.isSpentKeyImage(makeKeyImage(0));
    bool hasMidKey = storage2.isSpentKeyImage(makeKeyImage(NUM_KEY_IMAGES / 2));
    bool noFakeKey = !storage2.isSpentKeyImage(makeKeyImage(NUM_KEY_IMAGES * 3));

    std::cout << "  Reopen: " << elapsed << " ms" << std::endl;
    std::cout << "  Verified height: " << height << std::endl;
    std::cout << "  Key image integrity: first=" << hasKey
              << " mid=" << hasMidKey << " fake=" << noFakeKey << std::endl;

    storage2.close();
  }

  // Cleanup
  system(("rm -rf " + testDir).c_str());

  std::cout << "\n=== Benchmark Complete ===" << std::endl;
  std::cout << "\nComparison with current file-based storage:" << std::endl;
  std::cout << "  Current: Linear scan of flat files for key image checks" << std::endl;
  std::cout << "  MDBX:    B-tree index with O(log n) lookups" << std::endl;
  std::cout << "  Expected improvement: 100x-1000x faster for key image validation" << std::endl;

  return 0;
}