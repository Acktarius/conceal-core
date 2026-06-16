# Find wxWidgets for conceald-migrate-gui; optionally install OS packages when missing.
# Included from Migration-gui/CMakeLists.txt when BUILD_MIGRATION_GUI=ON.
#
# Options:
#   MIGRATION_GUI_AUTO_INSTALL_DEPS — try platform package manager (default: ON)
#
# Sets: wxWidgets_FOUND, includes wxWidgets_USE_FILE via conceal_find_or_install_wxwidgets()

option(MIGRATION_GUI_AUTO_INSTALL_DEPS
       "Try to install wxWidgets development packages if not found (Linux/macOS)"
       ON)

function(_conceal_run_elevated out_var)
  # Prefer non-interactive sudo (CI); then interactive sudo; then raw command (root).
  execute_process(
    COMMAND sudo -n ${ARGN}
    RESULT_VARIABLE _rv
    ERROR_QUIET
    OUTPUT_QUIET
  )
  if(_rv EQUAL 0)
    set(${out_var} 0 PARENT_SCOPE)
    return()
  endif()

  message(STATUS "  sudo -n failed; trying sudo (may prompt for password)...")
  execute_process(
    COMMAND sudo ${ARGN}
    RESULT_VARIABLE _rv
    ERROR_QUIET
    OUTPUT_QUIET
  )
  if(_rv EQUAL 0)
    set(${out_var} 0 PARENT_SCOPE)
    return()
  endif()

  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE _rv
    ERROR_QUIET
    OUTPUT_QUIET
  )
  set(${out_var} ${_rv} PARENT_SCOPE)
endfunction()

function(_conceal_install_wxwidgets_apt)
  if(NOT EXISTS "/etc/debian_version" AND NOT EXISTS "/etc/ubuntu_version")
    if(NOT EXISTS "/etc/os-release")
      return()
    endif()
    file(READ "/etc/os-release" _os_release)
    if(NOT _os_release MATCHES "ID=debian" AND NOT _os_release MATCHES "ID=ubuntu" AND NOT _os_release MATCHES "ID=linuxmint")
      return()
    endif()
  endif()

  find_program(APT_GET_EXECUTABLE apt-get)
  if(NOT APT_GET_EXECUTABLE)
    return()
  endif()

  message(STATUS "Debian/Ubuntu detected — installing wxGTK dev package via apt...")
  _conceal_run_elevated(_rv ${APT_GET_EXECUTABLE} update -qq)
  foreach(_pkg libwxgtk3.2-gtk3-dev libwxgtk3.0-gtk3-dev libwxgtk3.2-dev libwxgtk3.0-dev)
    message(STATUS "  trying: ${_pkg}")
    _conceal_run_elevated(_rv ${APT_GET_EXECUTABLE} install -y -qq ${_pkg})
    if(_rv EQUAL 0)
      return()
    endif()
  endforeach()
endfunction()

function(_conceal_install_wxwidgets_dnf)
  find_program(DNF_EXECUTABLE dnf)
  find_program(YUM_EXECUTABLE yum)
  set(_pm "")
  if(DNF_EXECUTABLE)
    set(_pm ${DNF_EXECUTABLE})
  elseif(YUM_EXECUTABLE)
    set(_pm ${YUM_EXECUTABLE})
  else()
    return()
  endif()

  message(STATUS "RPM-based distro — installing wxGTK-devel via ${_pm}...")
  _conceal_run_elevated(_rv ${_pm} install -y wxGTK3-devel)
  if(NOT _rv EQUAL 0)
    _conceal_run_elevated(_rv ${_pm} install -y wxGTK-devel)
  endif()
endfunction()

function(_conceal_install_wxwidgets_pacman)
  find_program(PACMAN_EXECUTABLE pacman)
  if(NOT PACMAN_EXECUTABLE)
    return()
  endif()
  if(NOT EXISTS "/etc/arch-release")
    return()
  endif()

  message(STATUS "Arch Linux detected — installing wxwidgets-gtk3 via pacman...")
  _conceal_run_elevated(_rv ${PACMAN_EXECUTABLE} -S --noconfirm --needed wxwidgets-gtk3)
endfunction()

function(_conceal_install_wxwidgets_brew)
  if(NOT APPLE)
    return()
  endif()
  find_program(BREW_EXECUTABLE brew)
  if(NOT BREW_EXECUTABLE)
    return()
  endif()

  message(STATUS "macOS — installing wxwidgets via Homebrew...")
  execute_process(
    COMMAND ${BREW_EXECUTABLE} install wxwidgets
    RESULT_VARIABLE _rv
  )
endfunction()

function(_conceal_install_wxwidgets_deps)
  if(WIN32)
    message(STATUS "Windows: install wxWidgets manually and set wxWidgets_ROOT_DIR if needed.")
    return()
  endif()

  if(APPLE)
    _conceal_install_wxwidgets_brew()
    return()
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    _conceal_install_wxwidgets_apt()
    _conceal_install_wxwidgets_dnf()
    _conceal_install_wxwidgets_pacman()
  endif()
endfunction()

function(_conceal_wxwidgets_manual_hint)
  message(STATUS "")
  message(STATUS "Install wxWidgets 3.x development files, then re-run cmake. Examples:")
  message(STATUS "  Debian/Ubuntu: sudo apt-get install -y libwxgtk3.2-gtk3-dev")
  message(STATUS "  Fedora:        sudo dnf install -y wxGTK3-devel")
  message(STATUS "  Arch:          sudo pacman -S --needed wxwidgets-gtk3")
  message(STATUS "  macOS:         brew install wxwidgets")
  message(STATUS "  Or disable auto-install: -DMIGRATION_GUI_AUTO_INSTALL_DEPS=OFF")
  message(STATUS "")
endfunction()

function(_conceal_wxwidgets_mingw_link_libs out_var)
  find_program(_wxConfigExe
    NAMES wx-config-static wx-config-3.2 wx-config
    HINTS "$ENV{MINGW_PREFIX}/bin" "/mingw64/bin")
  if(NOT _wxConfigExe)
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  set(_wxConfigArgs --unicode --release --libs core,base)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_wxConfigArgs --unicode --debug --libs core,base)
  endif()

  execute_process(
    COMMAND ${_wxConfigExe} ${_wxConfigArgs}
    OUTPUT_VARIABLE _wxLibs
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _wxErr
    RESULT_VARIABLE _wxRv)
  if(NOT _wxRv EQUAL 0 OR NOT _wxLibs)
    message(STATUS "wx-config failed (${_wxConfigExe}): ${_wxErr}")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()

  separate_arguments(_wxLibList NATIVE_COMMAND "${_wxLibs}")
  set(${out_var} "${_wxLibList}" PARENT_SCOPE)
endfunction()

# Macro (not function): find_package + include(wxWidgets_USE_FILE) must set
# wxWidgets_LIBRARIES in the caller's scope for target_link_libraries to work.
macro(conceal_find_or_install_wxwidgets)
  set(wxWidgets_USE_FILE "${wxWidgets_USE_FILE}" CACHE FILEPATH "" FORCE)

  if(WIN32)
    if(NOT wxWidgets_ROOT_DIR AND DEFINED ENV{WXWIN})
      set(wxWidgets_ROOT_DIR "$ENV{WXWIN}" CACHE PATH "wxWidgets root directory" FORCE)
    endif()
    if(wxWidgets_ROOT_DIR AND NOT wxWidgets_LIB_DIR)
      if(EXISTS "${wxWidgets_ROOT_DIR}/lib/vc14x_x64_dll")
        set(wxWidgets_LIB_DIR "${wxWidgets_ROOT_DIR}/lib/vc14x_x64_dll" CACHE PATH "wxWidgets library directory" FORCE)
      elseif(EXISTS "${wxWidgets_ROOT_DIR}/lib/vc14x_dll")
        set(wxWidgets_LIB_DIR "${wxWidgets_ROOT_DIR}/lib/vc14x_dll" CACHE PATH "wxWidgets library directory" FORCE)
      endif()
    endif()
  endif()

  find_package(wxWidgets 3.0 QUIET COMPONENTS core base)

  if(wxWidgets_FOUND)
    message(STATUS "wxWidgets ${wxWidgets_VERSION_STRING} found")
    include(${wxWidgets_USE_FILE})
    if(MINGW)
      _conceal_wxwidgets_mingw_link_libs(_mingwWxLibs)
      if(_mingwWxLibs)
        set(wxWidgets_LIBRARIES ${_mingwWxLibs})
      endif()
    endif()
  elseif(MIGRATION_GUI_AUTO_INSTALL_DEPS)
    message(STATUS "wxWidgets not found — attempting to install dependencies...")
    _conceal_install_wxwidgets_deps()
    unset(wxWidgets_DIR CACHE)
    find_package(wxWidgets 3.0 QUIET COMPONENTS core base)
    if(wxWidgets_FOUND)
      message(STATUS "wxWidgets ${wxWidgets_VERSION_STRING} found after install attempt")
      include(${wxWidgets_USE_FILE})
      if(MINGW)
        _conceal_wxwidgets_mingw_link_libs(_mingwWxLibs)
        if(_mingwWxLibs)
          set(wxWidgets_LIBRARIES ${_mingwWxLibs})
        endif()
      endif()
    endif()
  endif()

  if(NOT wxWidgets_FOUND)
    _conceal_wxwidgets_manual_hint()
    message(FATAL_ERROR
            "wxWidgets 3.0+ (core, base) is required for BUILD_MIGRATION_GUI=ON. "
            "Install the packages above or set wxWidgets_ROOT_DIR.")
  endif()
endmacro()
