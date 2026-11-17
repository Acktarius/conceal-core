// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "Common/CommandLine.h"
#include "Common/StringTools.h"
#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/CryptoNoteBasicImpl.h"
#include "P2p/NetNodeConfig.h"
#include "Rpc/AiProviderRpcCommands.h"
#include "P2p/AiDiscoveryLimits.h"
#include "P2p/AiDiscoveryProtocolDefinitions.h"
#include "P2p/AiDiscoveryService.h"
#include "P2p/AiProviderRegistry.h"
#include "P2p/AiProviderValidation.h"
#include "P2p/LevinProtocol.h"
#include "P2p/P2pNetworks.h"
#include "Serialization/SerializationTools.h"
#include "crypto/crypto.h"

using namespace cn;

namespace {

const int64_t kNow = 1700000100;
const int64_t kIssuedAt = 1700000000;
const int64_t kExpiresAt = 1700001800;

// CryptoNote cn_fast_hash (Keccak-256) and generate_signature, not RFC 8032.
// Provider seed bytes are 0x01..0x20. Spend seed is 0x21..0x40. View seed is 0x41..0x60.
// The locked vector is contact type 2, per-second billing, and a non-zero SHA-256 digest.
const char kExpectedPublicKey[] = "ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99";
const char kExpectedAddress[] =
    "ccx7FaPyWkgHPaFMtGrZi14kuNBCfWmjoTrxyyTjfom1QLZY3QNnjPP67gpGMufB5E6kffR9rt3mfAqp4Nh4AbeJAP125xKvEP";
const char kExpectedCanonical[] =
    "19636f6e6365616c2d61692d70726f76696465722d61642d763101ff00a1b2c75f8277f512a336e7e3dd12"
    "ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed990100000000000000"
    "00f153650000000008f85365000000000102000000010000000400000064656d6f00100000e803000000000000"
    "0301808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f02620000006363783746615079576b67"
    "485061464d7447725a6931346b754e424366576d6a6f5472787979546a666f6d31514c5a5933514e6e6a505036376770474d"
    "7566423545366b666652397274336d66417170344e68344162654a4150313235784b7645501742858484895cddce7be599be"
    "37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07";
const char kExpectedHash[] = "25719326248403979c0f8f473717c87f082cfffd6d18259f58c7e21aed6f2541";
const char kExpectedSignature[] =
    "7c7934a88a9b07ed00fbb11cb8428d7e6c05af9eee12337a29b988fbddc10902d599aaf6d396c0dc86dbbdebbbee2a6104ecc38561560dc801638bdbb0a8d602";
const char kExpectedBindingCanonical[] =
    "1e636f6e6365616c2d61692d70726f76696465722d62696e64696e672d7631ff00a1b2c75f8277f512a336e7e3dd12"
    "ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99620000006363783746615079576b67"
    "485061464d7447725a6931346b754e424366576d6a6f5472787979546a666f6d31514c5a5933514e6e6a505036376770474d"
    "7566423545366b666652397274336d66417170344e68344162654a4150313235784b764550";
const char kExpectedBindingHash[] = "87e42684f8be5560b1364d26343a6a5bfad8c5b96b12f348d8f8d1593ae9c5e0";
const char kExpectedBindingSignature[] =
    "1742858484895cddce7be599be37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07";
const char kExpectedRegisterJson[] =
    "{\"advertisement\":{\"addressBindingSignature\":\"1742858484895cddce7be599be37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07\",\"availableSlots\":2,\"contactType\":2,\"contactValue\":\"ccx7FaPyWkgHPaFMtGrZi14kuNBCfWmjoTrxyyTjfom1QLZY3QNnjPP67gpGMufB5E6kffR9rt3mfAqp4Nh4AbeJAP125xKvEP\",\"expiresAt\":1700001800,\"issuedAt\":1700000000,\"models\":[{\"billingUnit\":3,\"maxContextTokens\":4096,\"modelDigest\":\"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f\",\"modelDigestType\":1,\"modelId\":\"demo\",\"priceAtomicUnits\":1000}],\"networkId\":\"ff00a1b2c75f8277f512a336e7e3dd12\",\"protocolVersion\":1,\"providerPublicKey\":\"ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99\",\"sequence\":1,\"signature\":\"7c7934a88a9b07ed00fbb11cb8428d7e6c05af9eee12337a29b988fbddc10902d599aaf6d396c0dc86dbbdebbbee2a6104ecc38561560dc801638bdbb0a8d602\",\"status\":1}}";
const char kExpectedGetJson[] =
    "{\"directoryComplete\":false,\"nextCursor\":1,\"pageDone\":true,\"providers\":[{\"advertisement\":{\"addressBindingSignature\":\"1742858484895cddce7be599be37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07\",\"availableSlots\":2,\"contactType\":2,\"contactValue\":\"ccx7FaPyWkgHPaFMtGrZi14kuNBCfWmjoTrxyyTjfom1QLZY3QNnjPP67gpGMufB5E6kffR9rt3mfAqp4Nh4AbeJAP125xKvEP\",\"expiresAt\":1700001800,\"issuedAt\":1700000000,\"models\":[{\"billingUnit\":3,\"maxContextTokens\":4096,\"modelDigest\":\"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f\",\"modelDigestType\":1,\"modelId\":\"demo\",\"priceAtomicUnits\":1000}],\"networkId\":\"ff00a1b2c75f8277f512a336e7e3dd12\",\"protocolVersion\":1,\"providerPublicKey\":\"ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99\",\"sequence\":1,\"signature\":\"7c7934a88a9b07ed00fbb11cb8428d7e6c05af9eee12337a29b988fbddc10902d599aaf6d396c0dc86dbbdebbbee2a6104ecc38561560dc801638bdbb0a8d602\",\"status\":1},\"observation\":{\"localFirstSeen\":1700000100,\"localLastSeen\":1700000100,\"localSource\":1}}],\"status\":\"OK\"}";

crypto::PublicKey publicFromSeed() {
  crypto::SecretKey seed;
  crypto::SecretKey secret;
  crypto::PublicKey pub;
  for (int i = 0; i < 32; ++i) {
    seed.data[i] = static_cast<uint8_t>(i + 1);
  }
  crypto::generate_keys_from_seed(pub, secret, seed);
  return pub;
}

crypto::SecretKey secretFromSeed() {
  crypto::SecretKey seed;
  crypto::SecretKey secret;
  crypto::PublicKey pub;
  for (int i = 0; i < 32; ++i) {
    seed.data[i] = static_cast<uint8_t>(i + 1);
  }
  crypto::generate_keys_from_seed(pub, secret, seed);
  return secret;
}

AiProviderAdvertisement makeAdvertisement(uint64_t sequence, uint8_t status, const crypto::PublicKey& pub) {
  AiProviderAdvertisement ad;
  ad.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  ad.networkId = CRYPTONOTE_NETWORK;
  ad.providerPublicKey = pub;
  ad.sequence = sequence;
  ad.issuedAt = kIssuedAt;
  ad.expiresAt = kExpiresAt;
  ad.status = status;
  ad.availableSlots = status == AI_STATUS_OFFLINE ? 0 : 2;
  if (status != AI_STATUS_OFFLINE) {
    AiModelOffer model;
    model.modelId = "demo";
    model.maxContextTokens = 4096;
    model.priceAtomicUnits = 1000;
    model.billingUnit = AI_BILLING_PER_REQUEST;
    ad.models.push_back(model);
  }
  ad.contactType = AI_CONTACT_ENDPOINT;
  ad.contactValue = "127.0.0.1:18080";
  return ad;
}

AiProviderAdvertisement signedAd(uint64_t sequence, uint8_t status, const crypto::PublicKey& pub, const crypto::SecretKey& secret) {
  AiProviderAdvertisement ad = makeAdvertisement(sequence, status, pub);
  EXPECT_TRUE(signAiAdvertisement(ad, secret));
  return ad;
}

crypto::PublicKey keyFromByte(uint8_t seedByte) {
  crypto::SecretKey seed;
  crypto::SecretKey secret;
  crypto::PublicKey pub;
  memset(seed.data, seedByte, sizeof(seed.data));
  crypto::generate_keys_from_seed(pub, secret, seed);
  return pub;
}

crypto::SecretKey secretFromByte(uint8_t seedByte) {
  crypto::SecretKey seed;
  crypto::SecretKey secret;
  crypto::PublicKey pub;
  memset(seed.data, seedByte, sizeof(seed.data));
  crypto::generate_keys_from_seed(pub, secret, seed);
  return secret;
}

void keysFromSeedByte(uint8_t first, crypto::PublicKey& pub, crypto::SecretKey& secret) {
  crypto::SecretKey seed;
  memset(seed.data, 0, sizeof(seed.data));
  for (int i = 0; i < 32; ++i) {
    seed.data[i] = static_cast<uint8_t>(first + i);
  }
  crypto::generate_keys_from_seed(pub, secret, seed);
}

struct TestWallet {
  AccountPublicAddress address;
  crypto::SecretKey spendSecret;
  std::string text;
};

TestWallet makeTestWallet() {
  TestWallet wallet;
  crypto::PublicKey viewPublic;
  crypto::SecretKey viewSecret;
  keysFromSeedByte(0x21, wallet.address.spendPublicKey, wallet.spendSecret);
  keysFromSeedByte(0x41, viewPublic, viewSecret);
  wallet.address.viewPublicKey = viewPublic;
  wallet.text = getAccountAddressAsStr(parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX, wallet.address);
  return wallet;
}

void setDigest(AiModelOffer& model, uint8_t type) {
  model.modelDigestType = type;
  model.modelDigest.fill(0);
  if (type == AI_DIGEST_SHA256) {
    for (size_t i = 0; i < model.modelDigest.size(); ++i) {
      model.modelDigest[i] = static_cast<uint8_t>(0x80 + i);
    }
  }
}

AiProviderAdvertisement makeBoundAdvertisement(const crypto::PublicKey& provider, const crypto::SecretKey& providerSecret,
                                               const TestWallet& wallet) {
  AiProviderAdvertisement ad = makeAdvertisement(1, AI_STATUS_AVAILABLE, provider);
  ad.models[0].billingUnit = AI_BILLING_PER_SECOND;
  setDigest(ad.models[0], AI_DIGEST_SHA256);
  ad.contactType = AI_CONTACT_CCX_ADDRESS;
  ad.contactValue = wallet.text;
  EXPECT_TRUE(signAiAddressBinding(ad.networkId, ad.providerPublicKey, ad.contactValue, wallet.address.spendPublicKey,
                                   wallet.spendSecret, ad.addressBindingSignature));
  EXPECT_TRUE(signAiAdvertisement(ad, providerSecret));
  return ad;
}

crypto::PublicKey keyFromIndex(uint16_t index) {
  crypto::SecretKey seed;
  crypto::SecretKey secret;
  crypto::PublicKey pub;
  memset(seed.data, 0, sizeof(seed.data));
  seed.data[0] = static_cast<uint8_t>(index);
  seed.data[1] = static_cast<uint8_t>(index >> 8);
  seed.data[2] = 0xa5;
  crypto::generate_keys_from_seed(pub, secret, seed);
  return pub;
}

crypto::SecretKey secretFromIndex(uint16_t index) {
  crypto::SecretKey seed;
  crypto::SecretKey secret;
  crypto::PublicKey pub;
  memset(seed.data, 0, sizeof(seed.data));
  seed.data[0] = static_cast<uint8_t>(index);
  seed.data[1] = static_cast<uint8_t>(index >> 8);
  seed.data[2] = 0xa5;
  crypto::generate_keys_from_seed(pub, secret, seed);
  return secret;
}

}

TEST(AiProviderDiscovery, SigningVectorAndRoundTrip) {
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  const TestWallet wallet = makeTestWallet();
  EXPECT_EQ(kExpectedAddress, wallet.text);
  AiProviderAdvertisement ad = makeAdvertisement(1, AI_STATUS_AVAILABLE, pub);
  ad.models[0].billingUnit = AI_BILLING_PER_SECOND;
  setDigest(ad.models[0], AI_DIGEST_SHA256);
  ad.contactType = AI_CONTACT_CCX_ADDRESS;
  ad.contactValue = wallet.text;
  ASSERT_TRUE(common::podFromHex(kExpectedBindingSignature, ad.addressBindingSignature));
  ASSERT_TRUE(common::podFromHex(kExpectedSignature, ad.signature));

  std::vector<uint8_t> canonical;
  ASSERT_TRUE(buildAiAdvertisementCanonical(ad, canonical));
  const crypto::Hash hash = aiAdvertisementSigningHash(canonical);
  std::vector<uint8_t> bindingCanonical;
  ASSERT_TRUE(buildAiAddressBindingCanonical(ad.networkId, ad.providerPublicKey, ad.contactValue, bindingCanonical));
  const crypto::Hash bindingHash = crypto::cn_fast_hash(bindingCanonical.data(), bindingCanonical.size());

  EXPECT_EQ(std::string("conceal-ai-provider-ad-v1"), std::string(reinterpret_cast<const char*>(&canonical[1]), canonical[0]));
  EXPECT_EQ(kExpectedPublicKey, common::podToHex(pub));
  EXPECT_EQ(kExpectedCanonical, common::toHex(canonical));
  EXPECT_EQ(kExpectedHash, common::podToHex(hash));
  EXPECT_EQ(kExpectedBindingCanonical, common::toHex(bindingCanonical));
  EXPECT_EQ(kExpectedBindingHash, common::podToHex(bindingHash));
  EXPECT_TRUE(verifyAiAddressBinding(ad.networkId, ad.providerPublicKey, ad.contactValue, ad.addressBindingSignature));
  std::vector<uint8_t> again;
  EXPECT_TRUE(verifyAiAdvertisementSignature(ad, again));
  EXPECT_EQ(canonical, again);

  AiProviderAdvertisement fresh = makeBoundAdvertisement(pub, secret, wallet);
  std::vector<uint8_t> freshCanonical;
  EXPECT_TRUE(verifyAiAdvertisementSignature(fresh, freshCanonical));
  EXPECT_TRUE(verifyAiAddressBinding(fresh.networkId, fresh.providerPublicKey, fresh.contactValue, fresh.addressBindingSignature));

  COMMAND_RPC_REGISTER_AI_PROVIDER::request registerRequest;
  registerRequest.advertisement = ad;
  COMMAND_RPC_REGISTER_AI_PROVIDER::response registerResponse;
  registerResponse.status = "accepted";
  registerResponse.reason = "accepted";
  AiProviderRegistry registry(CRYPTONOTE_NETWORK);
  ASSERT_EQ(AiAcceptResult::accepted, registry.accept(ad, kNow, AI_SOURCE_LOCAL).result);
  COMMAND_RPC_GET_AI_PROVIDERS::request getRequest;
  getRequest.limit = 8;
  COMMAND_RPC_GET_AI_PROVIDERS::response getResponse;
  getResponse.status = "OK";
  AiProviderPage listed = registry.query(AiProviderQuery(), kNow);
  getResponse.providers = listed.providers;
  getResponse.nextCursor = listed.nextCursor;
  getResponse.pageDone = listed.done;
  getResponse.directoryComplete = false;
  EXPECT_EQ(kExpectedRegisterJson, storeToJson(registerRequest));
  EXPECT_EQ("{\"reason\":\"accepted\",\"status\":\"accepted\"}", storeToJson(registerResponse));
  EXPECT_EQ("{\"billingUnit\":0,\"cursor\":0,\"limit\":8,\"minContextTokens\":0,\"modelId\":\"\",\"status\":0}", storeToJson(getRequest));
  EXPECT_EQ(kExpectedGetJson, storeToJson(getResponse));

  const BinaryArray encoded = LevinProtocol::encode(ad);
  AiProviderAdvertisement decoded;
  ASSERT_TRUE(LevinProtocol::decode(encoded, decoded));
  EXPECT_EQ(ad.sequence, decoded.sequence);
  EXPECT_EQ(ad.models[0].modelId, decoded.models[0].modelId);
  EXPECT_EQ(ad.models[0].priceAtomicUnits, decoded.models[0].priceAtomicUnits);
  EXPECT_EQ(ad.models[0].billingUnit, decoded.models[0].billingUnit);
  EXPECT_EQ(ad.models[0].modelDigestType, decoded.models[0].modelDigestType);
  EXPECT_EQ(0, memcmp(ad.models[0].modelDigest.data(), decoded.models[0].modelDigest.data(), ad.models[0].modelDigest.size()));
  EXPECT_EQ(ad.contactValue, decoded.contactValue);
  EXPECT_EQ(0, memcmp(ad.addressBindingSignature.data, decoded.addressBindingSignature.data, sizeof(ad.addressBindingSignature.data)));
  EXPECT_EQ(0, memcmp(ad.signature.data, decoded.signature.data, sizeof(ad.signature.data)));
  std::vector<uint8_t> decodedCanonical;
  EXPECT_TRUE(verifyAiAdvertisementSignature(decoded, decodedCanonical));
}

TEST(AiProviderDiscovery, RejectsModifiedFieldInvalidSignatureWrongNetworkAndTime) {
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  AiProviderAdvertisement ad = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);

  ad.availableSlots = 3;
  std::vector<uint8_t> canonical;
  EXPECT_FALSE(verifyAiAdvertisementSignature(ad, canonical));

  ad = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  ad.signature.data[0] ^= 0xff;
  AiProviderRegistry registry(CRYPTONOTE_NETWORK);
  AiIngestResult badSig = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, badSig.result);

  ad = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  ad.networkId.data[0] ^= 0xff;
  AiIngestResult wrongNet = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, wrongNet.result);
  EXPECT_EQ("wrong network", wrongNet.reason);

  ad = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  ad.issuedAt = kNow + AI_MAX_CLOCK_SKEW_SECONDS + 5;
  ad.expiresAt = ad.issuedAt + 60;
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  AiIngestResult future = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, future.result);
  EXPECT_EQ("issued too far in the future", future.reason);

  ad = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  ad.issuedAt = kNow - 120;
  ad.expiresAt = kNow;
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  AiIngestResult expired = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, expired.result);
  EXPECT_EQ("expired", expired.reason);
}

TEST(AiProviderDiscovery, SequenceDuplicateConflictOfflineAndCapacity) {
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  AiProviderRegistry registry(CRYPTONOTE_NETWORK);

  AiProviderAdvertisement available = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  AiIngestResult first = registry.accept(available, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::accepted, first.result);
  EXPECT_TRUE(first.relay);

  AiIngestResult duplicate = registry.accept(available, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::duplicate, duplicate.result);
  EXPECT_FALSE(duplicate.relay);

  AiProviderAdvertisement conflict = available;
  conflict.availableSlots = 4;
  ASSERT_TRUE(signAiAdvertisement(conflict, secret));
  AiIngestResult conflicted = registry.accept(conflict, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, conflicted.result);
  EXPECT_FALSE(conflicted.relay);

  AiProviderAdvertisement busy = signedAd(2, AI_STATUS_BUSY, pub, secret);
  AiIngestResult busyResult = registry.accept(busy, kNow, AI_SOURCE_PEER);
  EXPECT_EQ(AiAcceptResult::accepted, busyResult.result);

  AiIngestResult stale = registry.accept(available, kNow, AI_SOURCE_PEER);
  EXPECT_EQ(AiAcceptResult::stale, stale.result);
  EXPECT_FALSE(stale.relay);

  AiProviderAdvertisement offline = signedAd(3, AI_STATUS_OFFLINE, pub, secret);
  EXPECT_EQ(AiAcceptResult::accepted, registry.accept(offline, kNow, AI_SOURCE_LOCAL).result);
  AiProviderQuery query;
  AiProviderPage hidden = registry.query(query, kNow);
  EXPECT_TRUE(hidden.providers.empty());
  query.status = AI_STATUS_OFFLINE;
  AiProviderPage shown = registry.query(query, kNow);
  ASSERT_EQ(1u, shown.providers.size());
  EXPECT_EQ(AI_SOURCE_LOCAL, shown.providers[0].observation.source);
  EXPECT_EQ(AiAcceptResult::stale, registry.accept(available, kNow, AI_SOURCE_PEER).result);

  AiProviderRegistry small(CRYPTONOTE_NETWORK, 2);
  for (uint8_t n = 1; n <= 2; ++n) {
    AiProviderAdvertisement ad = signedAd(1, AI_STATUS_AVAILABLE, keyFromByte(n), secretFromByte(n));
    EXPECT_EQ(AiAcceptResult::accepted, small.accept(ad, kNow, AI_SOURCE_LOCAL).result);
  }
  AiProviderAdvertisement third = signedAd(1, AI_STATUS_AVAILABLE, keyFromByte(3), secretFromByte(3));
  EXPECT_EQ(AiAcceptResult::capacityRejected, small.accept(third, kNow, AI_SOURCE_LOCAL).result);
  EXPECT_EQ(2u, small.size());
}

TEST(AiProviderDiscovery, PaginationExpiryAndDecodeLimits) {
  AiProviderRegistry registry(CRYPTONOTE_NETWORK);
  for (uint8_t n = 1; n <= 10; ++n) {
    AiProviderAdvertisement ad = signedAd(1, AI_STATUS_AVAILABLE, keyFromByte(n), secretFromByte(n));
    ad.models[0].modelId = n % 2 == 0 ? "even" : "odd";
    ad.models[0].maxContextTokens = n < 5 ? 1000 : 8000;
    ad.models[0].billingUnit = n % 2 == 0 ? AI_BILLING_PER_THOUSAND_TOKENS : AI_BILLING_PER_REQUEST;
    ASSERT_TRUE(signAiAdvertisement(ad, secretFromByte(n)));
    ASSERT_EQ(AiAcceptResult::accepted, registry.accept(ad, kNow, AI_SOURCE_LOCAL).result);
  }

  AiProviderQuery query;
  query.limit = 8;
  AiProviderPage first = registry.query(query, kNow);
  EXPECT_EQ(8u, first.providers.size());
  EXPECT_FALSE(first.done);
  query.cursor = first.nextCursor;
  AiProviderPage second = registry.query(query, kNow);
  EXPECT_EQ(2u, second.providers.size());
  EXPECT_TRUE(second.done);

  query.cursor = 0;
  query.modelId = "even";
  query.minContextTokens = 8000;
  query.billingUnit = AI_BILLING_PER_THOUSAND_TOKENS;
  AiProviderPage filtered = registry.query(query, kNow);
  EXPECT_FALSE(filtered.providers.empty());
  for (size_t i = 0; i < filtered.providers.size(); ++i) {
    EXPECT_EQ("even", filtered.providers[i].advertisement.models[0].modelId);
  }

  registry.expire(kExpiresAt);
  EXPECT_EQ(0u, registry.size());

  AiProviderAdvertisement tooMany = makeAdvertisement(1, AI_STATUS_AVAILABLE, publicFromSeed());
  tooMany.models.clear();
  for (int i = 0; i < 9; ++i) {
    AiModelOffer model;
    model.modelId = std::string(1, static_cast<char>('a' + i));
    model.maxContextTokens = 10;
    model.priceAtomicUnits = 1;
    model.billingUnit = AI_BILLING_PER_REQUEST;
    tooMany.models.push_back(model);
  }
  EXPECT_THROW(LevinProtocol::encode(tooMany), std::runtime_error);

  AiModelOffer offer;
  EXPECT_FALSE(loadFromJson(offer, "{\"modelId\":\"" + std::string(AI_MAX_MODEL_ID_LENGTH + 1, 'z') +
                                       "\",\"maxContextTokens\":1,\"priceAtomicUnits\":1,\"billingUnit\":1,"
                                       "\"modelDigestType\":0,\"modelDigest\":\"" +
                                       std::string(AI_MODEL_DIGEST_BYTES * 2, '0') + "\"}"));
}

TEST(AiProviderDiscovery, RateLimitsMaintenanceAndAccess) {
  AiRateWindow window;
  EXPECT_TRUE(window.allowMany(kNow, 2, 2, 60));
  EXPECT_FALSE(window.allow(kNow, 2, 60));
  EXPECT_TRUE(window.allow(kNow + 60, 2, 60));

  EXPECT_FALSE(aiDiscoveryEnqueueAllowed(0, AI_DISCOVERY_MAX_MESSAGE_BYTES + 1, 0));
  EXPECT_FALSE(aiDiscoveryEnqueueAllowed(AI_DISCOVERY_MAX_QUEUE_BYTES, 1, 0));
  EXPECT_FALSE(aiDiscoveryEnqueueAllowed(0, 16, AI_DISCOVERY_YIELD_QUEUE_BYTES));
  EXPECT_TRUE(aiDiscoveryEnqueueAllowed(0, 16, 0));
  EXPECT_TRUE(aiDiscoveryBodyExceedsCap(AI_DISCOVERY_CAPABILITIES_ID, AI_DISCOVERY_MAX_MESSAGE_BYTES + 1));
  EXPECT_FALSE(aiDiscoveryBodyExceedsCap(1001, AI_DISCOVERY_MAX_MESSAGE_BYTES + 1));
  EXPECT_FALSE(aiDiscoveryBodyExceedsCap(AI_DISCOVERY_CAPABILITIES_ID, AI_DISCOVERY_MAX_MESSAGE_BYTES));

  EXPECT_EQ(aiRegistrationDisabled, aiRegistrationAccess(false, 0x7f000001u));
  EXPECT_EQ(aiRegistrationForbidden, aiRegistrationAccess(true, 0x08080808u));
  EXPECT_EQ(aiRegistrationForbidden, aiRegistrationAccess(true, 0));
  EXPECT_EQ(aiRegistrationAllowed, aiRegistrationAccess(true, 0x7f000001u));
  EXPECT_TRUE(isLoopbackIpv4HostOrder(0x7f010203u));

  EXPECT_NE(AI_DISCOVERY_CAPABILITIES_ID, 1001u);
  EXPECT_NE(AI_DISCOVERY_ADVERTISEMENTS_ID, 1002u);
  EXPECT_NE(AI_DISCOVERY_REQUEST_PROVIDERS_ID, 1003u);
  EXPECT_GT(AI_DISCOVERY_CAPABILITIES_ID, 2010u);

  AiDiscoveryService service;
  service.setNetworkId(CRYPTONOTE_NETWORK);
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  AiIngestResult disabled = service.registerLocal(signedAd(1, AI_STATUS_AVAILABLE, pub, secret), kNow);
  EXPECT_EQ(AiAcceptResult::invalid, disabled.result);

  service.setEnabled(true);
  for (uint32_t n = 1; n <= AI_MAX_LOCAL_REGISTRATIONS_PER_MINUTE; ++n) {
    AiProviderAdvertisement ad = signedAd(n, AI_STATUS_AVAILABLE, pub, secret);
    ad.signature.data[0] ^= 0xff;
    EXPECT_EQ(AiAcceptResult::invalid, service.registerLocal(ad, kNow).result);
  }
  AiProviderAdvertisement limited = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  EXPECT_EQ(AiAcceptResult::rateLimited, service.registerLocal(limited, kNow).result);
  EXPECT_EQ(0u, service.getProviders(AiProviderQuery(), kNow).providers.size());
}

TEST(AiProviderDiscovery, RelaySyncLegacyAndShutdown) {
  AiDiscoveryService daemonA;
  AiDiscoveryService daemonB;
  daemonA.setEnabled(true);
  daemonB.setEnabled(true);
  daemonA.setNetworkId(CRYPTONOTE_NETWORK);
  daemonB.setNetworkId(CRYPTONOTE_NETWORK);

  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  AiProviderAdvertisement available = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  AiIngestResult registered = daemonA.registerLocal(available, kNow);
  ASSERT_EQ(AiAcceptResult::accepted, registered.result);
  ASSERT_TRUE(registered.relay);

  NOTIFY_AI_PROVIDER_ADVERTISEMENTS::request notify;
  notify.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  notify.advertisements.push_back(available);
  const BinaryArray notifyBody = LevinProtocol::encode(notify);

  AiConnectionState unsupported;
  unsupported.capability = AI_PEER_UNSUPPORTED;
  EXPECT_FALSE(daemonA.shouldRelayTo(unsupported));

  AiConnectionState peerOnB;
  peerOnB.capability = AI_PEER_SUPPORTED;
  daemonB.handleAdvertisements(notifyBody, peerOnB, kNow, AiRelayFn());
  AiProviderQuery query;
  AiProviderPage listed = daemonB.getProviders(query, kNow);
  ASSERT_EQ(1u, listed.providers.size());
  EXPECT_EQ(AI_STATUS_AVAILABLE, listed.providers[0].advertisement.status);
  EXPECT_EQ(AI_SOURCE_PEER, listed.providers[0].observation.source);
  EXPECT_FALSE(listed.providers[0].advertisement.models.empty());

  AiProviderAdvertisement busy = signedAd(2, AI_STATUS_BUSY, pub, secret);
  ASSERT_TRUE(daemonA.registerLocal(busy, kNow).relay);
  notify.advertisements.clear();
  notify.advertisements.push_back(busy);
  daemonB.handleAdvertisements(LevinProtocol::encode(notify), peerOnB, kNow, AiRelayFn());
  listed = daemonB.getProviders(query, kNow);
  ASSERT_EQ(1u, listed.providers.size());
  EXPECT_EQ(AI_STATUS_BUSY, listed.providers[0].advertisement.status);

  daemonB.handleAdvertisements(notifyBody, peerOnB, kNow, AiRelayFn());
  listed = daemonB.getProviders(query, kNow);
  EXPECT_EQ(AI_STATUS_BUSY, listed.providers[0].advertisement.status);

  EXPECT_TRUE(daemonB.getProviders(query, kExpiresAt).providers.empty());

  std::vector<AiOutbound> outbox;
  AiConnectionState legacy;
  AiPeerRef ref;
  ref.state = &legacy;
  ref.handshakeComplete = true;
  ref.enqueue = [&outbox](const AiOutbound& message) {
    outbox.push_back(message);
    return true;
  };
  std::vector<AiPeerRef> peers(1, ref);
  daemonA.maintain(kNow, peers);
  ASSERT_EQ(1u, outbox.size());
  EXPECT_EQ(static_cast<uint32_t>(AI_DISCOVERY_CAPABILITIES_ID), outbox[0].command);
  EXPECT_TRUE(outbox[0].request);
  EXPECT_EQ(AI_PEER_PROBE_SENT, legacy.capability);

  daemonA.maintain(kNow + 1, peers);
  EXPECT_EQ(1u, outbox.size());

  daemonA.handleCapabilityResponse(BinaryArray(), legacy, kNow + 2, ref.enqueue);
  EXPECT_EQ(AI_PEER_UNSUPPORTED, legacy.capability);
  daemonA.maintain(kNow + 3, peers);
  EXPECT_EQ(1u, outbox.size());

  AiConnectionState syncing;
  syncing.capability = AI_PEER_SUPPORTED;
  syncing.nextSyncAt = kNow;
  std::vector<AiOutbound> syncOut;
  AiEnqueueFn syncEnqueue = [&syncOut](const AiOutbound& message) {
    syncOut.push_back(message);
    return true;
  };
  ASSERT_TRUE(daemonB.requestAiProviderPage(syncing, kNow, syncEnqueue));
  ASSERT_EQ(1u, syncOut.size());
  BinaryArray response;
  AiConnectionState serverState;
  ASSERT_EQ(LEVIN_PROTOCOL_RETCODE_SUCCESS, daemonA.handleProvidersRequest(syncOut[0].body, response, serverState, kNow));
  daemonB.handleProvidersResponse(response, syncing, kNow, syncEnqueue, AiRelayFn());
  EXPECT_FALSE(syncing.hasOutstanding);

  syncing.hasOutstanding = true;
  syncing.outstandingRequestId = 99;
  syncing.outstandingDeadline = kNow + 5;
  COMMAND_REQUEST_AI_PROVIDERS::response mismatched;
  mismatched.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  mismatched.requestId = 100;
  mismatched.result = AI_DIRECTORY_OK;
  mismatched.done = true;
  daemonB.handleProvidersResponse(LevinProtocol::encode(mismatched), syncing, kNow, syncEnqueue, AiRelayFn());
  EXPECT_TRUE(syncing.hasOutstanding);
  EXPECT_EQ(99u, syncing.outstandingRequestId);

  daemonB.handleProvidersResponse(BinaryArray(), syncing, kNow, syncEnqueue, AiRelayFn());
  EXPECT_FALSE(syncing.hasOutstanding);

  AiConnectionState pending;
  pending.capability = AI_PEER_SUPPORTED;
  pending.hasOutstanding = true;
  pending.outstandingRequestId = 7;
  pending.outstandingDeadline = kNow + 100;
  daemonB.onDisconnect(pending);
  EXPECT_FALSE(pending.hasOutstanding);

  daemonB.shutdown();
  AiConnectionState afterStop;
  afterStop.capability = AI_PEER_SUPPORTED;
  afterStop.nextSyncAt = kNow;
  EXPECT_FALSE(daemonB.requestAiProviderPage(afterStop, kNow, syncEnqueue));
  EXPECT_TRUE(daemonB.stopped());
}

namespace {

bool discoveryEnabled(bool commandLineFlag, const std::string& configText) {
  boost::program_options::options_description desc;
  command_line::add_arg(desc, command_line::arg_testnet_on);
  NetNodeConfig::initOptions(desc);

  const char* argvOn[] = {"conceal-daemon", "--enable-ai-discovery"};
  const char* argvOff[] = {"conceal-daemon"};
  boost::program_options::variables_map vm;
  if (commandLineFlag) {
    boost::program_options::store(boost::program_options::parse_command_line(2, argvOn, desc), vm);
  } else {
    boost::program_options::store(boost::program_options::parse_command_line(1, argvOff, desc), vm);
  }
  if (!configText.empty()) {
    std::stringstream input(configText);
    boost::program_options::store(boost::program_options::parse_config_file(input, desc), vm);
  }
  boost::program_options::notify(vm);

  NetNodeConfig config;
  EXPECT_TRUE(config.init(vm));
  return config.getEnableAiDiscovery();
}

}

TEST(AiProviderDiscovery, EnablesFromCommandLineOrConfigFile) {
  EXPECT_FALSE(discoveryEnabled(false, ""));
  EXPECT_TRUE(discoveryEnabled(true, ""));
  EXPECT_TRUE(discoveryEnabled(false, "enable-ai-discovery=TRUE\n"));
  EXPECT_TRUE(discoveryEnabled(false, "enable-ai-discovery=true\n"));
  EXPECT_FALSE(discoveryEnabled(false, "enable-ai-discovery=false\n"));
  EXPECT_TRUE(discoveryEnabled(true, "enable-ai-discovery=false\n"));
}

TEST(AiProviderDiscovery, AddressBindingDigestAndPerSecondBilling) {
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  const TestWallet wallet = makeTestWallet();
  AiProviderRegistry registry(CRYPTONOTE_NETWORK);
  resetAiVerificationStats();
  AiProviderAdvertisement ad = makeBoundAdvertisement(pub, secret, wallet);
  AiIngestResult accepted = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::accepted, accepted.result);
  EXPECT_EQ(1u, aiVerificationStats().providerSignatures);
  EXPECT_EQ(1u, aiVerificationStats().bindingSignatures);

  AiProviderQuery query;
  query.billingUnit = AI_BILLING_PER_SECOND;
  AiProviderPage page = registry.query(query, kNow);
  ASSERT_EQ(1u, page.providers.size());
  EXPECT_EQ(AI_BILLING_PER_SECOND, page.providers[0].advertisement.models[0].billingUnit);

  ad.contactValue = "not-an-address";
  memset(ad.addressBindingSignature.data, 1, sizeof(ad.addressBindingSignature.data));
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  AiIngestResult malformed = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, malformed.result);
  EXPECT_EQ("malformed ccx address", malformed.reason);

  AccountPublicAddress parsed = wallet.address;
  ad.contactValue = getAccountAddressAsStr(1, parsed);
  ASSERT_TRUE(signAiAddressBinding(ad.networkId, pub, ad.contactValue, wallet.address.spendPublicKey, wallet.spendSecret,
                                   ad.addressBindingSignature));
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  AiIngestResult wrongPrefix = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ("wrong address prefix", wrongPrefix.reason);

  ad = makeBoundAdvertisement(pub, secret, wallet);
  memset(ad.addressBindingSignature.data, 0, sizeof(ad.addressBindingSignature.data));
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  resetAiVerificationStats();
  AiIngestResult missing = registry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ("missing address binding", missing.reason);
  EXPECT_EQ(0u, aiVerificationStats().providerSignatures);
  EXPECT_EQ(0u, aiVerificationStats().bindingSignatures);

  crypto::PublicKey otherSpend;
  crypto::SecretKey otherSecret;
  keysFromSeedByte(0x61, otherSpend, otherSecret);
  ad = makeBoundAdvertisement(pub, secret, wallet);
  ASSERT_TRUE(signAiAddressBinding(ad.networkId, pub, ad.contactValue, otherSpend, otherSecret, ad.addressBindingSignature));
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  resetAiVerificationStats();
  AiProviderRegistry wrongKeyRegistry(CRYPTONOTE_NETWORK);
  AiIngestResult wrongKey = wrongKeyRegistry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, wrongKey.result);
  EXPECT_EQ("invalid address binding", wrongKey.reason);
  EXPECT_EQ(1u, aiVerificationStats().providerSignatures);
  EXPECT_EQ(1u, aiVerificationStats().bindingSignatures);

  ad = makeBoundAdvertisement(pub, secret, wallet);
  boost::uuids::uuid otherNetwork = ad.networkId;
  otherNetwork.data[0] ^= 0xff;
  ASSERT_TRUE(signAiAddressBinding(otherNetwork, pub, ad.contactValue, wallet.address.spendPublicKey, wallet.spendSecret,
                                   ad.addressBindingSignature));
  ad.networkId = CRYPTONOTE_NETWORK;
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  AiProviderRegistry wrongNetworkRegistry(CRYPTONOTE_NETWORK);
  AiIngestResult wrongNetworkBinding = wrongNetworkRegistry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ("invalid address binding", wrongNetworkBinding.reason);

  ad = makeBoundAdvertisement(pub, secret, wallet);
  ad.models.push_back(ad.models[0]);
  ad.models[1].modelDigest[0] ^= 0xff;
  AiProviderRegistry sameIdRegistry(CRYPTONOTE_NETWORK);
  AiIngestResult sameId = sameIdRegistry.accept(ad, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ("models are not strictly ordered", sameId.reason);

  ad = makeBoundAdvertisement(pub, secret, wallet);
  ad.models[0].modelDigestType = 9;
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  AiProviderRegistry digestRegistry(CRYPTONOTE_NETWORK);
  EXPECT_EQ("unknown model digest type", digestRegistry.accept(ad, kNow, AI_SOURCE_LOCAL).reason);

  ad = makeBoundAdvertisement(pub, secret, wallet);
  ad.sequence = 2;
  ad.models[0].modelDigestType = AI_DIGEST_NONE;
  ad.models[0].modelDigest.fill(0);
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  EXPECT_EQ(AiAcceptResult::accepted, registry.accept(ad, kNow, AI_SOURCE_PEER).result);

  ad = signedAd(4, AI_STATUS_AVAILABLE, pub, secret);
  ad.models[0].billingUnit = 9;
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  EXPECT_EQ("unknown billing unit", registry.accept(ad, kNow, AI_SOURCE_LOCAL).reason);

  ad = signedAd(5, AI_STATUS_AVAILABLE, pub, secret);
  ad.models[0].modelDigestType = AI_DIGEST_NONE;
  ad.models[0].modelDigest[0] = 1;
  ASSERT_TRUE(signAiAdvertisement(ad, secret));
  EXPECT_EQ("empty digest type has a digest", registry.accept(ad, kNow, AI_SOURCE_LOCAL).reason);
}

TEST(AiProviderDiscovery, StaleAndDuplicateSkipSignatureBudget) {
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  AiDiscoveryService service;
  service.setEnabled(true);
  service.setNetworkId(CRYPTONOTE_NETWORK);
  AiProviderAdvertisement ad = signedAd(2, AI_STATUS_AVAILABLE, pub, secret);

  AiConnectionState state;
  NOTIFY_AI_PROVIDER_ADVERTISEMENTS::request notify;
  notify.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  notify.advertisements.push_back(ad);
  resetAiVerificationStats();
  service.handleAdvertisements(LevinProtocol::encode(notify), state, kNow, AiRelayFn());
  EXPECT_EQ(1u, aiVerificationStats().providerSignatures);
  EXPECT_EQ(0u, aiVerificationStats().bindingSignatures);
  EXPECT_EQ(1u, state.signaturesInWindow);
  EXPECT_EQ(1u, state.updatesInWindow);

  resetAiVerificationStats();
  service.handleAdvertisements(LevinProtocol::encode(notify), state, kNow, AiRelayFn());
  EXPECT_EQ(0u, aiVerificationStats().providerSignatures);
  EXPECT_EQ(1u, state.signaturesInWindow);
  EXPECT_EQ(2u, state.updatesInWindow);

  AiProviderAdvertisement older = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  notify.advertisements.clear();
  notify.advertisements.push_back(older);
  resetAiVerificationStats();
  service.handleAdvertisements(LevinProtocol::encode(notify), state, kNow, AiRelayFn());
  EXPECT_EQ(0u, aiVerificationStats().providerSignatures);
  EXPECT_EQ(1u, state.signaturesInWindow);
  EXPECT_EQ(3u, state.updatesInWindow);
  EXPECT_EQ(AI_STATUS_AVAILABLE, service.getProviders(AiProviderQuery(), kNow).providers[0].advertisement.status);
  EXPECT_EQ(2u, service.getProviders(AiProviderQuery(), kNow).providers[0].advertisement.sequence);

  AiProviderAdvertisement conflict = ad;
  conflict.availableSlots = 9;
  ASSERT_TRUE(signAiAdvertisement(conflict, secret));
  resetAiVerificationStats();
  AiProviderRegistry registry(CRYPTONOTE_NETWORK);
  ASSERT_EQ(AiAcceptResult::accepted, registry.accept(ad, kNow, AI_SOURCE_LOCAL).result);
  resetAiVerificationStats();
  AiIngestResult conflicted = registry.accept(conflict, kNow, AI_SOURCE_LOCAL);
  EXPECT_EQ(AiAcceptResult::invalid, conflicted.result);
  EXPECT_EQ("conflicting record at the same sequence", conflicted.reason);
  EXPECT_EQ(0u, aiVerificationStats().providerSignatures);
}

TEST(AiProviderDiscovery, ExpiredRecordInBatchIsSkipped) {
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  const crypto::PublicKey other = keyFromByte(4);
  const crypto::SecretKey otherSecret = secretFromByte(4);
  AiProviderAdvertisement expired = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);
  expired.issuedAt = kNow - 120;
  expired.expiresAt = kNow;
  ASSERT_TRUE(signAiAdvertisement(expired, secret));
  AiProviderAdvertisement valid = signedAd(1, AI_STATUS_AVAILABLE, other, otherSecret);

  AiDiscoveryService service;
  service.setEnabled(true);
  service.setNetworkId(CRYPTONOTE_NETWORK);
  NOTIFY_AI_PROVIDER_ADVERTISEMENTS::request notify;
  notify.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  notify.advertisements.push_back(expired);
  notify.advertisements.push_back(valid);
  AiConnectionState state;
  state.capability = AI_PEER_SUPPORTED;
  state.nextSyncAt = kNow;
  service.handleAdvertisements(LevinProtocol::encode(notify), state, kNow, AiRelayFn());
  AiProviderPage page = service.getProviders(AiProviderQuery(), kNow);
  ASSERT_EQ(1u, page.providers.size());
  EXPECT_EQ(0, memcmp(other.data, page.providers[0].advertisement.providerPublicKey.data, sizeof(other.data)));
  EXPECT_EQ(kNow, state.nextSyncAt);

  AiProviderAdvertisement broken = valid;
  broken.networkId.data[0] ^= 0xff;
  ASSERT_TRUE(signAiAdvertisement(broken, otherSecret));
  notify.advertisements.clear();
  notify.advertisements.push_back(valid);
  notify.advertisements.push_back(broken);
  AiDiscoveryService syncing;
  syncing.setEnabled(true);
  syncing.setNetworkId(CRYPTONOTE_NETWORK);
  AiConnectionState client;
  client.capability = AI_PEER_SUPPORTED;
  client.hasOutstanding = true;
  client.outstandingRequestId = 7;
  client.cursor = 0;
  COMMAND_REQUEST_AI_PROVIDERS::response response;
  response.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  response.requestId = 7;
  response.result = AI_DIRECTORY_OK;
  response.done = false;
  response.nextCursor = 2;
  response.advertisements.push_back(valid);
  response.advertisements.push_back(broken);
  std::vector<AiOutbound> extra;
  syncing.handleProvidersResponse(LevinProtocol::encode(response), client, kNow,
                                  [&extra](const AiOutbound& message) {
                                    extra.push_back(message);
                                    return true;
                                  },
                                  AiRelayFn());
  EXPECT_TRUE(syncing.getProviders(AiProviderQuery(), kNow).providers.empty());
  EXPECT_EQ(kNow + AI_SYNC_BACKOFF_SECONDS, client.nextSyncAt);
  EXPECT_EQ(0u, client.cursor);
}

TEST(AiProviderDiscovery, SyncContinuesPastTheFirstRound) {
  AiDiscoveryService source;
  AiDiscoveryService receiver;
  source.setEnabled(true);
  receiver.setEnabled(true);
  source.setNetworkId(CRYPTONOTE_NETWORK);
  receiver.setNetworkId(CRYPTONOTE_NETWORK);

  const uint16_t count = 150;
  int64_t regNow = kNow;
  for (uint16_t index = 1; index <= count; ++index) {
    AiProviderAdvertisement ad = signedAd(1, AI_STATUS_AVAILABLE, keyFromIndex(index), secretFromIndex(index));
    AiIngestResult ingested = source.registerLocal(ad, regNow);
    if (ingested.result == AiAcceptResult::rateLimited) {
      regNow += AI_RATE_WINDOW_SECONDS;
      ingested = source.registerLocal(ad, regNow);
    }
    ASSERT_EQ(AiAcceptResult::accepted, ingested.result);
  }
  EXPECT_EQ(static_cast<uint64_t>(count), source.listedCount(regNow));

  AiConnectionState state;
  state.capability = AI_PEER_SUPPORTED;
  state.nextSyncAt = regNow;
  std::vector<AiOutbound> outbox;
  AiEnqueueFn enqueue = [&outbox](const AiOutbound& message) {
    outbox.push_back(message);
    return true;
  };

  int64_t now = regNow;
  bool sawContinuation = false;
  for (uint32_t step = 0; step < 80 && receiver.listedCount(now) < count; ++step) {
    if (now < state.nextSyncAt) {
      now = state.nextSyncAt;
    }
    if (!state.hasOutstanding) {
      outbox.clear();
      if (!receiver.requestAiProviderPage(state, now, enqueue)) {
        ASSERT_LT(now, state.nextSyncAt);
        continue;
      }
    }
    ASSERT_TRUE(state.hasOutstanding);
    ASSERT_FALSE(outbox.empty());
    EXPECT_LE(state.pagesThisRound, AI_MAX_SYNC_PAGES);
    BinaryArray response;
    AiConnectionState serverState;
    ASSERT_EQ(LEVIN_PROTOCOL_RETCODE_SUCCESS, source.handleProvidersRequest(outbox.back().body, response, serverState, now));
    EXPECT_LE(response.size(), static_cast<size_t>(AI_MAX_PAGE_BYTES));
    outbox.clear();
    const uint64_t cursorBefore = state.cursor;
    receiver.handleProvidersResponse(response, state, now, enqueue, AiRelayFn());
    EXPECT_LE(state.pagesThisRound, AI_MAX_SYNC_PAGES);
    EXPECT_LE(state.bytesThisRound, static_cast<uint32_t>(AI_MAX_SYNC_BYTES));
    if (!state.hasOutstanding && state.nextSyncAt == now + AI_SYNC_BACKOFF_SECONDS) {
      EXPECT_EQ(cursorBefore, state.cursor);
    }
    if (!state.hasOutstanding && state.pagesThisRound == 0 && state.cursor > 0 &&
        state.nextSyncAt == now + AI_SYNC_CONTINUATION_INTERVAL_SECONDS) {
      sawContinuation = true;
    }
  }
  EXPECT_TRUE(sawContinuation);
  EXPECT_EQ(static_cast<uint64_t>(count), receiver.listedCount(now));
  EXPECT_EQ(0u, state.cursor);

  AiConnectionState pastEnd;
  pastEnd.capability = AI_PEER_SUPPORTED;
  pastEnd.hasOutstanding = true;
  pastEnd.outstandingRequestId = 11;
  pastEnd.cursor = 1000;
  pastEnd.nextSyncAt = now;
  COMMAND_REQUEST_AI_PROVIDERS::response emptyDone;
  emptyDone.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  emptyDone.requestId = 11;
  emptyDone.result = AI_DIRECTORY_OK;
  emptyDone.done = true;
  emptyDone.nextCursor = 1000;
  outbox.clear();
  receiver.handleProvidersResponse(LevinProtocol::encode(emptyDone), pastEnd, now, enqueue, AiRelayFn());
  EXPECT_FALSE(pastEnd.hasOutstanding);
  EXPECT_EQ(0u, pastEnd.cursor);
  EXPECT_EQ(now + AI_SYNC_ROUND_INTERVAL_SECONDS, pastEnd.nextSyncAt);
  EXPECT_NE(now + AI_SYNC_BACKOFF_SECONDS, pastEnd.nextSyncAt);
}

TEST(AiProviderDiscovery, ProbeRetriesOnceAfterTimeoutAndNotAfterLegacyError) {
  AiDiscoveryService service;
  service.setEnabled(true);
  AiConnectionState timed;
  std::vector<AiOutbound> outbox;
  AiPeerRef ref;
  ref.state = &timed;
  ref.handshakeComplete = true;
  ref.enqueue = [&outbox](const AiOutbound& message) {
    outbox.push_back(message);
    return true;
  };
  std::vector<AiPeerRef> peers(1, ref);

  service.maintain(kNow, peers);
  ASSERT_EQ(1u, outbox.size());
  EXPECT_EQ(AI_PEER_PROBE_SENT, timed.capability);

  service.maintain(kNow + AI_REQUEST_TIMEOUT_SECONDS, peers);
  EXPECT_EQ(AI_PEER_UNKNOWN, timed.capability);
  EXPECT_EQ(1u, outbox.size());
  service.maintain(kNow + AI_REQUEST_TIMEOUT_SECONDS + AI_PROBE_RETRY_BACKOFF_SECONDS - 1, peers);
  EXPECT_EQ(1u, outbox.size());
  service.maintain(kNow + AI_REQUEST_TIMEOUT_SECONDS + AI_PROBE_RETRY_BACKOFF_SECONDS, peers);
  EXPECT_EQ(2u, outbox.size());
  EXPECT_EQ(AI_PEER_PROBE_SENT, timed.capability);
  const int64_t secondSent = kNow + AI_REQUEST_TIMEOUT_SECONDS + AI_PROBE_RETRY_BACKOFF_SECONDS;
  service.maintain(secondSent + AI_REQUEST_TIMEOUT_SECONDS, peers);
  EXPECT_EQ(AI_PEER_UNSUPPORTED, timed.capability);
  EXPECT_EQ(2u, outbox.size());
  service.maintain(secondSent + AI_REQUEST_TIMEOUT_SECONDS + AI_PROBE_RETRY_BACKOFF_SECONDS + 5, peers);
  EXPECT_EQ(2u, outbox.size());

  AiDiscoveryService legacyService;
  legacyService.setEnabled(true);
  AiConnectionState legacy;
  std::vector<AiOutbound> legacyOut;
  AiPeerRef legacyRef;
  legacyRef.state = &legacy;
  legacyRef.handshakeComplete = true;
  legacyRef.enqueue = [&legacyOut](const AiOutbound& message) {
    legacyOut.push_back(message);
    return true;
  };
  std::vector<AiPeerRef> legacyPeers(1, legacyRef);
  legacyService.maintain(kNow, legacyPeers);
  ASSERT_EQ(1u, legacyOut.size());
  COMMAND_AI_DISCOVERY_CAPABILITIES::response supported;
  supported.protocolVersion = AI_DISCOVERY_PROTOCOL_VERSION;
  supported.discoverySupported = true;
  legacyService.handleCapabilityResponse(LevinProtocol::encode(supported), legacy, kNow + 1, legacyRef.enqueue,
                                         static_cast<int32_t>(LevinError::ERROR_CONNECTION_HANDLER_NOT_DEFINED));
  EXPECT_EQ(AI_PEER_UNSUPPORTED, legacy.capability);
  legacyService.maintain(kNow + AI_REQUEST_TIMEOUT_SECONDS + AI_PROBE_RETRY_BACKOFF_SECONDS, legacyPeers);
  EXPECT_EQ(1u, legacyOut.size());
}

TEST(AiProviderDiscovery, LocalReserveEvictsPeerRecordsAndCountMatchesListing) {
  AiProviderRegistry registry(CRYPTONOTE_NETWORK, AI_LOCAL_RESERVED_ENTRIES + 2);
  const int64_t soon = kIssuedAt + 200;
  const int64_t later = kIssuedAt + 600;
  AiProviderAdvertisement soonPeer = signedAd(1, AI_STATUS_AVAILABLE, keyFromIndex(1), secretFromIndex(1));
  soonPeer.expiresAt = soon;
  ASSERT_TRUE(signAiAdvertisement(soonPeer, secretFromIndex(1)));
  AiProviderAdvertisement offlinePeer = signedAd(1, AI_STATUS_OFFLINE, keyFromIndex(2), secretFromIndex(2));
  offlinePeer.expiresAt = kNow + 50;
  ASSERT_TRUE(signAiAdvertisement(offlinePeer, secretFromIndex(2)));
  ASSERT_EQ(AiAcceptResult::accepted, registry.accept(soonPeer, kNow, AI_SOURCE_PEER).result);
  ASSERT_EQ(AiAcceptResult::accepted, registry.accept(offlinePeer, kNow, AI_SOURCE_PEER).result);
  AiProviderAdvertisement thirdPeer = signedAd(1, AI_STATUS_AVAILABLE, keyFromIndex(3), secretFromIndex(3));
  EXPECT_EQ(AiAcceptResult::capacityRejected, registry.accept(thirdPeer, kNow, AI_SOURCE_PEER).result);
  EXPECT_EQ(2u, registry.size());

  for (uint16_t index = 10; index < 10 + AI_LOCAL_RESERVED_ENTRIES; ++index) {
    AiProviderAdvertisement local = signedAd(1, AI_STATUS_AVAILABLE, keyFromIndex(index), secretFromIndex(index));
    local.expiresAt = later;
    ASSERT_TRUE(signAiAdvertisement(local, secretFromIndex(index)));
    ASSERT_EQ(AiAcceptResult::accepted, registry.accept(local, kNow, AI_SOURCE_LOCAL).result);
  }
  EXPECT_EQ(static_cast<size_t>(AI_LOCAL_RESERVED_ENTRIES + 2), registry.size());

  AiProviderAdvertisement extra = signedAd(1, AI_STATUS_AVAILABLE, keyFromIndex(200), secretFromIndex(200));
  extra.expiresAt = later;
  ASSERT_TRUE(signAiAdvertisement(extra, secretFromIndex(200)));
  EXPECT_EQ(AiAcceptResult::accepted, registry.accept(extra, kNow, AI_SOURCE_LOCAL).result);
  EXPECT_EQ(static_cast<size_t>(AI_LOCAL_RESERVED_ENTRIES + 2), registry.size());
  AiProviderQuery offlineQuery;
  offlineQuery.status = AI_STATUS_OFFLINE;
  EXPECT_EQ(1u, registry.query(offlineQuery, kNow).providers.size());
  bool soonStillPresent = false;
  AiProviderQuery all;
  uint64_t cursor = 0;
  for (;;) {
    all.cursor = cursor;
    AiProviderPage page = registry.query(all, kNow);
    for (size_t i = 0; i < page.providers.size(); ++i) {
      if (memcmp(page.providers[i].advertisement.providerPublicKey.data, soonPeer.providerPublicKey.data, 32) == 0) {
        soonStillPresent = true;
      }
    }
    if (page.done) {
      break;
    }
    cursor = page.nextCursor;
  }
  EXPECT_FALSE(soonStillPresent);

  AiProviderAdvertisement blocked = signedAd(1, AI_STATUS_AVAILABLE, keyFromIndex(201), secretFromIndex(201));
  EXPECT_EQ(AiAcceptResult::capacityRejected, registry.accept(blocked, kNow, AI_SOURCE_LOCAL).result);

  AiProviderAdvertisement update = signedAd(2, AI_STATUS_BUSY, keyFromIndex(10), secretFromIndex(10));
  update.expiresAt = later;
  ASSERT_TRUE(signAiAdvertisement(update, secretFromIndex(10)));
  EXPECT_EQ(AiAcceptResult::accepted, registry.accept(update, kNow, AI_SOURCE_LOCAL).result);

  uint64_t paged = 0;
  cursor = 0;
  for (;;) {
    all.cursor = cursor;
    all.status = 0;
    AiProviderPage page = registry.query(all, kNow);
    paged += page.providers.size();
    if (page.done) {
      break;
    }
    cursor = page.nextCursor;
  }
  EXPECT_EQ(paged, registry.listedCount(kNow));
  EXPECT_EQ(paged + 1, static_cast<uint64_t>(registry.size()));
}

TEST(AiProviderDiscovery, RegistrationStaysLoopbackWithoutDaemonCredentials) {
  EXPECT_EQ(aiRegistrationForbidden, aiRegistrationAccess(true, 0x0a000001u));
  EXPECT_EQ(aiRegistrationAllowed, aiRegistrationAccess(true, 0x7f000001u));
  EXPECT_EQ(aiRegistrationDisabled, aiRegistrationAccess(false, 0x7f000001u));
}
