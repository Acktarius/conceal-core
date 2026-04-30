#include "MDBXBlockchainStorage.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include <iostream>
#include <cassert>

using namespace CryptoNote;
using namespace cn;

int main()
{
  std::cout << "Testing MDBX Blockchain Storage..." << std::endl;

  // Create test database
  const std::string testDir = "./mdbx_test_db";
  system(("rm -rf " + testDir).c_str());

  MDBXBlockchainStorage storage(testDir);

  // Test 1: Empty state
  assert(storage.topBlockHeight() == 0);
  std::cout << "✓ Empty database initialized" << std::endl;

  // Test 2: We need a genesis block to test with
  // For now, test the key image functionality
  crypto::KeyImage testKey;
  memset(&testKey, 0x42, sizeof(testKey));

  assert(!storage.isSpentKeyImage(testKey));
  std::cout << "✓ Key image not spent initially" << std::endl;

  storage.markKeyImageSpent(testKey);
  storage.flush();

  assert(storage.isSpentKeyImage(testKey));
  std::cout << "✓ Key image marked and verified" << std::endl;

  // Test 3: Height tracking
  storage.setTopBlockHeight(100);
  assert(storage.topBlockHeight() == 100);
  std::cout << "✓ Height tracking works" << std::endl;

  storage.close();

  // Test 4: Reopen and verify persistence
  MDBXBlockchainStorage storage2(testDir);
  assert(storage2.topBlockHeight() == 100);
  assert(storage2.isSpentKeyImage(testKey));
  std::cout << "✓ Data persists after close/reopen" << std::endl;

  storage2.close();

  // Cleanup
  system(("rm -rf " + testDir).c_str());

  std::cout << "\n✅ All MDBX tests passed!" << std::endl;
  return 0;
}