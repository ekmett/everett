# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds and checks the optional fixed-key Metal merge experiment.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(everett_fixed_gpu_merge LANGUAGES NONE)
if(NOT APPLE)
  message(FATAL_ERROR "The isolated fixed-key merge experiment currently requires Metal")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(EVERETT_DXC NAMES dxc REQUIRED)
find_program(EVERETT_SPIRV_VAL NAMES spirv-val REQUIRED)
find_program(EVERETT_SPIRV_CROSS NAMES spirv-cross REQUIRED)
file(GLOB helper_shaders CONFIGURE_DEPENDS "../gpu_merge/*.hlsl")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/prototype" "${CMAKE_BINARY_DIR}/kernels.metallib"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
    --build "${CMAKE_BINARY_DIR}" --dxc "${EVERETT_DXC}"
    --spirv-val "${EVERETT_SPIRV_VAL}" --spirv-cross "${EVERETT_SPIRV_CROSS}"
  DEPENDS build.py prototype.mm kernels.hlsl ${helper_shaders}
    ../gpu_merge/shader_compile.py ../../include/everett/elias_fano.h
    ../../include/everett/word_view.h ../../include/everett/error_detail.h
  VERBATIM)
add_custom_target(fixed_gpu_merge ALL DEPENDS "${CMAKE_BINARY_DIR}/prototype" "${CMAKE_BINARY_DIR}/kernels.metallib")
enable_testing()
add_test(NAME fixed_gpu_complete COMMAND "${CMAKE_BINARY_DIR}/prototype"
  "${CMAKE_BINARY_DIR}/kernels.metallib" "${CMAKE_BINARY_DIR}/scratch" quick)
