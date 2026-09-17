##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures the Everett build.

cmake_minimum_required(VERSION 4.4)
project(everett VERSION 0.1.0 DESCRIPTION "Experimental persistent string storage" LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(everett_top_level ON)
else()
  set(everett_top_level OFF)
endif()
option(EVERETT_BUILD_TESTS "Build Everett's tests" ${everett_top_level})
option(EVERETT_ENABLE_SQLITE "Build the optional SQLite catalog component" OFF)
option(EVERETT_BUILD_DOCS "Build and check Everett's Doxygen documentation" OFF)
option(EVERETT_SANITIZERS "Enable address and undefined-behavior sanitizers on tests" OFF)
option(EVERETT_USE_CCACHE "Use ccache when compiling Everett's tests" OFF)
if(EVERETT_SANITIZERS AND NOT EVERETT_BUILD_TESTS)
  message(FATAL_ERROR "EVERETT_SANITIZERS requires EVERETT_BUILD_TESTS=ON; only test targets are instrumented")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
foreach(everett_install_dir IN ITEMS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_DATADIR)
  if(IS_ABSOLUTE "${${everett_install_dir}}")
    message(FATAL_ERROR "${everett_install_dir} must be relative for a relocatable Everett package")
  endif()
endforeach()

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Everett's C++26 modules require Ninja or Ninja Multi-Config")
endif()
find_package(simd 0.0.1 CONFIG REQUIRED COMPONENTS simd)
find_package(Threads REQUIRED)

set(everett_available_profiles)
foreach(profile IN ITEMS NEON AVX2 AVX512)
  string(TOLOWER "${profile}" name)
  if(TARGET simd::${name})
    list(APPEND everett_available_profiles "${profile}")
  endif()
endforeach()
set(EVERETT_PROFILES "${everett_available_profiles}" CACHE STRING "Explicit native module profiles")
list(REMOVE_DUPLICATES EVERETT_PROFILES)

function(everett_module_settings target)
  target_compile_features(${target} PUBLIC cxx_std_26)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(${target} PUBLIC simd::simd Threads::Threads)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
endfunction()

add_library(everett STATIC src/crc32c.cc)
add_library(everett::everett ALIAS everett)
everett_module_settings(everett)
target_sources(everett PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES
  BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/modules" FILES modules/everett.ccm)

set(everett_module_targets everett)
foreach(profile IN LISTS EVERETT_PROFILES)
  string(TOLOWER "${profile}" name)
  if(NOT profile IN_LIST everett_available_profiles)
    message(FATAL_ERROR "Everett profile ${profile} is unavailable in the selected simd package")
  endif()
  add_library(everett_${name} STATIC)
  add_library(everett::${name} ALIAS everett_${name})
  everett_module_settings(everett_${name})
  set_target_properties(everett_${name} PROPERTIES EXPORT_NAME ${name})
  target_link_libraries(everett_${name} PUBLIC everett simd::${name})
  simd_target_profile(everett_${name} "${profile}")
  target_sources(everett_${name} PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/modules" FILES modules/everett.${name}.ccm)
  list(APPEND everett_module_targets everett_${name})
endforeach()

if(EVERETT_ENABLE_SQLITE)
  find_package(SQLite3 3.51.3 REQUIRED)
  add_library(everett_sqlite STATIC)
  add_library(everett::sqlite ALIAS everett_sqlite)
  everett_module_settings(everett_sqlite)
  set_target_properties(everett_sqlite PROPERTIES EXPORT_NAME sqlite)
  target_link_libraries(everett_sqlite PUBLIC everett::everett SQLite3::SQLite3)
  target_sources(everett_sqlite PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/modules" FILES modules/everett.sqlite.ccm)
endif()

set(everett_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/everett")
configure_package_config_file(cmake/everett-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/everett-config.cmake"
  INSTALL_DESTINATION "${everett_config_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/everett-config-version.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(TARGETS ${everett_module_targets} EXPORT everett-targets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  FILE_SET cxx_modules DESTINATION "${CMAKE_INSTALL_LIBDIR}/everett/modules")
install(DIRECTORY include/everett DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT everett-targets FILE everett-targets.cmake CXX_MODULES_DIRECTORY cxx-modules
  NAMESPACE everett:: DESTINATION "${everett_config_dir}")
if(EVERETT_ENABLE_SQLITE)
  install(TARGETS everett_sqlite EXPORT everett-sqlite-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET cxx_modules DESTINATION "${CMAKE_INSTALL_LIBDIR}/everett/modules")
  install(EXPORT everett-sqlite-targets FILE everett-sqlite-targets.cmake CXX_MODULES_DIRECTORY cxx-modules
    NAMESPACE everett:: DESTINATION "${everett_config_dir}")
endif()
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/everett-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/everett-config-version.cmake"
  DESTINATION "${everett_config_dir}")
install(FILES LICENSE THIRD_PARTY.md DESTINATION "${CMAKE_INSTALL_DATADIR}/everett")
install(DIRECTORY LICENSES/ DESTINATION "${CMAKE_INSTALL_DATADIR}/everett/LICENSES")
install(FILES third_party/fast-crc32/LICENSE.md third_party/fast-crc32/LICENSE.MIT.md
  third_party/fast-crc32/LICENSE.zlib.md third_party/fast-crc32/UPSTREAM.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/everett/third_party/fast-crc32")

if(EVERETT_BUILD_TESTS)
  enable_testing()
  include(cmake/everett-tests.cmake)
endif()

if(EVERETT_BUILD_DOCS)
  include(cmake/everett-docs.cmake)
endif()
