// The field row lays all of its slots out before any control is drawn (G3DLayout::SolveRow, pure
// geometry) and then hands each control its slot. Checked in two halves:
//  - the solver on its own: the fill slot takes the rest, the fixed slots after it end on the
//    right edge, droppable slots go highest priority first, the fill keeps its minimum once nothing
//    is left to drop, per-slot gaps;
//  - rows drawn in a headless context built like the app's, at four UI scales, against the items
//    the controls actually submit. One of them is the inspector's Range row, whose auto-range
//    button was once drawn at 56 px inside a 37.5 px reservation at 1.5x: it has to be a
//    Size::Control square on the value column's right edge at every scale.

#include "G3DLayout.h"
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

void Expect(bool ok, const std::string& label)
{
  if (!ok)
  {
    std::cerr << "FAIL [" << label << "]\n";
    ++failures;
  }
}

void ExpectNear(float got, float want, const std::string& label)
{
  if (std::abs(got - want) > 0.01f)
  {
    std::cerr << "FAIL [" << label << "]: got " << got << " want " << want << "\n";
    ++failures;
  }
}

G3DLayout::RowSlot Fixed(float width, int drop = 0)
{
  G3DLayout::RowSlot s;
  s.width = width;
  s.drop = drop;
  return s;
}

G3DLayout::RowSlot Fill(float minWidth = 0.f)
{
  G3DLayout::RowSlot s;
  s.width = minWidth;
  s.fill = true;
  return s;
}

void TestSolver()
{
  using G3DLayout::RowPlace;
  using G3DLayout::RowSlot;

  {
    // The fill takes what is left; the fixed slot after it ends exactly on the right edge.
    const RowSlot slots[] = { Fixed(30.f), Fill(20.f), Fixed(25.f) };
    RowPlace out[3];
    Expect(G3DLayout::SolveRow(slots, 3, 200.f, 4.f, out), "solver.fill.fits");
    ExpectNear(out[0].x, 0.f, "solver.fill.x0");
    ExpectNear(out[1].x, 34.f, "solver.fill.x1");
    ExpectNear(out[1].w, 200.f - 30.f - 25.f - 2.f * 4.f, "solver.fill.w1");
    ExpectNear(out[2].x + out[2].w, 200.f, "solver.fill.right");
  }
  {
    // Too narrow for the fill's minimum: the highest priority goes first, the later one on a tie,
    // and only as many as it takes.
    const RowSlot slots[] = { Fixed(40.f, 1), Fill(50.f), Fixed(40.f, 2), Fixed(40.f, 2) };
    RowPlace out[4];
    Expect(G3DLayout::SolveRow(slots, 4, 150.f, 0.f, out), "solver.drop.fits");
    Expect(out[0].shown, "solver.drop.keeps.low");
    Expect(out[2].shown, "solver.drop.keeps.earlier.tie");
    Expect(!out[3].shown, "solver.drop.drops.later.tie");
    ExpectNear(out[1].w, 150.f - 80.f, "solver.drop.fill.w");
    ExpectNear(out[3].w, 0.f, "solver.drop.dropped.w");
  }
  {
    // Nothing left to drop: the fill keeps its minimum and the row runs past its width, rather
    // than squeezing a control into a sliver or stacking the trailing slots over the leading ones.
    const RowSlot slots[] = { Fixed(100.f), Fill(50.f), Fixed(100.f) };
    RowPlace out[3];
    Expect(!G3DLayout::SolveRow(slots, 3, 180.f, 0.f, out), "solver.overflow.reported");
    ExpectNear(out[1].w, 50.f, "solver.overflow.fill.min");
    ExpectNear(out[2].x, 150.f, "solver.overflow.no.overlap");
  }
  {
    // No fill slot: left-aligned, and an overflow drops the droppable slot.
    const RowSlot slots[] = { Fixed(50.f), Fixed(50.f, 1), Fixed(50.f) };
    RowPlace out[3];
    Expect(G3DLayout::SolveRow(slots, 3, 120.f, 5.f, out), "solver.nofill.fits");
    Expect(!out[1].shown, "solver.nofill.dropped");
    ExpectNear(out[2].x, 55.f, "solver.nofill.x2");
  }
  {
    // A slot with its own gap; the row gap everywhere else.
    RowSlot second = Fixed(10.f);
    second.gapBefore = 20.f;
    const RowSlot slots[] = { Fixed(10.f), second, Fill() };
    RowPlace out[3];
    Expect(G3DLayout::SolveRow(slots, 3, 100.f, 4.f, out), "solver.gap.fits");
    ExpectNear(out[1].x, 30.f, "solver.gap.own");
    ExpectNear(out[2].x, 44.f, "solver.gap.row");
    ExpectNear(out[2].w, 56.f, "solver.gap.fill.w");
  }
}

void CheckRows(double requested)
{
  using G3DWidgets::FieldSlot;
  G3DWidgetHarness harness(requested);
  const std::string at = " @" + std::to_string(requested);

  // Two frames: the first one builds the font atlas and bakes the icons it meets.
  for (int frame = 0; frame < 2; ++frame)
  {
    const bool measure = frame == 1;
    harness.Begin();
    const G3DScale s = G3DWidgets::UiScale();
    const float gap = G3DTheme::Spacing::Xs * s;

    // --- The inspector's Range row: a fill slider and a trailing auto-range button, as the value
    // column of a property row.
    G3DWidgets::BeginPropRow("Range");
    const float valueLeft = ImGui::GetCursorScreenPos().x;
    const float valueW = ImGui::GetContentRegionAvail().x;
    const float valueRight = valueLeft + valueW;
    const float rowTop = ImGui::GetCursorScreenPos().y;
    const float btn = G3DWidgets::IconButtonSize(G3DTheme::Size::Control).x;
    G3DWidgets::BeginFieldRow("##range", { FieldSlot::Fill(), FieldSlot::Fixed(btn) });
    float lo = 0.2f;
    float hi = 0.8f;
    Expect(G3DWidgets::FieldRowNext(), "range.slot0.shown" + at);
    G3DWidgets::RangeSliderFloat("##v", &lo, &hi, 0.f, 1.f);
    const ImVec2 sliderMin = ImGui::GetItemRectMin();
    const ImVec2 sliderMax = ImGui::GetItemRectMax();
    Expect(G3DWidgets::FieldRowNext(), "range.slot1.shown" + at);
    G3DWidgets::IconButton("##fit", G3DIconId::Fit, G3DTheme::Size::Control);
    const ImVec2 btnMin = ImGui::GetItemRectMin();
    const ImVec2 btnMax = ImGui::GetItemRectMax();
    G3DWidgets::EndFieldRow();
    G3DWidgets::EndPropRow();
    if (measure)
    {
      ExpectNear(btnMax.x - btnMin.x, G3DTheme::Size::Control * s, "range.button.w" + at);
      ExpectNear(btnMax.y - btnMin.y, G3DTheme::Size::Control * s, "range.button.h" + at);
      // Flush with the value column's right edge; the slider takes the rest in whole pixels.
      ExpectNear(btnMax.x, valueRight, "range.button.right" + at);
      ExpectNear(sliderMin.x, valueLeft, "range.slider.left" + at);
      ExpectNear(sliderMax.x - sliderMin.x, std::floor(valueW - btn - gap), "range.slider.w" + at);
      Expect(btnMin.x - sliderMax.x >= gap - 0.01f && btnMin.x - sliderMax.x < gap + 1.f,
        "range.gap" + at);
      ExpectNear(sliderMin.y, rowTop, "range.slider.top" + at);
    }

    // --- Vertical centering, and the row as one item: a Fab next to an icon button.
    const ImVec2 row2 = ImGui::GetCursorScreenPos();
    const ImVec2 fab = G3DWidgets::IconButtonSize(G3DTheme::Size::Fab);
    const ImVec2 ib = G3DWidgets::IconButtonSize();
    G3DWidgets::BeginFieldRow("##transport",
      { FieldSlot::Fixed(fab.x, fab.y), FieldSlot::Fixed(ib.x, ib.y), FieldSlot::Fill() });
    G3DWidgets::FieldRowNext();
    G3DWidgets::IconButton("##play", G3DIconId::Play, G3DTheme::Size::Fab, true);
    G3DWidgets::FieldRowNext();
    G3DWidgets::IconButton("##step", G3DIconId::StepForward);
    const ImVec2 stepMin = ImGui::GetItemRectMin();
    G3DWidgets::FieldRowNext(); // an empty fill: a spacer
    G3DWidgets::EndFieldRow();
    const float after = ImGui::GetCursorScreenPos().y;
    if (measure)
    {
      ExpectNear(stepMin.y - row2.y, (fab.y - ib.y) * 0.5f, "transport.centered" + at);
      ExpectNear(stepMin.x - row2.x, fab.x + gap, "transport.x" + at);
      ExpectNear(after, std::floor(row2.y + fab.y + ImGui::GetStyle().ItemSpacing.y),
        "transport.next.line" + at);
    }

    // --- A toolbar too narrow for everything: the droppable slot goes, the rest still ends on the
    // right edge, and the fill takes the room it left.
    const float narrowW = 120_dp * s;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::BeginChild("##narrow", ImVec2(narrowW, 100_dp * s));
    const float left = ImGui::GetCursorScreenPos().x;
    const float rowW = ImGui::GetContentRegionAvail().x; // the child's width, in whole pixels
    static char query[32] = "";
    G3DWidgets::BeginFieldRow("##toolbar",
      { FieldSlot::Fill(60_dp * s), FieldSlot::Fixed(40_dp * s, 0.f, 1), FieldSlot::Fixed(btn) });
    Expect(G3DWidgets::FieldRowNext(), "toolbar.fill.shown" + at);
    G3DWidgets::InputText("##q", query, sizeof(query), "Search");
    const ImVec2 inMin = ImGui::GetItemRectMin();
    const ImVec2 inMax = ImGui::GetItemRectMax();
    Expect(!G3DWidgets::FieldRowNext(), "toolbar.dropped" + at);
    Expect(G3DWidgets::FieldRowNext(), "toolbar.button.shown" + at);
    G3DWidgets::IconButton("##expand", G3DIconId::ExpandAll, G3DTheme::Size::Control);
    const ImVec2 exMax = ImGui::GetItemRectMax();
    G3DWidgets::EndFieldRow();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    if (measure)
    {
      ExpectNear(exMax.x, left + rowW, "toolbar.button.right" + at);
      ExpectNear(inMin.x, left, "toolbar.fill.x" + at);
      ExpectNear(inMax.x - inMin.x, std::floor(rowW - btn - gap), "toolbar.fill.w" + at);
    }

    harness.End();
  }
}
}

int TestG3DFieldRow(int, char*[])
{
  TestSolver();
  // 18/14 is 125% as the UI actually renders it: ImGui rounds the 17.5px font to 18.
  for (const double uiScale : { 1.0, 18.0 / 14.0, 1.5, 2.0 })
  {
    CheckRows(uiScale);
  }
  if (failures > 0)
  {
    std::cerr << failures << " field row check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
