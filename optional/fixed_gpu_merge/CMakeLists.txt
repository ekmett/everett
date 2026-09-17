# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds and checks the optional fixed-key Metal merge experiment.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(everett_fixed_gpu_merge LANGUAGES CXX)
if(NOT APPLE)
  message(FATAL_ERROR "The isolated fixed-key merge experiment currently requires Metal")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/everett-experiment.cmake")
everett_experiment_objcxx()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(everett_manifest_native_args)
if(NOT everett_experiment_arch STREQUAL "SCALAR")
  list(APPEND everett_manifest_native_args --native-archive "$<TARGET_FILE:everett::${everett_experiment_name}>")
endif()
find_program(EVERETT_DXC NAMES dxc REQUIRED)
find_program(EVERETT_SPIRV_VAL NAMES spirv-val REQUIRED)
find_program(EVERETT_SPIRV_CROSS NAMES spirv-cross REQUIRED)
file(GLOB helper_shaders CONFIGURE_DEPENDS "../gpu_merge/*.hlsl")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/kernels.metallib"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
    --build "${CMAKE_BINARY_DIR}" --dxc "${EVERETT_DXC}"
    --spirv-val "${EVERETT_SPIRV_VAL}" --spirv-cross "${EVERETT_SPIRV_CROSS}"
  DEPENDS build.py kernels.hlsl ${helper_shaders} ../gpu_merge/shader_compile.py
  VERBATIM)
add_custom_target(fixed_gpu_merge_shaders DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib")
add_executable(prototype prototype.mm)
everett_experiment_metal(prototype)
add_dependencies(prototype fixed_gpu_merge_shaders)
add_custom_command(TARGET prototype POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
    --build "${CMAKE_CURRENT_BINARY_DIR}" --manifest-only
    --archive "$<TARGET_FILE:everett::everett>" --simd-archive "$<TARGET_FILE:simd::simd>" ${everett_manifest_native_args}
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/source-hashes.json" VERBATIM)
add_custom_target(fixed_gpu_merge ALL DEPENDS prototype)
enable_testing()
add_test(NAME fixed_gpu_complete COMMAND "${CMAKE_BINARY_DIR}/prototype"
  "${CMAKE_BINARY_DIR}/kernels.metallib" "${CMAKE_BINARY_DIR}/scratch" quick)
