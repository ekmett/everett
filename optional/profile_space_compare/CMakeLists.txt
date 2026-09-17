# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds a complete byte/bit native and fractional-index space comparison.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(everett_profile_space_compare LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/everett-experiment.cmake")
set(EVERETT_HEADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../include" CACHE PATH "Everett include directory")
option(EVERETT_SPACE_SANITIZE "Enable address/undefined behavior sanitizers" OFF)
add_executable(profile-space prototype.cc)
target_include_directories(profile-space PRIVATE "${EVERETT_HEADER_ROOT}")
everett_experiment_target(profile-space)
target_compile_options(profile-space PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(EVERETT_SPACE_SANITIZE)
  target_compile_options(profile-space PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(profile-space PRIVATE -fsanitize=address,undefined)
else()
  target_compile_options(profile-space PRIVATE -O2)
endif()
enable_testing()
foreach(mode IN ITEMS raw-byte raw-bit typed-byte typed-bit typed-byte-known)
  add_test(NAME "space_${mode}" COMMAND profile-space "${CMAKE_BINARY_DIR}/check-${mode}" "${mode}" 1024)
  set_tests_properties("space_${mode}" PROPERTIES RUN_SERIAL TRUE)
endforeach()
