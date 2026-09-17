# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0

# Standalone experiments use the same compiled library and toolchain as clients.
include_guard(GLOBAL)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Experiment build configuration" FORCE)
endif()
option(EVERETT_EXPERIMENT_INSTALLED "Use an installed Everett package for this experiment" OFF)
if(NOT TARGET everett::everett)
  if(EVERETT_EXPERIMENT_INSTALLED)
    find_package(everett CONFIG REQUIRED)
  else()
    set(EVERETT_BUILD_TESTS OFF)
    set(EVERETT_BUILD_DOCS OFF)
    set(EVERETT_ENABLE_SQLITE OFF)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_BINARY_DIR}/everett" EXCLUDE_FROM_ALL)
  endif()
endif()

# Objective-C++ is a textual bridge to native framework APIs. Use the C++
# compiler and standard-library flags already chosen by the toolchain.
macro(everett_experiment_objcxx)
  if(NOT DEFINED CMAKE_OBJCXX_COMPILER)
    set(CMAKE_OBJCXX_COMPILER "${CMAKE_CXX_COMPILER}" CACHE FILEPATH "Objective-C++ compiler")
  endif()
  if(NOT DEFINED CMAKE_OBJCXX_FLAGS_INIT)
    set(CMAKE_OBJCXX_FLAGS_INIT "${CMAKE_CXX_FLAGS}")
  endif()
  enable_language(OBJCXX)
  if(NOT CMAKE_OBJCXX_COMPILER_ID STREQUAL CMAKE_CXX_COMPILER_ID OR
     NOT CMAKE_OBJCXX_COMPILER_VERSION VERSION_EQUAL CMAKE_CXX_COMPILER_VERSION)
    message(FATAL_ERROR "Everett's Objective-C++ bridge requires the same compiler version as C++")
  endif()
endmacro()

function(everett_experiment_target target)
  target_link_libraries(${target} PRIVATE everett::everett)
  target_compile_features(${target} PRIVATE cxx_std_26)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF
    OBJCXX_STANDARD 26 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
  target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fms-extensions>")
endfunction()

function(everett_experiment_metal target)
  everett_experiment_target(${target})
  target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
    -Wall -Wextra -Wpedantic -Werror)
  target_link_libraries(${target} PRIVATE "-framework Foundation" "-framework Metal")
endfunction()
