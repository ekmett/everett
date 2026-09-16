##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures Everett's test targets and package checks.

if(EVERETT_USE_CCACHE)
  find_program(everett_ccache NAMES ccache REQUIRED)
endif()

if(EVERETT_SANITIZERS)
  if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    message(FATAL_ERROR "EVERETT_SANITIZERS requires a compiler supporting both address and undefined sanitizers")
  endif()
  # Check the compiler and runtime link together without changing global flags.
  function(everett_check_sanitizers)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" everett_sanitizers_supported)
    if(NOT everett_sanitizers_supported)
      message(FATAL_ERROR "The requested address/undefined sanitizer combination cannot compile and link")
    endif()
  endfunction()
  everett_check_sanitizers()
endif()

set(everett_test_names registry registry_compat crc32c rank groups rank_groups_builder elias_fano profile borrowed_writer profile_blob comparison_fc sampling index_builder index_builder_allocations index_pipeline query cola_index cola_frontier cola_route_reuse cola_schedule native_writer native_writer_allocations native_file_writer profile_file_output native_merge native_merge_mapped world pins durability mapped_file files object_writer object_stream mapped_blob multiverse catalog_bindings output_budget)
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # These suites seal real mapped inputs through posix_object_ops throughout.
  # Model-ops writer tests above retain their platform-independent coverage.
  list(APPEND everett_test_names mapped_index_builder file_index_builder file_index_pipeline mapped_cola mapped_cola_builder cola_terminal cola_file_index cola_adaptive_index tombstone_codec)
endif()
list(APPEND everett_test_names cola_local_merge cola_local_merge_failure cola_runtime redundant_runtime redundant_initial runtime_registry sort_codec sort_profile sort_profile_query sort_profile_file_writer sort_runtime typed_world typed_initial typed_depth typed_preflight typed_query_value typed_redundant typed_scan tombstone_admission replacement_rebuild session)
if(EVERETT_ENABLE_SQLITE)
  list(APPEND everett_test_names redundant_checkpoint sqlite_catalog sqlite_catalog_adversarial sqlite_catalog_vfs sqlite_catalog_restart sqlite_catalog_timeline sqlite_catalog_streamed)
  if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND everett_test_names sqlite_catalog_cola sqlite_catalog_sessions sqlite_catalog_runtime sqlite_catalog_redundant sqlite_catalog_connection sqlite_catalog_scan sqlite_catalog_bindings sqlite_catalog_binding_failures sqlite_catalog_graphs sqlite_catalog_sort_runtime sqlite_catalog_snapshot sqlite_catalog_rebuild sqlite_catalog_rebuild_streaming sqlite_catalog_rebuild_default sqlite_catalog_tiny_rebuild sqlite_catalog_active sqlite_catalog_seals sqlite_catalog_seal_pair sqlite_catalog_native_pair sqlite_catalog_ready_reservations sqlite_catalog_initial sqlite_catalog_empty sqlite_catalog_pair sqlite_catalog_streaming_runtime sqlite_catalog_streaming_restart sqlite_catalog_streaming_diagnostics sqlite_catalog_adaptive_native sqlite_catalog_native_reuse sort_runtime_context sort_runtime_adaptive_index sort_runtime_initial)
  endif()
endif()
foreach(everett_test IN LISTS everett_test_names)
  add_executable(everett_test_${everett_test} "${PROJECT_SOURCE_DIR}/tests/${everett_test}.cc")
  target_link_libraries(everett_test_${everett_test} PRIVATE everett::everett)
  if(everett_test MATCHES "^sqlite_catalog" OR everett_test MATCHES "^sort_runtime_(context|adaptive_index|initial)$" OR everett_test STREQUAL "redundant_checkpoint")
    target_link_libraries(everett_test_${everett_test} PRIVATE everett::sqlite)
  endif()
  if(everett_test STREQUAL "registry_compat" AND EVERETT_ENABLE_SQLITE)
    target_link_libraries(everett_test_${everett_test} PRIVATE everett::sqlite)
    target_compile_definitions(everett_test_${everett_test} PRIVATE EVERETT_REGISTRY_COMPAT_SQLITE=1)
  endif()
  set_target_properties(everett_test_${everett_test} PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(everett_test_${everett_test} PRIVATE /W4 /WX /UNDEBUG /permissive-)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(everett_test_${everett_test} PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  endif()
  if(EVERETT_SANITIZERS)
    target_compile_options(everett_test_${everett_test} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(everett_test_${everett_test} PRIVATE -fsanitize=address,undefined)
  endif()
  if(EVERETT_USE_CCACHE)
    set_property(TARGET everett_test_${everett_test} PROPERTY CXX_COMPILER_LAUNCHER "${everett_ccache}")
  endif()
  add_test(NAME everett.${everett_test} COMMAND everett_test_${everett_test})
endforeach()
set_tests_properties(everett.catalog_bindings PROPERTIES TIMEOUT 45)
if(TARGET everett_test_sqlite_catalog_ready_reservations)
  set_tests_properties(everett.sqlite_catalog_ready_reservations PROPERTIES TIMEOUT 180)
endif()
if(TARGET everett_test_sqlite_catalog_streaming_restart)
  set_tests_properties(everett.sqlite_catalog_streaming_restart PROPERTIES TIMEOUT 240)
endif()

if(EVERETT_ENABLE_SQLITE)
  configure_file("${PROJECT_SOURCE_DIR}/cmake/everett-sqlite-smoke.cmake.in"
    "${PROJECT_BINARY_DIR}/everett-sqlite-smoke.cmake" @ONLY)
  add_test(NAME everett.package.sqlite
    COMMAND "${CMAKE_COMMAND}" "-Deverett_smoke_config=$<CONFIG>"
      -P "${PROJECT_BINARY_DIR}/everett-sqlite-smoke.cmake")
  set_tests_properties(everett.package.sqlite PROPERTIES
    RESOURCE_LOCK everett_package_build TIMEOUT 180)
endif()

configure_file("${PROJECT_SOURCE_DIR}/cmake/everett-package-smoke.cmake.in"
  "${PROJECT_BINARY_DIR}/everett-package-smoke.cmake" @ONLY)
foreach(everett_mode IN ITEMS installed embedded)
  add_test(NAME everett.package.${everett_mode}
    COMMAND "${CMAKE_COMMAND}" "-Deverett_smoke_mode=${everett_mode}"
      "-Deverett_smoke_config=$<CONFIG>" -P "${PROJECT_BINARY_DIR}/everett-package-smoke.cmake")
  # Each smoke test can compile four jobs; avoid doubling that on parallel CTest.
  set_tests_properties(everett.package.${everett_mode} PROPERTIES
    RESOURCE_LOCK everett_package_build TIMEOUT 180)
endforeach()
