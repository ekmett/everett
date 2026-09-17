##
# \file
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures a separate Everett package consumer.
#
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense

cmake_minimum_required(VERSION 4.4)
project(everett_consumer LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)

if(EVERETT_PACKAGE_MODE STREQUAL "installed")
  find_package(everett 0.1 CONFIG REQUIRED)
  get_target_property(everett_imported everett::everett IMPORTED)
  if(NOT everett_imported)
    message(FATAL_ERROR "Expected an installed imported target")
  endif()
  get_target_property(everett_includes everett::everett INTERFACE_INCLUDE_DIRECTORIES)
  foreach(path IN LISTS everett_includes)
    cmake_path(IS_PREFIX EVERETT_EXPECTED_PREFIX "${path}" NORMALIZE under_prefix)
    if(NOT under_prefix)
      message(FATAL_ERROR "Installed include path escaped relocated prefix: ${path}")
    endif()
  endforeach()
elseif(EVERETT_PACKAGE_MODE STREQUAL "embedded")
  add_subdirectory("${EVERETT_SOURCE_DIR}" everett)
  if(EVERETT_BUILD_TESTS OR TARGET everett_test_rank)
    message(FATAL_ERROR "Embedded Everett unexpectedly enabled its tests")
  endif()
else()
  message(FATAL_ERROR "Expected installed or embedded package mode")
endif()

# Development tooling must not become a consumer's compile/link requirement.
foreach(property IN ITEMS INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS)
  get_target_property(value everett::everett "${property}")
  if(value)
    message(FATAL_ERROR "Everett leaked development options through ${property}: ${value}")
  endif()
endforeach()
add_executable(everett_consumer main.cc crc32c.cc)
target_link_libraries(everett_consumer PRIVATE everett::everett)
set_target_properties(everett_consumer PROPERTIES CXX_EXTENSIONS OFF)
enable_testing()
add_test(NAME everett.consumer COMMAND everett_consumer)

foreach(profile NEON AVX2 AVX512)
  string(TOLOWER "${profile}" name)
  if(TARGET everett::${name})
    add_library(native_${name} OBJECT native.cc)
    target_link_libraries(native_${name} PRIVATE everett::${name})
    simd_target_profile(native_${name} ${profile})
    target_compile_definitions(native_${name} PRIVATE EVERETT_PACKAGE_${profile}=1)
    add_executable(native_consumer_${name} native_main.cc $<TARGET_OBJECTS:native_${name}>)
    target_compile_definitions(native_consumer_${name} PRIVATE EVERETT_PACKAGE_${profile}=1)
    target_link_libraries(native_consumer_${name} PRIVATE everett::${name})
    set_property(TARGET native_consumer_${name} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    add_test(NAME everett.consumer.${name} COMMAND native_consumer_${name})
    set_tests_properties(everett.consumer.${name} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endforeach()
