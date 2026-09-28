/**
 * @file G3DLayoutProbe.h
 * @brief Records where every ImGui item and window landed in a frame, for layout regression tests.
 *
 * Armed only when the environment variable `G3D_LAYOUT_DUMP` names a file. While armed, every item
 * ImGui registers in a frame is collected through Dear ImGui's own test-engine hooks (the bundled
 * imgui is compiled with IMGUI_ENABLE_TEST_ENGINE for this), and once the frame is rendered the
 * file is rewritten with them as JSON — so it always holds the last frame, the one a `--output`
 * render writes out.
 *
 * The layout tests render the same scene at UI scale 1 and at S, then check that every item scaled
 * by S (scripts/compare-ui-layout.mjs). A length read in the wrong unit — scaled twice, or not at
 * all — breaks exactly that relation, wherever it is written, so the check needs no knowledge of
 * the code that drew the item. Disarmed, the hooks cost one branch per item.
 */

#ifndef G3DLayoutProbe_h
#define G3DLayoutProbe_h

#include "G3DUnits.h"

namespace G3DLayoutProbe
{
/// True when `G3D_LAYOUT_DUMP` is set (read once per process).
bool Armed();

/// Call right after ImGui::NewFrame(): starts recording this frame's items.
void BeginFrame();

/// Call after ImGui::Render(): writes the recorded frame. @p uiScale is the scale the host built
/// the UI at, stored alongside the items for the report.
void EndFrame(G3DScale uiScale);
}

#endif
