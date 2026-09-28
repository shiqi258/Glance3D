// A title that shares its line with a close button must stop short of it: a long title, a large UI
// scale or a narrow panel ellipsizes the title instead of running it underneath the button.
// Checked on the two title bars that carry one — the docked PanelHeader and the floating card —
// at four UI scales, by where the vertices land: every vertex painted in the title's color stays
// left of the close glyph, with a gap, and the title still uses the room it has (it is cut back,
// not dropped).

#include "G3DTheme.h"
#include "G3DWidgetHarness.h"
#include "G3DWidgets.h"

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void Expect(bool ok, const std::string& label, float got = 0.f, float bound = 0.f)
{
  if (!ok)
  {
    std::cerr << "FAIL [" << label << "]: " << got << " vs " << bound << "\n";
    ++failures;
  }
}

// Horizontal extent of the vertices [first, dl end) painted exactly @p col.
void Extent(const ImDrawList* dl, int first, ImU32 col, float& minX, float& maxX, int& n)
{
  minX = FLT_MAX;
  maxX = -FLT_MAX;
  n = 0;
  for (int i = first; i < dl->VtxBuffer.Size; ++i)
  {
    const ImDrawVert& v = dl->VtxBuffer[i];
    if (v.col == col)
    {
      minX = std::min(minX, v.pos.x);
      maxX = std::max(maxX, v.pos.x);
      ++n;
    }
  }
}

void CheckTitle(const ImDrawList* dl, int first, G3DScale s, const std::string& label)
{
  // The title is Text at 92% (see PanelHeaderImpl / BeginFloatingCard); the close glyph is plain
  // Text. Nothing else in either title bar is painted in those two colors.
  ImVec4 titleCol = G3DTheme::Text();
  titleCol.w *= 0.92f;
  float titleMin = 0.f;
  float titleMax = 0.f;
  float closeMin = 0.f;
  float closeMax = 0.f;
  int titleN = 0;
  int closeN = 0;
  Extent(dl, first, G3DTheme::U32(titleCol), titleMin, titleMax, titleN);
  Extent(dl, first, G3DTheme::U32(G3DTheme::Text()), closeMin, closeMax, closeN);
  Expect(titleN > 0, label + ".title.drawn");
  Expect(closeN > 0, label + ".close.drawn");
  if (titleN == 0 || closeN == 0)
  {
    return;
  }
  // Clear of the button, with at least half the Sm gap the layout keeps before it.
  const float gap = G3DTheme::Spacing::Sm * s * 0.5f;
  Expect(titleMax <= closeMin - gap, label + ".title.clear.of.close", titleMax, closeMin - gap);
  // ...and cut back into the room it has, not dropped or shrunk to a stub.
  const float reach = 60_dp * s;
  Expect(titleMax >= closeMin - reach, label + ".title.uses.room", titleMax, closeMin - reach);
}

void CheckScale(double requested)
{
  G3DWidgetHarness harness(requested, ImVec2(1000.f, 700.f));
  const std::string at = " @" + std::to_string(requested);
  const char* title = "An unusually long panel title that has no chance of fitting its bar";
  G3DWidgets::FloatingCardState card;

  // Two frames: the first one builds the font atlas and bakes the icons it meets.
  for (int frame = 0; frame < 2; ++frame)
  {
    const bool measure = frame == 1;
    harness.Begin();
    const G3DScale s = G3DWidgets::UiScale();

    // A docked panel header in a narrow bar.
    ImGui::BeginChild("##bar", ImVec2(220_dp * s, 120_dp * s));
    ImDrawList* barDl = ImGui::GetWindowDrawList();
    const int barFirst = barDl->VtxBuffer.Size;
    G3DWidgets::PanelHeader(title, G3DIconId::Layers, true);
    if (measure)
    {
      CheckTitle(barDl, barFirst, s, "panelheader" + at);
    }
    ImGui::EndChild();

    // A floating card sized well below its title's width.
    G3DWidgets::FloatingCardDesc desc;
    desc.id = "##clip.card";
    desc.title = title;
    desc.icon = G3DIconId::Help;
    desc.size = ImVec2(260_dp * s, 160_dp * s);
    desc.defaultPos = ImVec2(400_dp * s, 40_dp * s);
    desc.bounds = ImVec4(0.f, 0.f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    const G3DWidgets::FloatingCardResult r = G3DWidgets::BeginFloatingCard(card, desc);
    Expect(r.visible, "card.visible" + at);
    if (r.visible)
    {
      if (measure)
      {
        CheckTitle(ImGui::GetWindowDrawList(), 0, s, "floatingcard" + at);
      }
      G3DWidgets::EndFloatingCard();
    }

    harness.End();
  }
}
}

int TestG3DHeaderTitleClip(int, char*[])
{
  // 18/14 is 125% as the UI actually renders it: ImGui rounds the 17.5px font to 18.
  for (const double uiScale : { 1.0, 18.0 / 14.0, 1.5, 2.0 })
  {
    CheckScale(uiScale);
  }
  if (failures > 0)
  {
    std::cerr << failures << " title clip check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
