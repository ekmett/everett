##
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures a separate Diet package consumer.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

cmake_minimum_required(VERSION 3.20)
project(diet_consumer LANGUAGES CXX)

if(DIET_PACKAGE_MODE STREQUAL "installed")
  find_package(diet 0.1 CONFIG REQUIRED)
  get_target_property(diet_imported diet::diet IMPORTED)
  if(NOT diet_imported)
    message(FATAL_ERROR "Expected an installed imported target")
  endif()
  get_target_property(diet_includes diet::diet INTERFACE_INCLUDE_DIRECTORIES)
  foreach(path IN LISTS diet_includes)
    cmake_path(IS_PREFIX DIET_EXPECTED_PREFIX "${path}" NORMALIZE under_prefix)
    if(NOT under_prefix)
      message(FATAL_ERROR "Installed include path escaped relocated prefix: ${path}")
    endif()
  endforeach()
elseif(DIET_PACKAGE_MODE STREQUAL "embedded")
  add_subdirectory("${DIET_SOURCE_DIR}" diet)
  if(DIET_BUILD_TESTS OR TARGET diet_test_rank)
    message(FATAL_ERROR "Embedded Diet unexpectedly enabled its tests")
  endif()
else()
  message(FATAL_ERROR "Expected installed or embedded package mode")
endif()

# Development tooling must not become a consumer's compile/link requirement.
foreach(property IN ITEMS INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS)
  get_target_property(value diet::diet "${property}")
  if(value)
    message(FATAL_ERROR "Diet leaked development options through ${property}: ${value}")
  endif()
endforeach()
add_executable(diet_consumer main.cc crc32c.cc)
target_link_libraries(diet_consumer PRIVATE diet::diet)
set_target_properties(diet_consumer PROPERTIES CXX_EXTENSIONS OFF)
enable_testing()
add_test(NAME diet.consumer COMMAND diet_consumer)
