// Every widget a caller may have to reserve room for publishes a measuring twin (ButtonWidth,
// BadgeHeight, ToolGroupSize...), computed by the same code that draws it. This checks each twin
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
    const float s = G3DWidgetHarness::UiScale();

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
      }
      G3DWidgets::ButtonIcon(
        "Measure me", G3DIconId::Plus, G3DWidgets::ButtonVariant::Default, density);
      if (measure)
      {
        ExpectSize(ImGui::GetItemRectSize(),
          ImVec2(G3DWidgets::ButtonWidth("Measure me", density, true),
            G3DWidgets::ButtonHeight(true, density)),
          "button.icon." + d + at);
      }
    }

    G3DWidgets::Badge("128");
    if (measure)
    {
      ExpectSize(ImGui::GetItemRectSize(),
        ImVec2(G3DWidgets::BadgeWidth("128"), G3DWidgets::BadgeHeight()), "badge" + at);
    }

    // IconButton takes a logical edge and scales it itself.
    G3DWidgets::IconButton("##twins.ib.default", G3DIconId::Plus);
    if (measure)
    {
      const float edge = G3DTheme::Size::IconButton * s;
      ExpectSize(ImGui::GetItemRectSize(), ImVec2(edge, edge), "iconbutton.default" + at);
    }
    G3DWidgets::IconButton("##twins.ib.control", G3DIconId::Plus, G3DTheme::Size::Control);
    if (measure)
    {
      const float edge = G3DTheme::Size::Control * s;
      ExpectSize(ImGui::GetItemRectSize(), ImVec2(edge, edge), "iconbutton.control" + at);
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
