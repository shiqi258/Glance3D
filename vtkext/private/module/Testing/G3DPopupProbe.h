/**
 * @file G3DPopupProbe.h
 * @brief Drive and read back the floating surfaces of a G3DWidgetHarness frame, for the tests of
 * the widgets built on BeginPopover (TestG3DPopover, TestG3DSelect).
 *
 * A click reaches ImGui the way a real one does (move, press, release on successive frames), and
 * the popup window is read back as the frame left it — whether it was drawn (ImGui hides a popup
 * on the frame it opens), where, and at what size. Reading windows back needs imgui_internal.h,
 * which only tests include.
 */

#ifndef G3DPopupProbe_h
#define G3DPopupProbe_h

#include "G3DWidgetHarness.h"

#include <imgui.h>
#include <imgui_internal.h> // only to read windows back (Hidden / Pos / Size / ScrollMax / flags)

#include <functional>
#include <vector>

namespace G3DPopupProbe
{
/// The popup window as a frame left it.
struct Frame
{
  bool open = false;    ///< a popup window is active
  bool visible = false; ///< ...and was drawn
  ImVec2 pos = ImVec2(0.f, 0.f);
  ImVec2 size = ImVec2(0.f, 0.f);
  float scrollMaxY = 0.f;
  float Bottom() const { return this->pos.y + this->size.y; }
};

inline Frame ReadPopup()
{
  Frame f;
  for (ImGuiWindow* w : GImGui->Windows)
  {
    if ((w->Flags & ImGuiWindowFlags_Popup) != 0 && w->Active)
    {
      f.open = true;
      f.visible = !w->Hidden;
      f.pos = w->Pos;
      f.size = w->Size;
      f.scrollMaxY = w->ScrollMax.y;
    }
  }
  return f;
}

/// Whether a tooltip window is up.
inline bool TooltipShown()
{
  for (ImGuiWindow* w : GImGui->Windows)
  {
    if ((w->Flags & ImGuiWindowFlags_Tooltip) != 0 && w->Active && !w->Hidden)
    {
      return true;
    }
  }
  return false;
}

/// What one frame submits.
using Scene = std::function<void()>;

inline Frame Step(G3DWidgetHarness& harness, const Scene& scene)
{
  harness.Begin();
  scene();
  harness.End();
  return ReadPopup();
}

/// A click at @p at the way ImGui receives one (move, press, release on successive frames), then
/// @p settle more frames. Returns the frames from the release on: the release frame opens a popup.
inline std::vector<Frame> ClickAndWatch(
  G3DWidgetHarness& harness, const Scene& scene, const ImVec2& at, int settle = 6)
{
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(at.x, at.y);
  Step(harness, scene);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
  Step(harness, scene);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  std::vector<Frame> frames;
  frames.push_back(Step(harness, scene));
  for (int i = 0; i < settle; ++i)
  {
    frames.push_back(Step(harness, scene));
  }
  return frames;
}
}

#endif
