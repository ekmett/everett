# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds the isolated offset-selection comparison and correctness checks.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(everett_select_compare LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/everett-experiment.cmake")
set(EVERETT_SUX_ROOT "" CACHE PATH "External pinned Sux checkout")
if(NOT EXISTS "${EVERETT_SUX_ROOT}/sux/bits/SimpleSelect.hpp")
  message(FATAL_ERROR "Set EVERETT_SUX_ROOT to the separately fetched pinned Sux checkout")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_sux.py"
  --source "${EVERETT_SUX_ROOT}" --output "${CMAKE_BINARY_DIR}/external"
  COMMAND_ERROR_IS_FATAL ANY)
add_executable(select_compare prototype.cc)
target_include_directories(select_compare SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/external")
everett_experiment_target(select_compare)
target_include_directories(select_compare PRIVATE ../../include)
target_include_directories(select_compare SYSTEM PRIVATE "${EVERETT_SUX_ROOT}")
target_compile_options(select_compare PRIVATE -Wall -Wextra -Wpedantic)
option(SELECT_SANITIZERS "Instrument the optional correctness harness" OFF)
if(SELECT_SANITIZERS)
  target_compile_options(select_compare PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(select_compare PRIVATE -fsanitize=address,undefined)
endif()
enable_testing()
add_test(NAME select_correctness COMMAND select_compare check)
