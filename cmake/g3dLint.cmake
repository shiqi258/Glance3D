# Source lints, registered as CTest tests with the `lint` label: `ctest -L lint`.
#
# They read the source tree only, so they need nothing built — just node, which the UI layout
# invariance tests use too (application/testing/tests.ui_layout.cmake):
#   g3d::LintUiUnits  scripts/check-ui-units.mjs — the UI units ratchet (G3DDp / G3DScale rules the
#                     compiler cannot see: bare lengths, audit exits, a second scale factor)
#   g3d::LintLocales  scripts/check-locales.mjs  — every translated source key has a zh-CN entry and
#                     every ICU message is well formed
find_program(F3D_NODE_EXECUTABLE node)
if(NOT F3D_NODE_EXECUTABLE)
  message(STATUS "node not found: the source lint tests are not added")
  return()
endif()

add_test(NAME g3d::LintUiUnits
  COMMAND ${F3D_NODE_EXECUTABLE} "${F3D_SOURCE_DIR}/scripts/check-ui-units.mjs")
add_test(NAME g3d::LintLocales
  COMMAND ${F3D_NODE_EXECUTABLE} "${F3D_SOURCE_DIR}/scripts/check-locales.mjs")
set_tests_properties(g3d::LintUiUnits g3d::LintLocales PROPERTIES LABELS "lint")
