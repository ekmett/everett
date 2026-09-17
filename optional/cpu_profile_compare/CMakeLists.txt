# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds the isolated matched CPU profile experiment against a chosen header tree.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(everett_cpu_profile_compare LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/everett-experiment.cmake")
if(NOT UNIX)
  message(FATAL_ERROR "This experiment currently uses POSIX mmap")
endif()
set(EVERETT_HEADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../include" CACHE PATH "Frozen Everett include directory")
option(EVERETT_PROFILE_SANITIZE "Enable address/undefined behavior sanitizers" OFF)
add_executable(cpu-profile prototype.cc)
target_include_directories(cpu-profile PRIVATE "${EVERETT_HEADER_ROOT}")
everett_experiment_target(cpu-profile)
target_compile_options(cpu-profile PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(EVERETT_PROFILE_SANITIZE)
  target_compile_options(cpu-profile PRIVATE -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(cpu-profile PRIVATE -fsanitize=address,undefined)
else()
  target_compile_options(cpu-profile PRIVATE -O3)
endif()
enable_testing()
foreach(mode IN ITEMS raw-bit raw-byte typed-bit typed-byte)
  foreach(index IN ITEMS 0 3 6 9)
    add_test(NAME "cpu_${mode}_${index}" COMMAND cpu-profile "${mode}"
      "${CMAKE_BINARY_DIR}/check-${mode}-${index}" "${index}" 1)
    set_tests_properties("cpu_${mode}_${index}" PROPERTIES RUN_SERIAL TRUE)
  endforeach()
endforeach()
