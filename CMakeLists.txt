##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

cmake_minimum_required(VERSION 3.20)
project(everett VERSION 0.1.0 DESCRIPTION "Experimental persistent string storage" LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(everett_top_level ON)
else()
  set(everett_top_level OFF)
endif()
option(EVERETT_BUILD_TESTS "Build Everett's tests" ${everett_top_level})
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

add_library(everett INTERFACE)
add_library(everett::everett ALIAS everett)
target_compile_features(everett INTERFACE cxx_std_20)
target_include_directories(everett INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set(everett_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/everett")
configure_package_config_file(cmake/everett-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/everett-config.cmake"
  INSTALL_DESTINATION "${everett_config_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/everett-config-version.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
install(TARGETS everett EXPORT everett-targets)
install(DIRECTORY include/everett DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT everett-targets FILE everett-targets.cmake
  NAMESPACE everett:: DESTINATION "${everett_config_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/everett-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/everett-config-version.cmake"
  DESTINATION "${everett_config_dir}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/everett")
install(DIRECTORY LICENSES/ DESTINATION "${CMAKE_INSTALL_DATADIR}/everett/LICENSES")

if(EVERETT_BUILD_TESTS)
  enable_testing()
  include(cmake/everett-tests.cmake)
endif()

if(EVERETT_BUILD_DOCS)
  include(cmake/everett-docs.cmake)
endif()

##
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures the Everett build.
