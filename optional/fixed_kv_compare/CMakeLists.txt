# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Builds and checks matched fixed-key and front-coded Metal measurements.
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(everett_fixed_kv_compare LANGUAGES NONE)
if(NOT APPLE)
  message(FATAL_ERROR "The matched-format experiment requires Metal")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EVERETT_KV_METALLIB "" CACHE FILEPATH "Compiled optional/gpu_merge kernels")
set(EVERETT_FIXED_METALLIB "" CACHE FILEPATH "Compiled optional/fixed_gpu_merge kernels")
foreach(path IN ITEMS EVERETT_KV_METALLIB EVERETT_FIXED_METALLIB)
  if(NOT EXISTS "${${path}}")
    message(FATAL_ERROR "Provide ${path} from the corresponding standalone build")
  endif()
endforeach()
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "../../include/everett/*.h" "../../include/everett/*.inc"
  "../gpu_merge/*.h" "../fixed_gpu_merge/*.h")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/kv" "${CMAKE_BINARY_DIR}/fixed"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py" --build "${CMAKE_BINARY_DIR}"
    --kv-metallib "${EVERETT_KV_METALLIB}" --fixed-metallib "${EVERETT_FIXED_METALLIB}"
  DEPENDS build.py cases.h kv.mm fixed.mm ../gpu_merge/prototype.mm ../fixed_gpu_merge/prototype.mm
    "${EVERETT_KV_METALLIB}" "${EVERETT_FIXED_METALLIB}" ${sources} VERBATIM)
add_custom_target(matched_formats ALL DEPENDS "${CMAKE_BINARY_DIR}/kv" "${CMAKE_BINARY_DIR}/fixed")
enable_testing()
foreach(mode IN ITEMS fixed kv)
  foreach(index IN ITEMS 0 3 6 9)
    add_test(NAME "matched_${mode}_${index}" COMMAND "${CMAKE_BINARY_DIR}/${mode}"
      "${CMAKE_BINARY_DIR}/${mode}.metallib" "${CMAKE_BINARY_DIR}/check-${mode}-${index}" "${index}" 1)
    set_tests_properties("matched_${mode}_${index}" PROPERTIES RUN_SERIAL TRUE)
  endforeach()
endforeach()
