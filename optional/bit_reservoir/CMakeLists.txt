# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds the isolated bit-reservoir experiment against a chosen header tree.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(everett_bit_reservoir LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/everett-experiment.cmake")
if(NOT UNIX)
  message(FATAL_ERROR "The experiment and guard-page tests currently require POSIX mmap")
endif()
set(EVERETT_HEADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../include" CACHE PATH "Frozen Everett include directory")
option(EVERETT_RESERVOIR_SANITIZE "Enable address/undefined behavior sanitizers" OFF)
add_executable(reservoir prototype.cc)
add_executable(reservoir-tests ../../tests/sort_bit_reservoir.cc)
foreach(target IN ITEMS reservoir reservoir-tests)
  target_include_directories(${target} PRIVATE "${EVERETT_HEADER_ROOT}")
  everett_experiment_target(${target})
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(EVERETT_RESERVOIR_SANITIZE)
    target_compile_options(${target} PRIVATE -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  else()
    target_compile_options(${target} PRIVATE -O3)
  endif()
endforeach()
enable_testing()
add_test(NAME reservoir_fixtures COMMAND reservoir "${CMAKE_BINARY_DIR}/fixtures" 128 1 1)
add_test(NAME reservoir_differential COMMAND reservoir-tests)
