#include "Blockchain.h"

namespace std
{
  // Lexicographic comparison of two 32‑byte hashes (required for std::map ordering)
  bool operator<(const crypto::Hash &hash1, const crypto::Hash &hash2)
  {
    return memcmp(&hash1, &hash2, crypto::HASH_SIZE) < 0;
  }

  // Lexicographic comparison of two 32‑byte key images
  bool operator<(const crypto::KeyImage &keyImage1, const crypto::KeyImage &keyImage2)
  {
    return memcmp(&keyImage1, &keyImage2, 32) < 0;
  }
} // namespace std
