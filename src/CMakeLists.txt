add_definitions(-DMINIUPNP_STATICLIB)
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/external/parallel_hashmap)

if(ENABLE_MDBX AND MDBX_INCLUDE_DIR)
  include_directories(${MDBX_INCLUDE_DIR})
  add_definitions(-DHAVE_MDBX)
endif()


file(GLOB_RECURSE MDBX_STORAGE_FILES Storage/*)
file(GLOB_RECURSE Blockchain_FILES Blockchain/*)
file(GLOB_RECURSE SPV_FILES SPV/*)
file(GLOB_RECURSE BlockchainExplorer BlockchainExplorer/*)
file(GLOB_RECURSE Common Common/*)
file(GLOB_RECURSE crypto crypto/*)
file(GLOB_RECURSE CryptoNoteCore CryptoNoteCore/* CryptoNoteConfig.h)
file(GLOB_RECURSE CryptoNoteProtocol CryptoNoteProtocol/*)
file(GLOB_RECURSE Daemon Daemon/*)
file(GLOB_RECURSE Http HTTP/*)
file(GLOB_RECURSE InProcessNode InProcessNode/*)
file(GLOB_RECURSE Logging Logging/*)
file(GLOB_RECURSE Mnemonics Mnemonics/*)
file(GLOB_RECURSE NodeRpcProxy NodeRpcProxy/*)
file(GLOB_RECURSE P2p P2p/*)
file(GLOB_RECURSE Rpc Rpc/*)
file(GLOB_RECURSE Serialization Serialization/*)
file(GLOB_RECURSE ConcealWallet ConcealWallet/*)
file(GLOB_RECURSE Transfers Transfers/*)
file(GLOB_RECURSE Wallet Wallet/*)
file(GLOB_RECURSE WalletLegacy WalletLegacy/*)
file(GLOB_RECURSE JsonRpcServer JsonRpcServer/*)

file(GLOB_RECURSE BOLTSYNC_FILES BoltSync/*)
file(GLOB_RECURSE BOLTCORE_FILES BoltCore/*)
file(GLOB_RECURSE BOLT_TUI_FILES BoltClientWallet/*)
file(GLOB_RECURSE BOLTRPC_FILES BoltRPC/*)
file(GLOB_RECURSE SIDECHAIN_FILES Sidechain/*)
file(GLOB_RECURSE BOLTHTTP_FILES BoltHttp/*)

if(MSVC OR MINGW)
  file(GLOB_RECURSE System System/* Platform/Windows/System/*)
elseif(APPLE)
  file(GLOB_RECURSE System System/* Platform/OSX/System/*)
else()
  file(GLOB_RECURSE System System/* Platform/Linux/System/*)
endif()

source_group("" FILES ${Common} ${ConcealWallet} ${crypto} ${CryptoNoteCore} ${CryptoNoteProtocol} ${Daemon} ${JsonRpcServer} ${Http} ${Logging} ${NodeRpcProxy} ${P2p} ${Rpc} ${Serialization} ${System} ${Transfers} ${Wallet} ${WalletLegacy})
source_group("Storage" FILES ${MDBX_STORAGE_FILES})
source_group("Blockchain" FILES ${Blockchain_FILES})
source_group("SPV" FILES ${SPV_FILES})

# ── Libraries ──────────────────────────────────────────────────────────────
add_library(Blockchain STATIC ${Blockchain_FILES})
add_library(BlockchainExplorer STATIC ${BlockchainExplorer})
add_library(Common STATIC ${Common})
add_library(crypto STATIC ${crypto})
add_library(CryptoNoteCore STATIC ${CryptoNoteCore})
add_library(Http STATIC ${Http})
add_library(InProcessNode STATIC ${InProcessNode})
add_library(Logging STATIC ${Logging})
add_library(Mnemonics STATIC ${Mnemonics})
add_library(NodeRpcProxy STATIC ${NodeRpcProxy})
add_library(Rpc STATIC ${Rpc})
add_library(P2P STATIC ${CryptoNoteProtocol} ${P2p})
add_library(Serialization STATIC ${Serialization})
add_library(System STATIC ${System})
add_library(Transfers STATIC ${Transfers})
add_library(Wallet STATIC ${Wallet} ${WalletLegacy})
add_library(JsonRpcServer STATIC ${JsonRpcServer})
add_library(BoltSync STATIC ${BOLTSYNC_FILES})
add_library(BoltCore STATIC ${BOLTCORE_FILES})
add_library(BoltHttp STATIC ${BOLTHTTP_FILES})

add_library(Storage STATIC ${MDBX_STORAGE_FILES})
add_dependencies(Storage version)
target_link_libraries(Storage PRIVATE ${MDBX_LIBRARY_TARGET})

# SPV static library
add_library(SPVLib STATIC ${SPV_FILES})
target_link_libraries(SPVLib PUBLIC Common CryptoNoteCore Serialization crypto 
    ${Boost_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
add_dependencies(SPVLib version)

# Sidechain static library
add_library(SidechainLib STATIC ${SIDECHAIN_FILES})
target_link_libraries(SidechainLib PUBLIC Blockchain NodeRpcProxy System Rpc Http BoltSync BoltCore BoltHttp CryptoNoteCore Serialization Common Logging crypto ${Boost_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
add_dependencies(SidechainLib version)

# BoltRPC static library
add_library(BoltRpcLib STATIC ${BOLTRPC_FILES})
target_link_libraries(BoltRpcLib PUBLIC Blockchain Mnemonics NodeRpcProxy System Rpc Http BoltSync BoltCore BoltHttp CryptoNoteCore Serialization Common Logging crypto ${Boost_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
add_dependencies(BoltRpcLib version)

# ── Link dependencies for the new Blockchain library ───────────────────────
target_link_libraries(Blockchain PUBLIC CryptoNoteCore Storage Common crypto Serialization Logging ${Boost_LIBRARIES})
add_dependencies(Blockchain version)

# ── Executables ────────────────────────────────────────────────────────────
add_executable(Daemon ${Daemon})
add_executable(ConcealWallet ${ConcealWallet})
add_executable(BoltClientWallet ${BOLT_TUI_FILES})
add_executable(BoltRPC ${BOLTRPC_FILES})
add_executable(Sidechain ${SIDECHAIN_FILES})

if (MSVC)
  target_link_libraries(System ws2_32)
endif ()

if (MINGW)
  target_link_libraries(Common dnsapi)
  target_link_libraries(P2P bcrypt)
  target_link_libraries(System ws2_32)
endif ()

# ── Executable link dependencies ───────────────────────────────────────────
target_link_libraries(Daemon CryptoNoteCore Blockchain Storage P2P Rpc System Http Logging Common crypto libminiupnpc-static BlockchainExplorer ${Boost_LIBRARIES} Serialization)
target_link_libraries(ConcealWallet Wallet NodeRpcProxy Transfers Rpc Http CryptoNoteCore Blockchain Storage System Logging Common crypto ${Boost_LIBRARIES} Serialization Mnemonics)

# ── Bolt library link dependencies ─────────────────────────────────────────
target_link_libraries(BoltSync PUBLIC Blockchain CryptoNoteCore Storage Common crypto Serialization ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(BoltCore PUBLIC Blockchain CryptoNoteCore Common crypto ${Boost_LIBRARIES})

target_link_libraries(BoltClientWallet BoltSync BoltCore NodeRpcProxy Rpc Http System Blockchain CryptoNoteCore Serialization
  -Wl,--start-group
      Common 
      Logging 
      crypto
  -Wl,--end-group
  ${Boost_LIBRARIES} 
  ${CMAKE_THREAD_LIBS_INIT}
)

target_link_libraries(BoltRPC  SPVLib BoltSync BoltCore BoltHttp Mnemonics NodeRpcProxy Rpc Http System Blockchain CryptoNoteCore Serialization Logging Common crypto
  ${Boost_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT}
)

target_link_libraries(Sidechain BoltSync BoltCore BoltHttp NodeRpcProxy Rpc Http System Blockchain CryptoNoteCore Serialization
  -Wl,--start-group
      Common
      Logging
      crypto
  -Wl,--end-group
  ${Boost_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT}
)

# Tools (ColdSync + MigrationTool + Optimizer)
add_subdirectory(Tools)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
  target_link_libraries(ConcealWallet -lresolv)
  target_link_libraries(Daemon -lresolv)
  target_link_libraries(BoltClientWallet -lresolv)
  target_link_libraries(BoltRPC -lresolv)
  target_link_libraries(Sidechain -lresolv)
endif ()

# ── Dependencies ───────────────────────────────────────────────────────────
add_dependencies(Rpc version)
add_dependencies(Daemon version)
add_dependencies(ConcealWallet version)
add_dependencies(P2P version)
add_dependencies(BoltRPC version)
add_dependencies(Sidechain version)
add_dependencies(Storage version)

set_property(TARGET ConcealWallet PROPERTY OUTPUT_NAME "concealwallet")
set_property(TARGET Daemon PROPERTY OUTPUT_NAME "conceald")
set_property(TARGET BoltRPC PROPERTY OUTPUT_NAME "conceal-rpc")
set_property(TARGET Sidechain PROPERTY OUTPUT_NAME "conceal-side")