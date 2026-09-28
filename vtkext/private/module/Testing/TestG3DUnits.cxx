// The UI unit types promise mostly that a wrong expression does not compile: a design constant
// cannot reach ImGui unscaled, and an already scaled length cannot be scaled again. Those promises
// are checked below with static_assert over requires-expressions, so this test fails at BUILD time
// when one of them breaks. The runtime half is the scale quantization: a table, the formula ImGui
// rounds font sizes with, and (UI builds) the font size a real ImGui context ends up rendering.

#include "G3DUnits.h"

#if F3D_MODULE_UI
#include "G3DWidgetHarness.h"

#include <imgui_internal.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>

namespace
{
template <class A, class B>
concept Addable = requires(A a, B b) { a + b; };
template <class A, class B>
concept Subtractable = requires(A a, B b) { a - b; };
template <class A, class B>
concept Multipliable = requires(A a, B b) { a * b; };
template <class A, class B>
concept Divisible = requires(A a, B b) { a / b; };
template <class A, class B>
concept LessComparable = requires(A a, B b) { a < b; };
template <class A, class B>
concept AddAssignable = requires(A& a, B b) { a += b; };

template <class A, class B>
using Product = decltype(std::declval<A>() * std::declval<B>());
template <class A, class B>
using Quotient = decltype(std::declval<A>() / std::declval<B>());

// Zero cost: a unit is exactly the float it wraps.
static_assert(sizeof(G3DDp) == sizeof(float));
static_assert(sizeof(G3DScale) == sizeof(float));
static_assert(sizeof(G3DDp2) == 2 * sizeof(float));
static_assert(std::is_trivially_copyable_v<G3DDp>);
static_assert(std::is_trivially_copyable_v<G3DScale>);
static_assert(std::is_trivially_copyable_v<G3DDp2>);
static_assert(std::is_aggregate_v<G3DDp2>);

// What compiles, and the unit it yields.
static_assert(std::is_same_v<Product<G3DDp, G3DScale>, float>); // the one conversion
static_assert(std::is_same_v<Product<G3DScale, G3DDp>, float>);
static_assert(std::is_same_v<Product<G3DDp, float>, G3DDp>); // a proportion of a length
static_assert(std::is_same_v<Product<float, G3DDp>, G3DDp>);
static_assert(std::is_same_v<Quotient<G3DDp, float>, G3DDp>);
static_assert(std::is_same_v<Quotient<G3DDp, G3DDp>, float>); // a ratio has no unit
static_assert(std::is_same_v<decltype(G3DScale().ToDp(1.f)), G3DDp>);

// Missed scaling: a design constant cannot reach a float parameter (ImGui) without the scale, and a
// bare number is not a unit without saying so.
static_assert(!std::is_convertible_v<G3DDp, float>);
static_assert(!std::is_convertible_v<float, G3DDp>);
static_assert(!std::is_convertible_v<int, G3DDp>);
static_assert(!std::is_convertible_v<G3DScale, float>);
static_assert(!std::is_convertible_v<float, G3DScale>);
static_assert(!std::is_convertible_v<G3DDp, G3DScale>);

// Double scaling: a physical length cannot be scaled again, whatever arithmetic type it is in.
static_assert(!Multipliable<float, G3DScale>);
static_assert(!Multipliable<G3DScale, float>);
static_assert(!Multipliable<double, G3DScale>);
static_assert(!Multipliable<G3DScale, double>);
static_assert(!Multipliable<int, G3DScale>);
static_assert(!Multipliable<G3DScale, int>);
static_assert(!Divisible<float, G3DScale>);
static_assert(!Divisible<int, G3DScale>);
static_assert(!Divisible<G3DDp, G3DScale>);
static_assert(!Multipliable<G3DScale, G3DScale>);

// Unit mixing: logical and physical lengths neither combine nor compare.
static_assert(!Addable<G3DDp, float>);
static_assert(!Addable<float, G3DDp>);
static_assert(!Subtractable<G3DDp, float>);
static_assert(!Subtractable<float, G3DDp>);
static_assert(!AddAssignable<G3DDp, float>);
static_assert(!AddAssignable<float, G3DDp>);
static_assert(!LessComparable<G3DDp, float>);
static_assert(!LessComparable<float, G3DDp>);
static_assert(!Multipliable<G3DDp, G3DDp>); // px^2 is not a UI length
static_assert(!Addable<G3DDp2, G3DDp>);
static_assert(!Addable<G3DDp2, float>);

// The arithmetic itself.
static_assert(G3DDp().Raw() == 0.f);
static_assert(G3DScale().Factor() == 1.f);
static_assert((12_dp + 4_dp).Raw() == 16.f);
static_assert((12_dp - 4_dp).Raw() == 8.f);
static_assert((-4_dp).Raw() == -4.f);
static_assert((10_dp * 0.5f).Raw() == 5.f);
static_assert((0.5f * 10_dp).Raw() == 5.f);
static_assert((10_dp / 4.f).Raw() == 2.5f);
static_assert(10_dp / 4_dp == 2.5f);
static_assert((2.5_dp).Raw() == 2.5f);
static_assert(12_dp * G3DScale(1.5f) == 18.f);
static_assert(G3DScale(1.5f) * 12_dp == 18.f);
static_assert(G3DScale(2.f).ToDp(24.f) == 12_dp);
static_assert(4_dp < 8_dp && 8_dp > 4_dp && 4_dp <= 4_dp && 4_dp != 8_dp);
static_assert(std::max(4_dp, 8_dp) == 8_dp);
static_assert(std::clamp(12_dp, 4_dp, 8_dp) == 8_dp);

constexpr G3DDp CompoundAssign()
{
  G3DDp d = 10_dp;
  d += 4_dp;
  d -= 2_dp;
  d *= 0.5f;
  return d;
}
static_assert(CompoundAssign() == 6_dp);

static_assert(G3DDp2{ 10_dp, 6_dp } + G3DDp2{ 2_dp, 2_dp } == G3DDp2{ 12_dp, 8_dp });
static_assert(G3DDp2{ 10_dp, 6_dp } - G3DDp2{ 2_dp, 2_dp } == G3DDp2{ 8_dp, 4_dp });
static_assert(G3DDp2{ 10_dp, 6_dp } * 0.5f == G3DDp2{ 5_dp, 3_dp });
static_assert(G3DDp2{}.x == 0_dp && G3DDp2{}.y == 0_dp);

// The scale quantization: requested scale -> the font size ImGui renders the 14px face at.
struct QuantizeCase
{
  double requested;
  float fontPx;
};
constexpr QuantizeCase kQuantizeCases[] = {
  { 1.0, 14.f },
  { 1.1, 15.f },   // 15.4
  { 1.2, 17.f },   // 16.8
  { 1.25, 18.f },  // 17.5 rounds up: the 125% monitor setting
  { 1.3, 18.f },   // 18.2
  { 1.5, 21.f },
  { 1.75, 25.f },  // 24.5
  { 2.0, 28.f },
  { 2.25, 32.f },  // 31.5
  { 2.5, 35.f },
  { 3.0, 42.f },
  { 0.5, 7.f },
  { 0.01, 1.f },   // ImGui never renders a font below 1px
  { 0.0, 1.f },
  { -1.0, 1.f },
  { 100.0, 512.f }, // ...nor above IMGUI_FONT_SIZE_MAX
};

constexpr bool QuantizeTableHolds()
{
  for (const QuantizeCase& c : kQuantizeCases)
  {
    const G3DScale scale = G3DQuantizeUiScale(c.requested);
    if (scale.Factor() != c.fontPx / 14.f || G3DBaseFontSize * scale != c.fontPx)
    {
      return false;
    }
  }
  return true;
}
static_assert(QuantizeTableHolds());
static_assert(G3DBaseFontSize == 14_dp);

int failures = 0;

void Fail(const char* what, double requested, float got, float want)
{
  std::cerr << "FAIL [" << what << " @" << requested << "]: got " << got << " want " << want << "\n";
  ++failures;
}

void CheckQuantizeAtRuntime()
{
  for (const QuantizeCase& c : kQuantizeCases)
  {
    // volatile: evaluate the same function at run time, where a compiler could round differently
    // from its constant evaluator.
    volatile double requested = c.requested;
    const G3DScale scale = G3DQuantizeUiScale(requested);
    if (scale.Factor() != c.fontPx / 14.f)
    {
      Fail("quantize.factor", c.requested, scale.Factor(), c.fontPx / 14.f);
    }
    if (G3DBaseFontSize * scale != c.fontPx)
    {
      Fail("quantize.font", c.requested, G3DBaseFontSize * scale, c.fontPx);
    }

#if F3D_MODULE_UI
    // The two statements ImGui sizes every font with (ImGui::UpdateCurrentFontSize): if a future
    // ImGui stops rounding, this is where it shows.
    const float imguiPx = ImClamp(ImGui::GetRoundedFontSize(static_cast<float>(14.0 * requested)),
      1.f, IMGUI_FONT_SIZE_MAX);
    if (imguiPx != G3DBaseFontSize * scale)
    {
      Fail("quantize.imgui_rounding", c.requested, G3DBaseFontSize * scale, imguiPx);
    }
#endif
  }
}

#if F3D_MODULE_UI
// End to end: a context built the way the app builds its own gives the text, the style and the
// widgets one and the same scale -- the UI face renders at the base font size times the quantized
// scale, ScaleAllSizes ran with that factor, and the widget library lays out with it.
void CheckOneScaleEverywhere()
{
  for (const double requested : { 1.0, 1.25, 1.5, 1.75, 2.0 })
  {
    const G3DScale want = G3DQuantizeUiScale(requested);
    G3DWidgetHarness harness(requested);
    harness.Begin();
    const float rendered = ImGui::GetFontSize();
    const float styleScale = ImGui::GetStyle()._MainScale;
    const G3DScale widgets = G3DWidgets::UiScale();
    harness.End();
    if (rendered != G3DBaseFontSize * want)
    {
      Fail("rendered_font", requested, rendered, G3DBaseFontSize * want);
    }
    if (styleScale != want.Factor())
    {
      Fail("style_scale", requested, styleScale, want.Factor());
    }
    if (!(widgets == want))
    {
      Fail("widget_scale", requested, widgets.Factor(), want.Factor());
    }
  }
}
#endif
}

int TestG3DUnits(int, char*[])
{
  CheckQuantizeAtRuntime();
#if F3D_MODULE_UI
  CheckOneScaleEverywhere();
#endif
  if (failures > 0)
  {
    std::cerr << failures << " unit check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
