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
#include "P2p/NetNodeConfig.h"
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
// Seed bytes are 0x01..0x20. Field values are the signing-vector advertisement in this file.
const char kExpectedPublicKey[] = "ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99";
const char kExpectedCanonical[] =
    "19636f6e6365616c2d61692d70726f76696465722d61642d763101ff00a1b2c75f8277f512a336e7e3dd12"
    "ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed990100000000000000"
    "00f153650000000008f85365000000000102000000010000000400000064656d6f00100000e803000000000000"
    "01010f0000003132372e302e302e313a3138303830";
const char kExpectedHash[] = "6d40f89ddfe5b5b5236d2e5f707f02e79e5680057b0d44c26b29453c6ae7dc16";
const char kExpectedSignature[] =
    "32f3d4b8165aed4219d58f8aac3cea19a4ede108e2d198b756f25b2e19253009b6b9906314df6b40dda0c253ac61a8c83415feee27eb0bdf567121f43c12570b";

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

}

TEST(AiProviderDiscovery, SigningVectorAndRoundTrip) {
  const crypto::PublicKey pub = publicFromSeed();
  const crypto::SecretKey secret = secretFromSeed();
  AiProviderAdvertisement ad = signedAd(1, AI_STATUS_AVAILABLE, pub, secret);

  std::vector<uint8_t> canonical;
  ASSERT_TRUE(buildAiAdvertisementCanonical(ad, canonical));
  const crypto::Hash hash = aiAdvertisementSigningHash(canonical);
  const std::string publicHex = common::podToHex(pub);
  const std::string canonicalHex = common::toHex(canonical);
  const std::string hashHex = common::podToHex(hash);

  EXPECT_EQ(std::string("conceal-ai-provider-ad-v1"), std::string(reinterpret_cast<const char*>(&canonical[1]), canonical[0]));
  EXPECT_EQ(kExpectedPublicKey, publicHex);
  EXPECT_EQ(kExpectedCanonical, canonicalHex);
  EXPECT_EQ(kExpectedHash, hashHex);
  std::vector<uint8_t> again;
  EXPECT_TRUE(verifyAiAdvertisementSignature(ad, again));
  EXPECT_EQ(canonical, again);

  // generate_signature draws a random nonce, so signature bytes change between runs.
  // This vector is one signature that verifies the locked hash with the CryptoNote checker.
  crypto::Signature locked;
  ASSERT_TRUE(common::podFromHex(kExpectedSignature, locked));
  AiProviderAdvertisement lockedAd = ad;
  lockedAd.signature = locked;
  std::vector<uint8_t> lockedCanonical;
  EXPECT_TRUE(verifyAiAdvertisementSignature(lockedAd, lockedCanonical));
  EXPECT_EQ(canonical, lockedCanonical);

  const BinaryArray encoded = LevinProtocol::encode(ad);
  AiProviderAdvertisement decoded;
  ASSERT_TRUE(LevinProtocol::decode(encoded, decoded));
  EXPECT_EQ(ad.sequence, decoded.sequence);
  EXPECT_EQ(ad.models[0].modelId, decoded.models[0].modelId);
  EXPECT_EQ(ad.models[0].priceAtomicUnits, decoded.models[0].priceAtomicUnits);
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
                                       "\",\"maxContextTokens\":1,\"priceAtomicUnits\":1,\"billingUnit\":1}"));
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
