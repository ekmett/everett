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

set(everett_test_names rank groups front world pins durability mapped_file files)
# Include profile and facade fixtures when present in the checkout.
foreach(everett_optional_test IN ITEMS profile profile_blob multiverse)
  if(EXISTS "${PROJECT_SOURCE_DIR}/tests/${everett_optional_test}.cc")
    list(APPEND everett_test_names "${everett_optional_test}")
  endif()
endforeach()
foreach(everett_test IN LISTS everett_test_names)
  add_executable(everett_test_${everett_test} "${PROJECT_SOURCE_DIR}/tests/${everett_test}.cc")
  target_link_libraries(everett_test_${everett_test} PRIVATE everett::everett)
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

##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
# SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
# \endlicense
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures Everett's test targets and package checks.
