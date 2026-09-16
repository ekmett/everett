# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures the standalone byte-profile GPU merge experiment.
#
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(everett_byte_gpu_merge LANGUAGES CXX)
if(NOT APPLE)
  message(FATAL_ERROR "This optional host driver requires macOS Metal")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(EVERETT_DXC dxc REQUIRED)
find_program(EVERETT_SPIRV_VAL spirv-val REQUIRED)
find_program(EVERETT_SPIRV_CROSS spirv-cross REQUIRED)
file(GLOB_RECURSE EVERETT_HEADERS CONFIGURE_DEPENDS "../../include/everett/*.h" "../../include/everett/*.inc")
file(GLOB EVERETT_SHARED_SHADERS CONFIGURE_DEPENDS "../gpu_merge/*.hlsl" "../gpu_merge/*.h")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/prototype" "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py" --build "${CMAKE_CURRENT_BINARY_DIR}"
    --dxc "${EVERETT_DXC}" --spirv-val "${EVERETT_SPIRV_VAL}" --spirv-cross "${EVERETT_SPIRV_CROSS}"
  DEPENDS build.py kernels.hlsl prototype.mm prepared_input.h metal.h ../gpu_merge/shader_compile.py
    ${EVERETT_HEADERS} ${EVERETT_SHARED_SHADERS}
  VERBATIM)
add_custom_target(byte_gpu_merge ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/prototype" "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib")
enable_testing()
add_test(NAME byte_gpu_merge_complete COMMAND "${CMAKE_CURRENT_BINARY_DIR}/prototype"
  "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib" "${CMAKE_CURRENT_BINARY_DIR}/checks" check)
set_tests_properties(byte_gpu_merge_complete PROPERTIES RUN_SERIAL TRUE)
