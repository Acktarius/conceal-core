# conceald-migrate-gui — wxWidgets front-end for conceald-migrate
#
# Build: cmake -DBUILD_MIGRATION_GUI=ON ..
# Optional: -DMIGRATION_GUI_AUTO_INSTALL_DEPS=OFF to skip apt/dnf/brew attempts

if(NOT BUILD_MIGRATION_GUI)
  return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/FindOrInstallWxWidgets.cmake")
conceal_find_or_install_wxwidgets()

set(MIGRATION_GUI_SOURCES
  MigrationGuiMain.cpp
  MigrationGuiApp.cpp
  MigrationDialog.cpp
  MigratePaths.cpp
)

add_executable(MigrationToolGui ${MIGRATION_GUI_SOURCES})
target_include_directories(MigrationToolGui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${wxWidgets_INCLUDE_DIRS}
)
target_compile_definitions(MigrationToolGui PRIVATE ${wxWidgets_DEFINITIONS})
if(wxWidgets_CXX_FLAGS)
  target_compile_options(MigrationToolGui PRIVATE ${wxWidgets_CXX_FLAGS})
endif()
# wxMSW links version.lib; avoid CMake resolving "version" to our version UTILITY target.
set(_migrationGuiWxLibs)
foreach(_wxLib ${wxWidgets_LIBRARIES})
  if(MSVC AND _wxLib STREQUAL "version")
    list(APPEND _migrationGuiWxLibs "version.lib")
  else()
    list(APPEND _migrationGuiWxLibs ${_wxLib})
  endif()
endforeach()
target_link_libraries(MigrationToolGui PRIVATE ${_migrationGuiWxLibs})
# wx-config-static lists wx/bundled libs but MSYS2 omits Win32 system libs (comctl32, etc.).
if(MINGW)
  if(NOT _mingwWxLibs)
    target_link_libraries(MigrationToolGui PRIVATE
      "-lwxpng-3.2" "-lwxzlib-3.2" "-lwxjpeg-3.2" "-lwxregexu-3.2" "-lwxtiff-3.2" "-lwxexpat-3.2")
  endif()
  target_link_libraries(MigrationToolGui PRIVATE
    comctl32 shell32 ole32 oleaut32 uuid oleacc
    gdi32 winspool comdlg32 advapi32 user32 kernel32
    ws2_32 shlwapi uxtheme "-lversion")
elseif(MSVC)
  target_link_libraries(MigrationToolGui PRIVATE shlwapi.lib uxtheme.lib)
endif()
add_dependencies(MigrationToolGui version)
set_property(TARGET MigrationToolGui PROPERTY OUTPUT_NAME "conceald-migrate-gui")

if(WIN32)
  set_target_properties(MigrationToolGui PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

message(STATUS "Migration GUI enabled: conceald-migrate-gui")
