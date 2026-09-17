# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds and checks matched fixed-key and front-coded Metal measurements.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 4.4)
project(everett_fixed_kv_compare LANGUAGES CXX)
if(NOT APPLE)
  message(FATAL_ERROR "The matched-format experiment requires Metal")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/everett-experiment.cmake")
everett_experiment_objcxx()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(everett_manifest_native_args)
if(NOT everett_experiment_arch STREQUAL "SCALAR")
  list(APPEND everett_manifest_native_args --native-archive "$<TARGET_FILE:everett::${everett_experiment_name}>")
endif()
set(EVERETT_KV_METALLIB "" CACHE FILEPATH "Compiled optional/gpu_merge kernels")
set(EVERETT_FIXED_METALLIB "" CACHE FILEPATH "Compiled optional/fixed_gpu_merge kernels")
foreach(path IN ITEMS EVERETT_KV_METALLIB EVERETT_FIXED_METALLIB)
  if(NOT EXISTS "${${path}}")
    message(FATAL_ERROR "Provide ${path} from the corresponding standalone build")
  endif()
endforeach()
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "../../include/everett/*.h" "../../include/everett/*.inc"
  "../gpu_merge/*.h" "../fixed_gpu_merge/*.h")
foreach(mode kv fixed)
  add_executable(${mode} ${mode}.mm)
  everett_experiment_metal(${mode})
endforeach()
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/manifest.json"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py" --build "${CMAKE_BINARY_DIR}"
    --kv-metallib "${EVERETT_KV_METALLIB}" --fixed-metallib "${EVERETT_FIXED_METALLIB}"
    --archive "$<TARGET_FILE:everett::everett>" --simd-archive "$<TARGET_FILE:simd::simd>" ${everett_manifest_native_args}
  DEPENDS kv fixed build.py cases.h ../gpu_merge/prototype.mm ../fixed_gpu_merge/prototype.mm
    "${EVERETT_KV_METALLIB}" "${EVERETT_FIXED_METALLIB}" ${sources}
  BYPRODUCTS "${CMAKE_BINARY_DIR}/kv.metallib" "${CMAKE_BINARY_DIR}/fixed.metallib" VERBATIM)
add_custom_target(matched_formats ALL DEPENDS "${CMAKE_BINARY_DIR}/manifest.json")
enable_testing()
foreach(mode IN ITEMS fixed kv)
  foreach(index IN ITEMS 0 3 6 9)
    add_test(NAME "matched_${mode}_${index}" COMMAND "${CMAKE_BINARY_DIR}/${mode}"
      "${CMAKE_BINARY_DIR}/${mode}.metallib" "${CMAKE_BINARY_DIR}/check-${mode}-${index}" "${index}" 1)
    set_tests_properties("matched_${mode}_${index}" PROPERTIES RUN_SERIAL TRUE)
  endforeach()
endforeach()
