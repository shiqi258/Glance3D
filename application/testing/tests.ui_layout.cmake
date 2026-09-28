## UI layout invariance under DPI scaling
#
# Each scene is rendered at DPI 1.0, 1.5 and 2.0 with --dpi-aware, so the window grows with the
# scale and the logical layout must not change. G3DLayoutProbe dumps where every item landed
# (G3D_LAYOUT_DUMP) and scripts/compare-ui-layout.mjs checks that every item's size and spacing
# scaled by the same factor. A length read in the wrong unit -- scaled twice, or never -- breaks
# exactly that, whichever code drew it. Findings already known are listed, with the reason, in
# testing/ui-layout-allow.json; anything else fails.
#
# The probe rides Dear ImGui's test-engine hooks, which only the bundled imgui is built with, and
# the comparer needs node.
if(F3D_MODULE_UI AND NOT F3D_USE_EXTERNAL_IMGUI AND F3D_TESTING_ENABLE_RENDERING_TESTS)
  find_program(F3D_NODE_EXECUTABLE node)
  if(NOT F3D_NODE_EXECUTABLE)
    message(STATUS "node not found: the UI layout invariance tests are not added")
  else()
    set(_g3d_layout_dir "${CMAKE_BINARY_DIR}/Testing/Temporary")

    # g3d_layout_scene(<scene> DATA <files...> ARGS <args...>)
    function(g3d_layout_scene scene)
      cmake_parse_arguments(_scene "" "" "DATA;ARGS" ${ARGN})
      foreach(dpi IN ITEMS 1.0 1.5 2.0)
        string(REPLACE "." "" _tag "${dpi}0")
        set(_dump "${_g3d_layout_dir}/G3DLayout_${scene}_${_tag}.json")
        f3d_test(NAME TestG3DLayoutDump${scene}${_tag} DATA ${_scene_DATA}
          ARGS --dpi-aware ${_scene_ARGS} RESOLUTION 1100,700 DPI_SCALE ${dpi} UI NO_BASELINE
          ENV "G3D_LAYOUT_DUMP=${_dump}" LABELS ui-layout)
        set_tests_properties(f3d::TestG3DLayoutDump${scene}${_tag} PROPERTIES
          FIXTURES_SETUP g3d::layout::${scene}${_tag})
        if(NOT dpi STREQUAL "1.0")
          add_test(NAME f3d::TestG3DLayoutInvariance${scene}${_tag}
            COMMAND ${F3D_NODE_EXECUTABLE} "${F3D_SOURCE_DIR}/scripts/compare-ui-layout.mjs"
              "${_g3d_layout_dir}/G3DLayout_${scene}_100.json" "${_dump}"
              --allow "${F3D_SOURCE_DIR}/testing/ui-layout-allow.json" --scene ${scene})
          set_tests_properties(f3d::TestG3DLayoutInvariance${scene}${_tag} PROPERTIES
            FIXTURES_REQUIRED "g3d::layout::${scene}100;g3d::layout::${scene}${_tag}"
            LABELS "application;ui-layout")
        endif()
      endforeach()
    endfunction()

    # The workbench over an animated rig with more than 20 nodes: scene tree toolbar, timeline,
    # inspector, plus both floating cards.
    g3d_layout_scene(Rig DATA RiggedFigure.glb
      ARGS -Dui.control_panel=true -Dui.cheatsheet=true -Dui.notification_center=true)
    # Scalar coloring: the inspector's Coloring section, whose Range row carries an icon button.
    g3d_layout_scene(Coloring DATA bluntfin.vts ARGS -s -Dui.control_panel=true)
  endif()
endif()
