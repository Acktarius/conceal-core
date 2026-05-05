add_definitions(-DMINIUPNP_STATICLIB)
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/external/parallel_hashmap)

if(ENABLE_MDBX AND MDBX_INCLUDE_DIR)
  include_directories(${MDBX_INCLUDE_DIR})
  add_definitions(-DHAVE_MDBX)
endif()

if(ENABLE_MDBX)
  file(GLOB_RECURSE MDBX_STORAGE_FILES Storage/*)
endif()

file(GLOB_RECURSE BlockchainExplorer BlockchainExplorer/*)
file(GLOB_RECURSE Common Common/*)
file(GLOB_RECURSE crypto crypto/*)
file(GLOB_RECURSE CryptoNoteCore CryptoNoteCore/* CryptoNoteConfig.h)
file(GLOB_RECURSE CryptoNoteProtocol CryptoNoteProtocol/*)
file(GLOB_RECURSE Daemon Daemon/*)
file(GLOB_RECURSE Http HTTP/*)
file(GLOB_RECURSE InProcessNode InProcessNode/*)
file(GLOB_RECURSE Logging Logging/*)
file(GLOB_RECURSE Optimizer Optimizer/*)
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
file(GLOB_RECURSE PaymentGate PaymentGate/*)
file(GLOB_RECURSE PaymentGateService PaymentGateService/*)
file(GLOB_RECURSE WALLET_INIT_FILES WalletInit/*)

if(MSVC OR MINGW)
  file(GLOB_RECURSE System System/* Platform/Windows/System/*)
elseif(APPLE)
  file(GLOB_RECURSE System System/* Platform/OSX/System/*)
else()
  file(GLOB_RECURSE System System/* Platform/Linux/System/*)
endif()

source_group("" FILES ${Common} ${ConcealWallet} ${crypto} ${CryptoNoteCore} ${CryptoNoteProtocol} ${Daemon} ${JsonRpcServer} ${Http} ${Logging} ${NodeRpcProxy} ${P2p} ${Rpc} ${Serialization} ${System} ${Transfers} ${Wallet} ${WalletLegacy})

if(ENABLE_MDBX AND MDBX_STORAGE_FILES)
  source_group("Storage" FILES ${MDBX_STORAGE_FILES})
endif()

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
add_library(PaymentGate STATIC ${PaymentGate})
add_library(JsonRpcServer STATIC ${JsonRpcServer})

# Storage library (only when MDBX is enabled)
if(ENABLE_MDBX AND MDBX_STORAGE_FILES)
  add_library(Storage STATIC ${MDBX_STORAGE_FILES})
  add_dependencies(Storage version)
  if(MDBX_LIBRARY_TARGET)
    target_link_libraries(Storage PRIVATE ${MDBX_LIBRARY_TARGET})
  endif()
endif()

add_executable(Daemon ${Daemon})
add_executable(ConcealWallet ${ConcealWallet})
add_executable(PaymentGateService ${PaymentGateService})
add_executable(Optimizer ${Optimizer})
add_executable(WalletInit ${WALLET_INIT_FILES})

if (MSVC)
  target_link_libraries(System ws2_32)
endif ()

if (MINGW)
  target_link_libraries(Common dnsapi)
  target_link_libraries(P2P bcrypt)
  target_link_libraries(System ws2_32)
endif ()

target_link_libraries(Daemon CryptoNoteCore Storage P2P Rpc System Http Logging Common crypto libminiupnpc-static BlockchainExplorer ${Boost_LIBRARIES} Serialization)
target_link_libraries(ConcealWallet Wallet NodeRpcProxy Transfers Rpc Http CryptoNoteCore Storage System Logging Common crypto ${Boost_LIBRARIES} Serialization Mnemonics)
target_link_libraries(PaymentGateService PaymentGate JsonRpcServer Wallet NodeRpcProxy Transfers CryptoNoteCore Storage crypto P2P Rpc Http System Logging Common InProcessNode libminiupnpc-static BlockchainExplorer ${Boost_LIBRARIES} Serialization)
target_link_libraries(Optimizer PaymentGate Rpc Http CryptoNoteCore Storage Logging Serialization crypto System Common ${Boost_LIBRARIES})

# WalletInit - matches ConcealWallet's link libraries exactly
target_link_libraries(WalletInit
    Wallet
    NodeRpcProxy
    Transfers
    Rpc
    Http
    CryptoNoteCore
    Storage
    System
    Logging
    Common
    crypto
    ${Boost_LIBRARIES}
    Serialization
    Mnemonics
)

if(ENABLE_MDBX AND MDBX_STORAGE_FILES)
  file(GLOB_RECURSE MIGRATION_TOOL_FILES MigrationTool/*)
  add_executable(MigrationTool ${MIGRATION_TOOL_FILES})
  target_link_libraries(MigrationTool
    CryptoNoteCore
    Storage
    P2P
    Rpc
    System
    Http
    Logging
    Common
    crypto
    Serialization
    ${Boost_LIBRARIES}
  )
  add_dependencies(MigrationTool version)
  set_property(TARGET MigrationTool PROPERTY OUTPUT_NAME "conceald-migrate")
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
  target_link_libraries(ConcealWallet -lresolv)
  target_link_libraries(Daemon -lresolv)
  target_link_libraries(PaymentGateService -lresolv)
  target_link_libraries(WalletInit -lresolv)
endif ()

add_dependencies(Rpc version)
add_dependencies(Daemon version)
add_dependencies(ConcealWallet version)
add_dependencies(PaymentGateService version)
add_dependencies(P2P version)
add_dependencies(WalletInit version)

set_property(TARGET ConcealWallet PROPERTY OUTPUT_NAME "concealwallet")
set_property(TARGET PaymentGateService PROPERTY OUTPUT_NAME "walletd")
set_property(TARGET Daemon PROPERTY OUTPUT_NAME "conceald")
set_property(TARGET Optimizer PROPERTY OUTPUT_NAME "optimizer")
set_property(TARGET WalletInit PROPERTY OUTPUT_NAME "conceal-wallet-init")