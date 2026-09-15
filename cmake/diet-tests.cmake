##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures Diet's test targets and package checks.

if(DIET_USE_CCACHE)
  find_program(diet_ccache NAMES ccache REQUIRED)
endif()

if(DIET_SANITIZERS)
  if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR "DIET_SANITIZERS requires a compiler supporting both address and undefined sanitizers")
  endif()
  # Check the compiler and runtime link together without changing global flags.
  function(diet_check_sanitizers)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" diet_sanitizers_supported)
    if(NOT diet_sanitizers_supported)
      message(FATAL_ERROR "The requested address/undefined sanitizer combination cannot compile and link")
    endif()
  endfunction()
  diet_check_sanitizers()
endif()

set(diet_test_names registry registry_compat crc32c rank groups rank_groups_builder elias_fano profile borrowed_writer profile_blob comparison_fc sampling index_builder index_builder_allocations index_pipeline query cola_index cola_frontier cola_route_reuse cola_schedule native_writer native_writer_allocations native_file_writer profile_file_output native_merge native_merge_mapped cola pins durability mapped_file files object_writer object_stream mapped_blob fridge)
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # These suites seal real mapped inputs through posix_object_ops throughout.
  # Model-ops writer tests above retain their platform-independent coverage.
  list(APPEND diet_test_names mapped_index_builder file_index_builder file_index_pipeline mapped_cola mapped_cola_builder cola_terminal)
endif()
list(APPEND diet_test_names cola_local_merge cola_local_merge_failure tap)
if(DIET_ENABLE_SQLITE)
  list(APPEND diet_test_names sqlite_catalog sqlite_catalog_adversarial sqlite_catalog_vfs sqlite_catalog_restart sqlite_catalog_timeline sqlite_catalog_streamed)
  if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND diet_test_names sqlite_catalog_cola sqlite_catalog_taps)
  endif()
endif()
foreach(diet_test IN LISTS diet_test_names)
  add_executable(diet_test_${diet_test} "${PROJECT_SOURCE_DIR}/tests/${diet_test}.cc")
  target_link_libraries(diet_test_${diet_test} PRIVATE diet::diet)
  if(diet_test MATCHES "^sqlite_catalog")
    target_link_libraries(diet_test_${diet_test} PRIVATE diet::sqlite)
  endif()
  if(diet_test STREQUAL "registry_compat" AND DIET_ENABLE_SQLITE)
    target_link_libraries(diet_test_${diet_test} PRIVATE diet::sqlite)
    target_compile_definitions(diet_test_${diet_test} PRIVATE DIET_REGISTRY_COMPAT_SQLITE=1)
  endif()
  set_target_properties(diet_test_${diet_test} PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(diet_test_${diet_test} PRIVATE /W4 /WX /UNDEBUG /permissive-)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(diet_test_${diet_test} PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  endif()
  if(DIET_SANITIZERS)
    target_compile_options(diet_test_${diet_test} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(diet_test_${diet_test} PRIVATE -fsanitize=address,undefined)
  endif()
  if(DIET_USE_CCACHE)
    set_property(TARGET diet_test_${diet_test} PROPERTY CXX_COMPILER_LAUNCHER "${diet_ccache}")
  endif()
  add_test(NAME diet.${diet_test} COMMAND diet_test_${diet_test})
endforeach()

if(DIET_ENABLE_SQLITE)
  configure_file("${PROJECT_SOURCE_DIR}/cmake/diet-sqlite-smoke.cmake.in"
    "${PROJECT_BINARY_DIR}/diet-sqlite-smoke.cmake" @ONLY)
  add_test(NAME diet.package.sqlite
    COMMAND "${CMAKE_COMMAND}" "-Ddiet_smoke_config=$<CONFIG>"
      -P "${PROJECT_BINARY_DIR}/diet-sqlite-smoke.cmake")
  set_tests_properties(diet.package.sqlite PROPERTIES
    RESOURCE_LOCK diet_package_build TIMEOUT 180)
endif()

configure_file("${PROJECT_SOURCE_DIR}/cmake/diet-package-smoke.cmake.in"
  "${PROJECT_BINARY_DIR}/diet-package-smoke.cmake" @ONLY)
foreach(diet_mode IN ITEMS installed embedded)
  add_test(NAME diet.package.${diet_mode}
    COMMAND "${CMAKE_COMMAND}" "-Ddiet_smoke_mode=${diet_mode}"
      "-Ddiet_smoke_config=$<CONFIG>" -P "${PROJECT_BINARY_DIR}/diet-package-smoke.cmake")
  # Each smoke test can compile four jobs; avoid doubling that on parallel CTest.
  set_tests_properties(diet.package.${diet_mode} PROPERTIES
    RESOURCE_LOCK diet_package_build TIMEOUT 180)
endforeach()
