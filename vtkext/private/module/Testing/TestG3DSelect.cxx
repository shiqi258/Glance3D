// A dropdown shows its values whole. Checked in a headless context built like the app's, at four UI
// scales, by the glyph vertices each label leaves in the draw lists (a label cut to "0..." leaves
// fewer than the label drawn whole):
//  - a trigger reserved with SelectSize draws every one of its values uncut;
//  - the menu is at least the trigger's width and fits its widest item with the check column kept
//    on every row, whichever item is selected: the selected one used to be the one cut to a bare
//    "..." (the timeline's speed menu showed "0.25x" until it was picked);
//  - the width the menu is first drawn at is the one it keeps, opening above a bottom bar and in a
//    list long enough to scroll included, and a menu that fits shows no scrollbar;
//  - past the menu's width cap an item is cut and shows whole in a tooltip, an uncut one does not;
//  - the trigger's tooltip names it, and a value the trigger had to cut shows whole there;
//  - the colormap trigger's swatch shrinks before the colormap's name is cut.

#include "G3DLayout.h"
#include "G3DPopupProbe.h"
#include "G3DTheme.h"
#include "G3DWidgetHarness.h"
#include "G3DWidgets.h"

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace
{
using namespace G3DPopupProbe;

int failures = 0;

void Expect(bool ok, const std::string& label, float got = 0.f, float want = 0.f)
{
  if (!ok)
  {
    std::cerr << "FAIL [" << label << "]: " << got << " vs " << want << "\n";
    ++failures;
  }
}

// Vertices of [first, end) painted exactly @p col, left of @p maxX.
int CountVertices(const ImDrawList* dl, int first, ImU32 col, float maxX = FLT_MAX)
{
  int n = 0;
  for (int i = first; i < dl->VtxBuffer.Size; ++i)
  {
    const ImDrawVert& v = dl->VtxBuffer[i];
    n += (v.col == col && v.pos.x < maxX) ? 1 : 0;
  }
  return n;
}

// The vertices a label leaves drawn whole in the current font, drawn in the harness window in a
// color nothing else uses: the count a label drawn uncut must match.
int WholeVertices(const char* label)
{
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int first = dl->VtxBuffer.Size;
  dl->AddText(ImVec2(8.f, 8.f), IM_COL32(1, 2, 3, 255), label);
  return dl->VtxBuffer.Size - first;
}

// One dropdown, its trigger reserved with SelectSize over its labels unless `width` is set, and
// what the last frame drew.
struct Dropdown
{
  std::vector<std::string> labels;
  int current = 0;
  ImVec2 pos = ImVec2(40.f, 40.f);
  float width = 0.f; ///< trigger width px; 0: SelectSize(labels)
  const char* tooltip = nullptr;

  G3DLayout::Rect trigger;       ///< as drawn
  bool triggerWhole = false;     ///< the current value drawn uncut in the trigger
  std::vector<bool> rowWhole;    ///< per item, drawn uncut (empty: the menu was not submitted)
  std::vector<G3DLayout::Rect> rows;
  std::vector<std::string> rowWhy; ///< per item: what it drew, for a failure message
};

Scene DropdownScene(Dropdown& d)
{
  return [&d]()
  {
    const G3DScale s = G3DWidgets::UiScale();
    std::vector<const char*> ptrs;
    std::vector<int> whole;
    for (const std::string& label : d.labels)
    {
      ptrs.push_back(label.c_str());
      whole.push_back(WholeVertices(label.c_str()));
    }
    const float w = d.width > 0.f
      ? d.width
      : G3DWidgets::SelectSize(std::span<const char* const>(ptrs.data(), ptrs.size())).x;

    ImGui::SetCursorScreenPos(d.pos);
    ImGui::SetNextItemWidth(w);
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    const int triggerFirst = tdl->VtxBuffer.Size;
    const bool open = G3DWidgets::BeginSelect("##sel", ptrs[d.current], nullptr, d.tooltip);
    d.triggerWhole =
      CountVertices(tdl, triggerFirst, G3DTheme::U32(G3DTheme::Text())) == whole[d.current];
    d.rowWhole.clear();
    d.rows.clear();
    d.rowWhy.clear();
    if (open)
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      // A row's label ends before its check column: the row's padding, the check, half the gap.
      const float checkCol = (10_dp + 14_dp + 4_dp) * s;
      for (std::size_t i = 0; i < d.labels.size(); ++i)
      {
        const int first = dl->VtxBuffer.Size;
        const bool selected = static_cast<int>(i) == d.current;
        G3DWidgets::SelectItem(ptrs[i], selected);
        const ImVec2 r0 = ImGui::GetItemRectMin();
        const ImVec2 r1 = ImGui::GetItemRectMax();
        d.rows.push_back({ r0.x, r0.y, r1.x - r0.x, r1.y - r0.y });
        const ImU32 col = G3DTheme::U32(selected ? G3DTheme::Accent() : G3DTheme::Text());
        const int drawn = CountVertices(dl, first, col, r1.x - checkCol);
        d.rowWhole.push_back(drawn == whole[i]);
        d.rowWhy.push_back(std::to_string(drawn) + "/" + std::to_string(whole[i]) +
          " vertices, row " + std::to_string(r1.x - r0.x) + " px for a " +
          std::to_string(ImGui::CalcTextSize(ptrs[i]).x) + " px label");
      }
      G3DWidgets::EndSelect();
    }
    const ImVec2 t0 = ImGui::GetItemRectMin();
    const ImVec2 t1 = ImGui::GetItemRectMax();
    d.trigger = { t0.x, t0.y, t1.x - t0.x, t1.y - t0.y };
  };
}

ImVec2 Center(const G3DLayout::Rect& r)
{
  return ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}

// ImGui's hidden measuring frame first, then the menu drawn at one place and one size from its very
// first frame on. Returns that frame.
Frame FirstDrawIsFinal(const std::vector<Frame>& frames, const std::string& label)
{
  Expect(
    !frames.empty() && frames[0].open && !frames[0].visible, label + ".hidden.measuring.frame");
  Expect(frames.size() > 1 && frames[1].visible, label + ".drawn.next.frame");
  if (frames.size() < 2 || !frames[1].visible)
  {
    return Frame{};
  }
  const Frame& a = frames[1];
  for (std::size_t i = 2; i < frames.size(); ++i)
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

// Close an open menu with a click away from it.
void CloseMenu(G3DWidgetHarness& harness, const Scene& scene, const std::string& label)
{
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ClickAndWatch(harness, scene, ImVec2(display.x - 10.f, display.y * 0.5f), 1);
  Expect(!Step(harness, scene).open, label + ".closed");
}

// Hover @p at long enough for the house tooltip delay.
void Hover(G3DWidgetHarness& harness, const Scene& scene, const ImVec2& at)
{
  ImGui::GetIO().AddMousePosEvent(at.x, at.y);
  for (int i = 0; i < 45; ++i)
  {
    Step(harness, scene);
  }
}

const std::vector<std::string> kSpeeds = { "0.1\xc3\x97", "0.25\xc3\x97", "0.5\xc3\x97",
  "1\xc3\x97", "2\xc3\x97", "4\xc3\x97" };

void CheckScale(double uiScale)
{
  const std::string at = " @" + std::to_string(uiScale);
  G3DWidgetHarness harness(uiScale);
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const G3DScale s = G3DWidgets::UiScale();

  // The playback-speed set, near the bottom like the timeline's: the menu opens above it.
  Dropdown d;
  d.labels = kSpeeds;
  d.pos = ImVec2(40.f, display.y - 80.f);
  const Scene scene = DropdownScene(d);
  Step(harness, scene); // builds the font atlas
  for (int k = 0; k < static_cast<int>(kSpeeds.size()); ++k)
  {
    const std::string label = "speed." + std::to_string(k) + at;
    d.current = k;
    Step(harness, scene);
    Expect(d.triggerWhole, label + ".trigger.whole");

    const Frame f = FirstDrawIsFinal(ClickAndWatch(harness, scene, Center(d.trigger)), label);
    Expect(f.Bottom() <= d.trigger.y, label + ".opens.above", f.Bottom(), d.trigger.y);
    // Six items fit: no scrollbar (a height a fraction short of the content showed one at 18/14,
    // and it took its width from the rows).
    Expect(f.scrollMaxY == 0.f, label + ".fits.no.scrollbar", f.scrollMaxY, 0.f);
    Expect(f.size.x >= d.trigger.w, label + ".at.least.trigger", f.size.x, d.trigger.w);
    Expect(d.rowWhole.size() == kSpeeds.size(), label + ".rows");
    for (std::size_t i = 0; i < d.rowWhole.size(); ++i)
    {
      Expect(d.rowWhole[i], label + ".row." + std::to_string(i) + ".whole (" + d.rowWhy[i] + ")");
    }
    CloseMenu(harness, scene, label);
  }

  // A list long enough to scroll, its widest item first (in view), near the top and near the
  // bottom: the scrollbar's gutter is part of the first width drawn, not added a frame later.
  Dropdown l;
  l.labels.push_back("A considerably wider first option");
  for (int i = 1; i < 30; ++i)
  {
    l.labels.push_back("Option " + std::to_string(i));
  }
  l.width = 120_dp * s;
  const Scene longScene = DropdownScene(l);
  for (const bool bottom : { false, true })
  {
    const std::string label = std::string(bottom ? "long.bottom" : "long.top") + at;
    l.pos = ImVec2(40.f, bottom ? display.y - 80.f : 40.f);
    Step(harness, longScene);
    const Frame f = FirstDrawIsFinal(ClickAndWatch(harness, longScene, Center(l.trigger)), label);
    Expect(f.scrollMaxY > 0.f, label + ".scrolls");
    Expect(f.size.x >= l.trigger.w, label + ".at.least.trigger", f.size.x, l.trigger.w);
    Expect(!l.rowWhole.empty() && l.rowWhole[0], label + ".widest.row.whole");
    CloseMenu(harness, longScene, label);
  }

  // Past the width cap: that item is cut, and hovering it shows it whole; an uncut one shows
  // nothing.
  Dropdown c;
  c.labels = { "Short",
    "An animation clip name far too long for any menu, exported by a tool that likes long names" };
  c.width = 120_dp * s;
  const Scene capScene = DropdownScene(c);
  Step(harness, capScene);
  {
    const std::string label = "cap" + at;
    const Frame f = FirstDrawIsFinal(ClickAndWatch(harness, capScene, Center(c.trigger)), label);
    const float cap = std::floor(360_dp * s);
    Expect(std::abs(f.size.x - cap) < 0.01f, label + ".width.is.cap", f.size.x, cap);
    Expect(c.rowWhole.size() == 2 && c.rowWhole[0], label + ".short.whole");
    Expect(c.rowWhole.size() == 2 && !c.rowWhole[1], label + ".long.cut");
    if (c.rows.size() == 2)
    {
      Hover(harness, capScene, Center(c.rows[1]));
      Expect(TooltipShown(), label + ".cut.item.tooltip");
      Hover(harness, capScene, Center(c.rows[0]));
      Expect(!TooltipShown(), label + ".whole.item.no.tooltip");
    }
    CloseMenu(harness, capScene, label);
  }

  // The trigger's tooltip: its name when given one, the value when it had to cut it, else none.
  const ImVec2 away(display.x - 10.f, display.y * 0.5f);
  Dropdown t;
  t.labels = { "1\xc3\x97" };
  const Scene tipScene = DropdownScene(t);
  Step(harness, tipScene);
  t.tooltip = "Playback speed";
  Hover(harness, tipScene, Center(t.trigger));
  Expect(TooltipShown(), "trigger.named.tooltip" + at);
  Hover(harness, tipScene, away);
  t.tooltip = nullptr;
  Hover(harness, tipScene, Center(t.trigger));
  Expect(!TooltipShown(), "trigger.whole.no.tooltip" + at);
  Hover(harness, tipScene, away);
  t.labels = { "A value much wider than the trigger it is shown in" };
  t.width = 90_dp * s;
  Step(harness, tipScene);
  Expect(!t.triggerWhole, "trigger.cut" + at);
  Hover(harness, tipScene, Center(t.trigger));
  Expect(TooltipShown(), "trigger.cut.tooltip" + at);
  Hover(harness, tipScene, away);

  // The colormap trigger's swatch gives its room to the name first: a trigger just wide enough for
  // the name beside the smallest swatch draws the name whole (at its full width the swatch left the
  // name a bare "..." in the inspector's column).
  const double ramp[] = { 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0 }; // red to blue
  const G3DWidgets::GradientStops stops{ ramp, 8 };
  bool cmapWhole = false;
  const Scene cmapScene = [&]()
  {
    const int whole = WholeVertices("Viridis");
    const float w = G3DWidgets::SelectSize({ "Viridis" }).x + (20_dp + G3DTheme::Spacing::Sm) * s;
    ImGui::SetCursorScreenPos(ImVec2(40.f, 200.f));
    ImGui::SetNextItemWidth(w);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int first = dl->VtxBuffer.Size;
    if (G3DWidgets::BeginSelectColormap("##cmap", "Viridis", stops))
    {
      G3DWidgets::EndSelect();
    }
    cmapWhole = CountVertices(dl, first, G3DTheme::U32(G3DTheme::Text())) == whole;
  };
  Step(harness, cmapScene);
  Expect(cmapWhole, "colormap.name.whole" + at);
}
}

int TestG3DSelect(int, char*[])
{
  // 18/14 is 125% as the UI actually renders it: ImGui rounds the 17.5px font to 18.
  for (const double uiScale : { 1.0, 18.0 / 14.0, 1.5, 2.0 })
  {
    CheckScale(uiScale);
  }
  if (failures > 0)
  {
    std::cerr << failures << " dropdown check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
