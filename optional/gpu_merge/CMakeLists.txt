# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(everett_gpu_merge_experiment LANGUAGES CXX)
if(NOT APPLE)
  message(FATAL_ERROR "This optional host prototype currently requires macOS Metal")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(EVERETT_DXC dxc REQUIRED)
find_program(EVERETT_SPIRV_VAL spirv-val REQUIRED)
find_program(EVERETT_SPIRV_CROSS spirv-cross REQUIRED)
file(GLOB_RECURSE EVERETT_GPU_CPU_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include/everett/*.h")
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/prototype" "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
    --build "${CMAKE_CURRENT_BINARY_DIR}"
    --dxc "${EVERETT_DXC}" --spirv-val "${EVERETT_SPIRV_VAL}" --spirv-cross "${EVERETT_SPIRV_CROSS}"
  DEPENDS build.py shader_compile.py prototype.mm adversarial.h kernels.hlsl prepared_input.h
    parse_input.hlsl prefix_owner.hlsl prefix_cache.hlsl emit_word.hlsl
    ef_input_test.h ef_output.hlsl ef_output_test.h emit_word_test.h rank_shared.h cutover_cases.h gpu_sort.h
    index_rank.hlsl index_rank_test.h collision_rank.hlsl collision_rank_test.h rank512.hlsl
    ${EVERETT_GPU_CPU_HEADERS}
  VERBATIM)
add_custom_target(gpu_merge ALL DEPENDS
  "${CMAKE_CURRENT_BINARY_DIR}/prototype" "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib")
enable_testing()
add_executable(gpu_cutover_test cutover_test.cc)
target_compile_features(gpu_cutover_test PRIVATE cxx_std_20)
target_compile_options(gpu_cutover_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(gpu_cutover_test PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME gpu_cutover_selector COMMAND gpu_cutover_test)
add_test(NAME gpu_cutover_calibration
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cutover_calibration_test.py")
add_test(NAME gpu_shader_toolchain
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/shader_compile_test.py")
add_test(NAME gpu_collision_scalar
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/collision_rank_scalar.py")
add_test(NAME gpu_rank_boundaries COMMAND "${CMAKE_CURRENT_BINARY_DIR}/prototype"
  "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib" "${CMAKE_CURRENT_BINARY_DIR}/rank-check" quick)
add_test(NAME gpu_merge_adversarial COMMAND "${CMAKE_CURRENT_BINARY_DIR}/prototype"
  "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib" "${CMAKE_CURRENT_BINARY_DIR}/merge-check" complete)

add_test(NAME gpu_merge_optimized_adversarial COMMAND "${CMAKE_CURRENT_BINARY_DIR}/prototype"
  "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib" "${CMAKE_CURRENT_BINARY_DIR}/optimized-check" optimized)
# Run correctness tests serially; benchmark timings require an otherwise idle device.
set_tests_properties(gpu_rank_boundaries gpu_merge_adversarial gpu_merge_optimized_adversarial
  PROPERTIES RUN_SERIAL TRUE)

foreach(layout IN ITEMS 512 2048)
  foreach(path IN ITEMS direct tiled compact)
    set(mode "collision${layout}")
    if(NOT path STREQUAL "direct")
      string(APPEND mode "-${path}")
    endif()
    add_test(NAME "gpu_merge_${mode}" COMMAND "${CMAKE_CURRENT_BINARY_DIR}/prototype"
      "${CMAKE_CURRENT_BINARY_DIR}/kernels.metallib" "${CMAKE_CURRENT_BINARY_DIR}/${mode}-check" "${mode}")
    set_tests_properties("gpu_merge_${mode}" PROPERTIES RUN_SERIAL TRUE)
  endforeach()
endforeach()
