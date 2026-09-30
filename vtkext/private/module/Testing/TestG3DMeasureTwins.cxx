// Every widget a caller may have to reserve room for publishes a measuring twin (ButtonSize,
// BadgeSize, IconButtonSize, SegmentedIconSize, SelectSize, ToolGroupSize...), computed by the same
// code that draws it — the widths a FieldRow is laid out from. This checks each twin
// against the item the widget actually submits, at several UI scales: a twin that drifts from its
// widget is the reservation bug (a row sized for one width, drawn at another), and it only shows
// at a scale other than 1.
//
// It also pins IconButton's unit: its size is a logical length the widget scales itself, which is
// what callers passing an already scaled size got wrong.

#include "G3DTheme.h"
#include "G3DWidgetHarness.h"
#include "G3DWidgets.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void ExpectNear(float got, float want, const std::string& label)
{
  if (std::abs(got - want) > 0.01f)
  {
    std::cerr << "FAIL [" << label << "]: got " << got << " want " << want << "\n";
    ++failures;
  }
}

void ExpectSize(const ImVec2& got, const ImVec2& want, const std::string& label)
{
  ExpectNear(got.x, want.x, label + ".w");
  ExpectNear(got.y, want.y, label + ".h");
}

void CheckScale(double uiScale)
{
  G3DWidgetHarness harness(uiScale);
  const std::string at = " @" + std::to_string(uiScale);
  const G3DWidgets::ButtonDensity densities[] = { G3DWidgets::ButtonDensity::Default,
    G3DWidgets::ButtonDensity::Compact };

  // Two frames: the first one builds the font atlas and bakes the icons it meets.
  for (int frame = 0; frame < 2; ++frame)
  {
    const bool measure = frame == 1;
    harness.Begin();
    const G3DScale s = G3DWidgets::UiScale();

    for (G3DWidgets::ButtonDensity density : densities)
    {
      const std::string d = density == G3DWidgets::ButtonDensity::Compact ? "compact" : "default";
      G3DWidgets::Button("Measure me", G3DWidgets::ButtonVariant::Default, density);
      if (measure)
      {
        ExpectSize(ImGui::GetItemRectSize(),
          ImVec2(G3DWidgets::ButtonWidth("Measure me", density, false),
            G3DWidgets::ButtonHeight(false, density)),
          "button." + d + at);
        ExpectSize(ImGui::GetItemRectSize(), G3DWidgets::ButtonSize("Measure me", density, false),
          "buttonsize." + d + at);
      }
      G3DWidgets::ButtonIcon(
        "Measure me", G3DIconId::Plus, G3DWidgets::ButtonVariant::Default, density);
      if (measure)
      {
        ExpectSize(ImGui::GetItemRectSize(),
          ImVec2(G3DWidgets::ButtonWidth("Measure me", density, true),
            G3DWidgets::ButtonHeight(true, density)),
          "button.icon." + d + at);
        ExpectSize(ImGui::GetItemRectSize(), G3DWidgets::ButtonSize("Measure me", density, true),
          "buttonsize.icon." + d + at);
      }
    }

    G3DWidgets::Badge("128");
    if (measure)
    {
      ExpectSize(ImGui::GetItemRectSize(),
        ImVec2(G3DWidgets::BadgeWidth("128"), G3DWidgets::BadgeHeight()), "badge" + at);
      ExpectSize(ImGui::GetItemRectSize(), G3DWidgets::BadgeSize("128"), "badgesize" + at);
    }

    // IconButton takes a logical edge and scales it itself; IconButtonSize publishes that box.
    G3DWidgets::IconButton("##twins.ib.default", G3DIconId::Plus);
    if (measure)
    {
      const float edge = G3DTheme::Size::IconButton * s;
      ExpectSize(ImGui::GetItemRectSize(), ImVec2(edge, edge), "iconbutton.default" + at);
      ExpectSize(ImGui::GetItemRectSize(), G3DWidgets::IconButtonSize(),
        "iconbuttonsize.default" + at);
    }
    G3DWidgets::IconButton("##twins.ib.control", G3DIconId::Plus, G3DTheme::Size::Control);
    if (measure)
    {
      const float edge = G3DTheme::Size::Control * s;
      ExpectSize(ImGui::GetItemRectSize(), ImVec2(edge, edge), "iconbutton.control" + at);
      ExpectSize(ImGui::GetItemRectSize(), G3DWidgets::IconButtonSize(G3DTheme::Size::Control),
        "iconbuttonsize.control" + at);
    }

    // A segmented control submits one item per segment: its box spans the first segment's
    // top-left corner to the last one's bottom-right.
    const G3DWidgets::SegmentedIconItem segs[3] = { { G3DIconId::PanelLeft, nullptr, true, false },
      { G3DIconId::PanelRight, nullptr, false, false },
      { G3DIconId::PanelBottom, nullptr, false, true } };
    const ImVec2 segOrigin = ImGui::GetCursorScreenPos();
    G3DWidgets::SegmentedIcon("##twins.seg", segs, 3);
    if (measure)
    {
      const ImVec2 segEnd = ImGui::GetItemRectMax();
      ExpectSize(ImVec2(segEnd.x - segOrigin.x, segEnd.y - segOrigin.y),
        G3DWidgets::SegmentedIconSize(3), "segmented" + at);
    }

    // A dropdown trigger reserved with SelectSize: the box it submits is that size (TestG3DSelect
    // checks that the values then show whole). The widest label sets the width, not the current.
    const char* speeds[] = { "0.1\xc3\x97", "0.25\xc3\x97", "4\xc3\x97" };
    ImGui::SetNextItemWidth(G3DWidgets::SelectSize(speeds).x);
    G3DWidgets::BeginSelect("##twins.select", speeds[2]); // closed: nothing to end
    if (measure)
    {
      ExpectSize(ImGui::GetItemRectSize(), G3DWidgets::SelectSize(speeds), "selectsize" + at);
      ExpectSize(G3DWidgets::SelectSize({ "0.25\xc3\x97" }), G3DWidgets::SelectSize(speeds),
        "selectsize.widest" + at);
    }

    G3DWidgets::ToolItem items[3];
    items[0].id = "##twins.tg.a";
    items[1].id = "##twins.tg.b";
    items[2].id = "##twins.tg.c";
    items[2].separatorBefore = true;
    for (const bool framed : { false, true })
    {
      G3DWidgets::ToolGroupDesc desc;
      desc.items = items;
      desc.count = 3;
      desc.framed = framed;
      G3DWidgets::ToolGroup(framed ? "##twins.tg.framed" : "##twins.tg.bare", desc);
      if (measure)
      {
        ExpectSize(ImGui::GetItemRectSize(), G3DWidgets::ToolGroupSize(desc),
          std::string(framed ? "toolgroup.framed" : "toolgroup.bare") + at);
      }
    }

    harness.End();
  }
}
}

int TestG3DMeasureTwins(int, char*[])
{
  // 18/14 is 125% as the UI actually renders it: ImGui rounds the 17.5px font to 18.
  for (const double uiScale : { 1.0, 18.0 / 14.0, 1.5, 2.0 })
  {
    CheckScale(uiScale);
  }
  if (failures > 0)
  {
    std::cerr << failures << " measuring twin check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
