##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
# \endlicense
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures Everett's optional Doxygen verification.

find_package(Doxygen 1.9.8 REQUIRED COMPONENTS doxygen)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

# Documentation tools remain optional development dependencies. The checker
# generates API/Markdown HTML/XML and checks associations, math, and links.
add_custom_target(everett_docs
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/check_doxygen.py"
    --doxygen "${DOXYGEN_EXECUTABLE}" --source "${PROJECT_SOURCE_DIR}"
    --output "${PROJECT_BINARY_DIR}/docs"
  COMMENT "Generate Everett API and Markdown documentation; check ownership, math, and links"
  VERBATIM)
if(EVERETT_BUILD_TESTS)
  add_test(NAME everett.doxygen
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/check_doxygen.py"
      --doxygen "${DOXYGEN_EXECUTABLE}" --source "${PROJECT_SOURCE_DIR}"
      --output "${PROJECT_BINARY_DIR}/docs-test")
  set_tests_properties(everett.doxygen PROPERTIES TIMEOUT 120)
endif()
