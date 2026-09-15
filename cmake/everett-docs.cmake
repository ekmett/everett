find_package(Doxygen 1.9.8 REQUIRED COMPONENTS doxygen)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

# Documentation tools remain optional development dependencies. The checker
# generates both the reference HTML/XML and isolated association fixtures.
add_custom_target(everett_docs
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/check_doxygen.py"
    --doxygen "${DOXYGEN_EXECUTABLE}" --source "${PROJECT_SOURCE_DIR}"
    --output "${PROJECT_BINARY_DIR}/docs"
  COMMENT "Generate Everett documentation and check XML ownership"
  VERBATIM)
if(EVERETT_BUILD_TESTS)
  add_test(NAME everett.doxygen
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/check_doxygen.py"
      --doxygen "${DOXYGEN_EXECUTABLE}" --source "${PROJECT_SOURCE_DIR}"
      --output "${PROJECT_BINARY_DIR}/docs-test")
  set_tests_properties(everett.doxygen PROPERTIES TIMEOUT 120)
endif()

##
# \file
# \license
# SPDX-FileType: SOURCE
# SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>. All rights reserved.
# SPDX-License-Identifier: LicenseRef-Everett-All-Rights-Reserved
# \endlicense
# \author Edward Kmett <ekmett@gmail.com>
# \brief Configures Everett's optional Doxygen verification.
