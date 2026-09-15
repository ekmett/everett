##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

cmake_minimum_required(VERSION 3.20)
project(diet VERSION 0.1.0 DESCRIPTION "Experimental persistent string storage" LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(diet_top_level ON)
else()
  set(diet_top_level OFF)
endif()
option(DIET_BUILD_TESTS "Build Diet's tests" ${diet_top_level})
option(DIET_ENABLE_SQLITE "Build the optional SQLite catalog component" OFF)
option(DIET_BUILD_DOCS "Build and check Diet's Doxygen documentation" OFF)
option(DIET_SANITIZERS "Enable address and undefined-behavior sanitizers on tests" OFF)
option(DIET_USE_CCACHE "Use ccache when compiling Diet's tests" OFF)
if(DIET_SANITIZERS AND NOT DIET_BUILD_TESTS)
  message(FATAL_ERROR "DIET_SANITIZERS requires DIET_BUILD_TESTS=ON; only test targets are instrumented")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
foreach(diet_install_dir IN ITEMS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_DATADIR)
  if(IS_ABSOLUTE "${${diet_install_dir}}")
    message(FATAL_ERROR "${diet_install_dir} must be relative for a relocatable Diet package")
  endif()
endforeach()

add_library(diet INTERFACE)
add_library(diet::diet ALIAS diet)
target_compile_features(diet INTERFACE cxx_std_20)
target_include_directories(diet INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(DIET_ENABLE_SQLITE)
  find_package(SQLite3 3.51.3 REQUIRED)
  add_library(diet_sqlite INTERFACE)
  add_library(diet::sqlite ALIAS diet_sqlite)
  set_target_properties(diet_sqlite PROPERTIES EXPORT_NAME sqlite)
  target_link_libraries(diet_sqlite INTERFACE diet::diet SQLite::SQLite3)
endif()

set(diet_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/diet")
configure_package_config_file(cmake/diet-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/diet-config.cmake"
  INSTALL_DESTINATION "${diet_config_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/diet-config-version.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
install(TARGETS diet EXPORT diet-targets)
install(DIRECTORY include/diet DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT diet-targets FILE diet-targets.cmake
  NAMESPACE diet:: DESTINATION "${diet_config_dir}")
if(DIET_ENABLE_SQLITE)
  install(TARGETS diet_sqlite EXPORT diet-sqlite-targets)
  install(EXPORT diet-sqlite-targets FILE diet-sqlite-targets.cmake
    NAMESPACE diet:: DESTINATION "${diet_config_dir}")
endif()
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/diet-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/diet-config-version.cmake"
  DESTINATION "${diet_config_dir}")
install(FILES LICENSE THIRD_PARTY.md DESTINATION "${CMAKE_INSTALL_DATADIR}/diet")
install(DIRECTORY LICENSES/ DESTINATION "${CMAKE_INSTALL_DATADIR}/diet/LICENSES")
install(FILES third_party/fast-crc32/LICENSE.md third_party/fast-crc32/LICENSE.MIT.md
  third_party/fast-crc32/LICENSE.zlib.md third_party/fast-crc32/UPSTREAM.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/diet/third_party/fast-crc32")

if(DIET_BUILD_TESTS)
  enable_testing()
  include(cmake/diet-tests.cmake)
endif()

if(DIET_BUILD_DOCS)
  include(cmake/diet-docs.cmake)
endif()

##
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures the Diet build.
