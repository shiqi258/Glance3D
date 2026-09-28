// A popover is placed from its measured size. ImGui hides a popup on the frame it opens and lays its
// content out at a reset size, so a placement decided from the window size remembered on an earlier
// frame put the first frame anyone saw below the trigger, and flipped it above one frame later.
// Checked frame by frame in a headless context built like the app's, at four UI scales:
//  - near the bottom a popover opens above its trigger, near the top below it, and the first frame
//    it is drawn at is where it stays;
//  - the side decision is exact: one px more room than it needs below opens it below, one px less
//    opens it above — the size measured on the hidden frame is the size drawn;
//  - while open it keeps its side and the edge facing the trigger, whatever its content does;
//  - short of room on both sides it takes the bigger side, shrunk, scrolling, clear of the trigger;
//  - every open decides afresh;
//  - the widgets built on it (the color picker, the dropdown) behave the same.

#include "G3DLayout.h"
#include "G3DTheme.h"
#include "G3DWidgetHarness.h"
#include "G3DWidgets.h"

#include <imgui.h>
#include <imgui_internal.h> // only to read the popup window back (Hidden / Pos / Size / ScrollMax)

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Expect(bool ok, const std::string& label)
{
  if (!ok)
  {
    std::cerr << "FAIL [" << label << "]\n";
    ++failures;
  }
}

void ExpectNear(float got, float want, float tolerance, const std::string& label)
{
  if (std::abs(got - want) > tolerance)
  {
    std::cerr << "FAIL [" << label << "]: got " << got << " want " << want << "\n";
    ++failures;
  }
}

// What the widgets traced: the popover logs its side once per open, and a MISMATCH when the first
// frame drawn is not the size it was placed with.
std::vector<std::string> gTraces;
void CollectTrace(const char* line)
{
  gTraces.emplace_back(line);
}

int CountTraces(const char* needle)
{
  int n = 0;
  for (const std::string& t : gTraces)
  {
    n += t.find(needle) != std::string::npos ? 1 : 0;
  }
  return n;
}

// The popup window as a frame left it.
struct Frame
{
  bool open = false;    ///< a popup window is active
  bool visible = false; ///< ...and was drawn
  ImVec2 pos = ImVec2(0.f, 0.f);
  ImVec2 size = ImVec2(0.f, 0.f);
  float scrollMaxY = 0.f;
  float Bottom() const { return this->pos.y + this->size.y; }
};

Frame ReadPopup()
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

using Scene = std::function<void()>;

Frame Step(G3DWidgetHarness& harness, const Scene& scene)
{
  harness.Begin();
  scene();
  harness.End();
  return ReadPopup();
}

// A click at @p at the way ImGui receives one (move, press, release on successive frames), then
// @p settle more frames. Returns the frames from the release on: the release frame opens the popup.
std::vector<Frame> ClickAndWatch(
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

// ImGui's hidden measuring frame first, then the popover drawn at one place and one size from its
// very first frame on. Returns that frame.
Frame ExpectStableFromFirstDraw(const std::vector<Frame>& frames, const std::string& label)
{
  Expect(!frames.empty() && frames[0].open && !frames[0].visible, label + ".hidden.measuring.frame");
  int first = -1;
  for (int i = 0; i < static_cast<int>(frames.size()); ++i)
  {
    if (frames[i].visible)
    {
      first = i;
      break;
    }
  }
  Expect(first == 1, label + ".drawn.next.frame");
  if (first < 0)
  {
    return Frame{};
  }
  const Frame& a = frames[first];
  for (int i = first + 1; i < static_cast<int>(frames.size()); ++i)
  {
    const Frame& b = frames[i];
    const bool same = b.visible && std::abs(b.pos.x - a.pos.x) < 0.01f &&
      std::abs(b.pos.y - a.pos.y) < 0.01f && std::abs(b.size.x - a.size.x) < 0.01f &&
      std::abs(b.size.y - a.size.y) < 0.01f;
    if (!same)
    {
      std::cerr << "FAIL [" << label << ".stable]: first drawn at (" << a.pos.x << ", " << a.pos.y
                << ") " << a.size.x << "x" << a.size.y << ", frame " << i << " at (" << b.pos.x
                << ", " << b.pos.y << ") " << b.size.x << "x" << b.size.y << "\n";
      ++failures;
      break;
    }
  }
  return a;
}

// A bare popover: an invisible trigger at `triggerPos` and a fixed-size block of content.
struct Generic
{
  ImVec2 triggerPos = ImVec2(40.f, 40.f);
  ImVec2 triggerSize = ImVec2(120.f, 24.f);
  float contentW = 200.f;
  float contentH = 300.f;
  G3DDp minHeight;
  bool closeNow = false;
  G3DLayout::Rect trigger; ///< as drawn
};

Scene GenericScene(Generic& g)
{
  return [&g]()
  {
    const G3DScale s = G3DWidgets::UiScale();
    const float pad = G3DTheme::Spacing::Md * s;
    ImGui::SetCursorScreenPos(g.triggerPos);
    if (ImGui::InvisibleButton("##trigger", g.triggerSize))
    {
      ImGui::OpenPopup("##pop");
    }
    const ImVec2 t0 = ImGui::GetItemRectMin();
    const ImVec2 t1 = ImGui::GetItemRectMax();
    g.trigger = { t0.x, t0.y, t1.x - t0.x, t1.y - t0.y };
    G3DWidgets::PopoverDesc pd;
    pd.anchor = g.trigger;
    pd.width = g.contentW + 2.f * pad;
    pd.minHeight = g.minHeight;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    if (G3DWidgets::BeginPopover("##pop", pd))
    {
      ImGui::Dummy(ImVec2(g.contentW, g.contentH));
      if (g.closeNow)
      {
        ImGui::CloseCurrentPopup();
      }
      G3DWidgets::EndPopover();
    }
    ImGui::PopStyleVar();
  };
}

ImVec2 Center(const G3DLayout::Rect& r)
{
  return ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

// ImGui puts a window on whole pixels (it truncates the position, and a constrained size), so an
// edge pinned `offset` from the trigger lands on the pixel that truncation gives.
float EdgeAbove(const G3DLayout::Rect& trigger, float offset)
{
  return std::floor(trigger.y - offset);
}
float EdgeBelow(const G3DLayout::Rect& trigger, float offset)
{
  return std::floor(trigger.y + trigger.h + offset);
}

void CheckGeneric(double uiScale)
{
  const std::string at = " @" + std::to_string(uiScale);
  const ImVec2 display(800.f, 600.f);
  G3DWidgetHarness harness(uiScale, display);
  const G3DScale s = G3DWidgets::UiScale();
  const float pad = G3DTheme::Spacing::Md * s;
  const float offset = 6_dp * s;
  const float margin = G3DTheme::Spacing::Sm * s;
  Generic g;
  const Scene scene = GenericScene(g);
  Step(harness, scene); // builds the font atlas
  // The window height: the content extent ImGui truncates, the padding, the constrained size it
  // truncates again.
  const auto windowH = [&](float contentH) { return std::floor(std::trunc(contentH) + 2.f * pad); };

  // Near the bottom: above the trigger, its bottom edge `offset` over it, from the first frame.
  {
    g.triggerPos = ImVec2(40.f, display.y - 80.f);
    Step(harness, scene);
    const Frame f = ExpectStableFromFirstDraw(
      ClickAndWatch(harness, scene, Center(g.trigger)), "bottom.opens.above" + at);
    ExpectNear(f.Bottom(), EdgeAbove(g.trigger, offset), 0.01f, "bottom.edge.on.trigger" + at);
    ExpectNear(f.size.y, windowH(g.contentH), 0.01f, "bottom.natural.height" + at);

    // While open it keeps its side and the edge facing the trigger: content that grows moves the
    // top edge only; content short enough to fit below now stays above all the same.
    g.contentH += 40.f;
    Frame grown;
    for (int i = 0; i < 3; ++i)
    {
      grown = Step(harness, scene);
    }
    ExpectNear(grown.Bottom(), f.Bottom(), 0.5f, "grow.keeps.edge" + at);
    ExpectNear(grown.size.y, f.size.y + 40.f, 0.5f, "grow.grows" + at);
    g.contentH = 20.f;
    Frame shrunk;
    for (int i = 0; i < 3; ++i)
    {
      shrunk = Step(harness, scene);
    }
    ExpectNear(shrunk.Bottom(), f.Bottom(), 0.5f, "shrink.keeps.side.and.edge" + at);
    g.contentH = 300.f;

    // Every open decides afresh: closed, moved to the top, reopened — below it now.
    g.closeNow = true;
    Step(harness, scene);
    g.closeNow = false;
    Expect(!Step(harness, scene).open, "closed" + at);
    g.triggerPos = ImVec2(40.f, 40.f);
    Step(harness, scene);
    const Frame r = ExpectStableFromFirstDraw(
      ClickAndWatch(harness, scene, Center(g.trigger)), "reopen.top.opens.below" + at);
    ExpectNear(r.pos.y, EdgeBelow(g.trigger, offset), 0.01f, "reopen.edge.on.trigger" + at);
    g.closeNow = true;
    Step(harness, scene);
    g.closeNow = false;
    Step(harness, scene);
  }

  // The side decision is exact at the threshold: the room below is the window height plus or
  // minus one px (with room enough above for it whole).
  g.contentH = 200.f;
  for (const float extra : { 1.f, -1.f })
  {
    const float triggerBottom = display.y - margin - offset - (windowH(g.contentH) + extra);
    g.triggerPos = ImVec2(40.f, triggerBottom - g.triggerSize.y);
    Step(harness, scene);
    const std::string label = extra > 0.f ? "threshold.fits.below" : "threshold.flips.above";
    const Frame f =
      ExpectStableFromFirstDraw(ClickAndWatch(harness, scene, Center(g.trigger)), label + at);
    if (extra > 0.f)
    {
      ExpectNear(f.pos.y, EdgeBelow(g.trigger, offset), 0.01f, label + ".edge" + at);
    }
    else
    {
      ExpectNear(f.Bottom(), EdgeAbove(g.trigger, offset), 0.01f, label + ".edge" + at);
    }
    ExpectNear(f.size.y, windowH(g.contentH), 0.01f, label + ".whole" + at);
    g.closeNow = true;
    Step(harness, scene);
    g.closeNow = false;
    Step(harness, scene);
  }
}

// Short of room on both sides: the bigger side, shrunk to it, the content scrolling, the trigger
// uncovered.
void CheckShortWindow(double uiScale)
{
  const std::string at = " @" + std::to_string(uiScale);
  const ImVec2 display(800.f, 360.f);
  G3DWidgetHarness harness(uiScale, display);
  const G3DScale s = G3DWidgets::UiScale();
  const float offset = 6_dp * s;
  const float margin = G3DTheme::Spacing::Sm * s;
  Generic g;
  g.contentH = 400.f;
  g.minHeight = 60_dp;
  const Scene scene = GenericScene(g);
  Step(harness, scene);

  for (const float triggerY : { 150.f, 200.f })
  {
    g.triggerPos = ImVec2(40.f, triggerY);
    Step(harness, scene);
    const float roomBelow = display.y - margin - (triggerY + g.triggerSize.y + offset);
    const float roomAbove = triggerY - offset - margin;
    const bool below = roomBelow >= roomAbove;
    const std::string label = std::string("short.") + (below ? "below" : "above");
    const Frame f =
      ExpectStableFromFirstDraw(ClickAndWatch(harness, scene, Center(g.trigger)), label + at);
    // All the room, less at most the pixel ImGui truncates a constrained height by.
    const float room = below ? roomBelow : roomAbove;
    Expect(f.size.y <= room + 0.01f && f.size.y > room - 1.01f, label + ".fills.room" + at);
    Expect(f.scrollMaxY > 0.f, label + ".scrolls" + at);
    const bool clear = below ? f.pos.y >= g.trigger.y + g.trigger.h : f.Bottom() <= g.trigger.y;
    Expect(clear, label + ".trigger.uncovered" + at);
    g.closeNow = true;
    Step(harness, scene);
    g.closeNow = false;
    Step(harness, scene);
  }
}

// The widgets built on it: the color picker's panel and the dropdown's menu, near the bottom and
// near the top of a window tall enough to take the picker whole at 2x.
void CheckWidgets(double uiScale)
{
  const std::string at = " @" + std::to_string(uiScale);
  const ImVec2 display(1000.f, 1200.f);
  G3DWidgetHarness harness(uiScale, display);
  const G3DScale s = G3DWidgets::UiScale();

  float col[4] = { 0.2f, 0.4f, 0.8f, 1.f };
  ImVec2 pickerPos(40.f, 40.f);
  G3DLayout::Rect swatch;
  const Scene picker = [&]()
  {
    ImGui::SetCursorScreenPos(pickerPos);
    ImGui::SetNextItemWidth(200.f);
    G3DWidgets::ColorEdit("##pick", col);
    const ImVec2 t0 = ImGui::GetItemRectMin();
    const ImVec2 t1 = ImGui::GetItemRectMax();
    swatch = { t0.x, t0.y, t1.x - t0.x, t1.y - t0.y };
  };
  Step(harness, picker);
  for (const bool bottom : { true, false })
  {
    pickerPos = ImVec2(40.f, bottom ? display.y - 80.f : 40.f);
    Step(harness, picker);
    const std::string label = bottom ? "picker.bottom.opens.above" : "picker.top.opens.below";
    const Frame f =
      ExpectStableFromFirstDraw(ClickAndWatch(harness, picker, Center(swatch)), label + at);
    const float gap = G3DTheme::Spacing::Xs * s;
    if (bottom)
    {
      ExpectNear(f.Bottom(), EdgeAbove(swatch, gap), 0.01f, label + ".edge" + at);
    }
    else
    {
      ExpectNear(f.pos.y, EdgeBelow(swatch, gap), 0.01f, label + ".edge" + at);
    }
    // Close it with Esc-free means: a click away from it.
    ClickAndWatch(harness, picker, ImVec2(display.x - 10.f, 10.f), 1);
    Expect(!Step(harness, picker).open, label + ".closed" + at);
  }

  ImVec2 selectPos(40.f, display.y - 80.f);
  G3DLayout::Rect trigger;
  int current = 0;
  const Scene select = [&]()
  {
    ImGui::SetCursorScreenPos(selectPos);
    ImGui::SetNextItemWidth(160.f);
    if (G3DWidgets::BeginSelect("##sel", "Option"))
    {
      for (int i = 0; i < 6; ++i)
      {
        const std::string item = "Option " + std::to_string(i);
        if (G3DWidgets::SelectItem(item.c_str(), i == current))
        {
          current = i;
        }
      }
      G3DWidgets::EndSelect();
    }
    const ImVec2 t0 = ImGui::GetItemRectMin();
    const ImVec2 t1 = ImGui::GetItemRectMax();
    trigger = { t0.x, t0.y, t1.x - t0.x, t1.y - t0.y };
  };
  Step(harness, select);
  Step(harness, select);
  const Frame f = ExpectStableFromFirstDraw(
    ClickAndWatch(harness, select, Center(trigger)), "select.bottom.opens.above" + at);
  ExpectNear(f.Bottom(), EdgeAbove(trigger, 6_dp * s), 0.01f, "select.bottom.edge" + at);
}
}

int TestG3DPopover(int, char*[])
{
  G3DWidgets::SetTraceSink(CollectTrace);
  // 18/14 is 125% as the UI actually renders it: ImGui rounds the 17.5px font to 18.
  for (const double uiScale : { 1.0, 18.0 / 14.0, 1.5, 2.0 })
  {
    CheckGeneric(uiScale);
    CheckShortWindow(uiScale);
    CheckWidgets(uiScale);
  }
  G3DWidgets::SetTraceSink(nullptr);

  // One side decision per open (4 generic + 2 short + 3 widget opens per scale), and not one
  // first frame drawn at a size other than the one it was placed with.
  Expect(CountTraces("[pop.place]") > 0, "traced");
  Expect(CountTraces("side=") == 4 * 9, "one.decision.per.open");
  Expect(CountTraces("MISMATCH") == 0, "no.mismatch");
  for (const std::string& t : gTraces)
  {
    if (t.find("MISMATCH") != std::string::npos)
    {
      std::cerr << t << "\n";
    }
  }

  if (failures > 0)
  {
    std::cerr << failures << " popover check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
