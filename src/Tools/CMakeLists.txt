# Conceal tools
# ColdSync - standalone wallet pre-syncer
# MigrationTool - blockchain migration utility
# Optimizer - fusion transaction optimizer

# Optimizer
add_executable(Optimizer Optimizer.cpp)
target_link_libraries(Optimizer BoltHttp Rpc Http Blockchain CryptoNoteCore Storage Logging Serialization crypto System Common
    ${Boost_LIBRARIES}
)
add_dependencies(Optimizer version)
set_property(TARGET Optimizer PROPERTY OUTPUT_NAME "conceal-optimizer")

# ColdSync
add_executable(ColdSync ColdSync.cpp)
target_link_libraries(ColdSync
    -Wl,--start-group
    BoltRpcLib
    BoltCore
    BoltSync
    Wallet
    NodeRpcProxy
    Transfers
    Rpc
    Http
    CryptoNoteCore
    Storage
    System
    Logging
    crypto
    ${Boost_LIBRARIES}
    Serialization
    Mnemonics
    Common
    -Wl,--end-group
)
add_dependencies(ColdSync version)
set_property(TARGET ColdSync PROPERTY OUTPUT_NAME "conceal-coldsync")

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR APPLE AND NOT ANDROID)
  target_link_libraries(ColdSync -lresolv)
endif()

# MigrationTool
add_executable(MigrationTool MigrationTool.cpp)
target_link_libraries(MigrationTool CryptoNoteCore Storage P2P Rpc System Http Logging Common crypto Serialization
  ${Boost_LIBRARIES}
)
add_dependencies(MigrationTool version)
set_property(TARGET MigrationTool PROPERTY OUTPUT_NAME "conceald-migrate")