# Conceal tools
# ColdSync  - standalone wallet pre-syncer
# MigrationTool - blockchain migration utility

add_executable(ColdSync ColdSync.cpp)
target_link_libraries(ColdSync
    -Wl,--start-group
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

if(ENABLE_MDBX AND MDBX_STORAGE_FILES)
  add_executable(MigrationTool MigrationTool.cpp)
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

  # Same directory as conceald-migrate (build/src/Tools with default Makefile/Ninja layout).
  add_custom_command(
    TARGET MigrationTool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tools/migrate.sh"
      "${CMAKE_CURRENT_BINARY_DIR}/migrate.sh"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tools/migrate-gui.sh"
      "${CMAKE_CURRENT_BINARY_DIR}/migrate-gui.sh"
    COMMENT "Copying migrate.sh and migrate-gui.sh next to conceald-migrate"
  )
endif()