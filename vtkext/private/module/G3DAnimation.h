/**
 * @file G3DAnimation.h
 * @brief Reusable UI animation helpers for the immediate-mode (ImGui) presenter.
 *
 * Dear ImGui is immediate-mode: it has no retained widgets to interpolate and no built-in
 * transition system (unlike a browser's CSS `transition`). The standard immediate-mode answer is to
 * drive a value toward a target each frame along an easing curve. These small, dependency-free
 * helpers package that once so panels/buttons get "near-declarative" animation and new effects are
 * easy to add:
 *
 * @code
 *   // declare once (e.g. as members):
 *   G3DAnimatedFloat slide{ 0.f, 0.22, G3DEasing::SmoothStep };
 *   G3DFrameClock clock;
 *
 *   // each frame:
 *   slide.AnimateTo(open ? 1.f : 0.f);          // like setting a CSS transition target
 *   slide.Update(clock.Tick(ImGui::GetFrameCount()));
 *   const float x = G3DLerp(closedX, openX, slide.Value());
 * @endcode
 *
 * Header-only and free of any ImGui/VTK dependency (G3DFrameClock takes a frame id rather than
 * calling ImGui), so it is trivial to reuse from any presenter and to unit-test in isolation.
 *
 * To add a new effect, either add an easing curve to G3DEasing/G3DApplyEasing, or compose
 * G3DAnimatedFloat instances (position, opacity, scale, color channels, ...) — one per animated
 * quantity.
 */

#ifndef G3DAnimation_h
#define G3DAnimation_h

#include <algorithm>
#include <chrono>
#include <cmath>

/**
 * Easing curves, each mapping a linear progress @c t in [0,1] to an eased value in [0,1].
 * Extend this enum (and G3DApplyEasing) to introduce new motion feels.
 */
enum class G3DEasing
{
  Linear,         ///< constant speed
  SmoothStep,     ///< ease in/out, symmetric — good default for open/close
  EaseOutCubic,   ///< fast start, gentle settle
  EaseInOutCubic, ///< slow start and end
  EaseOutBack,    ///< slight overshoot then settle (playful)
};

/** Apply easing curve @p easing to progress @p t (clamped to [0,1]). */
inline double G3DApplyEasing(G3DEasing easing, double t)
{
  const double x = std::clamp(t, 0.0, 1.0);
  switch (easing)
  {
    case G3DEasing::Linear:
      return x;
    case G3DEasing::SmoothStep:
      return x * x * (3.0 - 2.0 * x);
    case G3DEasing::EaseOutCubic:
    {
      const double inv = 1.0 - x;
      return 1.0 - inv * inv * inv;
    }
    case G3DEasing::EaseInOutCubic:
      return x < 0.5 ? 4.0 * x * x * x : 1.0 - std::pow(-2.0 * x + 2.0, 3.0) / 2.0;
    case G3DEasing::EaseOutBack:
    {
      constexpr double c1 = 1.70158;
      constexpr double c3 = c1 + 1.0;
      const double inv = x - 1.0;
      return 1.0 + c3 * inv * inv * inv + c1 * inv * inv;
    }
  }
  return x;
}

/** Linear interpolation from @p a to @p b by @p t. */
inline float G3DLerp(float a, float b, float t)
{
  return a + (b - a) * t;
}

/**
 * A scalar that eases toward a target over a fixed duration — the building block for transitions.
 * Re-baselines on a new target so an interrupted animation continues smoothly (like a CSS
 * transition whose target changes mid-flight).
 */
class G3DAnimatedFloat
{
public:
  explicit G3DAnimatedFloat(
    float initial = 0.f, double durationSec = 0.2, G3DEasing easing = G3DEasing::SmoothStep)
    : StartValue(initial)
    , TargetValue(initial)
    , CurrentValue(initial)
    , Elapsed(durationSec) // start settled
    , DurationSec(durationSec)
    , Easing(easing)
  {
  }

  /**
   * Head toward @p target. No-op if already targeting it; otherwise re-baselines from the current
   * (eased) value so the curve restarts cleanly even mid-animation.
   */
  void AnimateTo(float target)
  {
    if (target == this->TargetValue)
    {
      return;
    }
    this->StartValue = this->CurrentValue;
    this->TargetValue = target;
    this->Elapsed = 0.0;
  }

  /** Jump instantly to @p value (also becomes the target); no animation. */
  void Snap(float value)
  {
    this->StartValue = this->TargetValue = this->CurrentValue = value;
    this->Elapsed = this->DurationSec;
  }

  /** Advance the animation by @p dt seconds. Safe to call with dt <= 0 (no-op) or when settled. */
  void Update(double dt)
  {
    if (!this->IsAnimating())
    {
      return;
    }
    if (this->DurationSec <= 0.0)
    {
      this->CurrentValue = this->TargetValue;
      this->Elapsed = this->DurationSec;
      return;
    }
    if (dt > 0.0)
    {
      this->Elapsed += dt;
    }
    const double t = std::clamp(this->Elapsed / this->DurationSec, 0.0, 1.0);
    this->CurrentValue = G3DLerp(
      this->StartValue, this->TargetValue, static_cast<float>(G3DApplyEasing(this->Easing, t)));
  }

  float Value() const { return this->CurrentValue; } ///< eased current value, ready to render
  float Target() const { return this->TargetValue; }
  bool IsAnimating() const { return this->Elapsed < this->DurationSec; }

  void SetDuration(double sec) { this->DurationSec = sec; }
  void SetEasing(G3DEasing easing) { this->Easing = easing; }

private:
  float StartValue;
  float TargetValue;
  float CurrentValue;
  double Elapsed;
  double DurationSec;
  G3DEasing Easing;
};

/**
 * Per-frame wall-clock delta, de-duplicated by frame id: returns the elapsed seconds the first time
 * it is called in a new frame, and 0 on repeat calls within the same frame. This lets several
 * animators pull the delta, and lets callers advance from more than one place per frame, without
 * double-advancing. Uses steady_clock (not frame/ImGui time) so it keeps ticking even while a
 * blocking load stalls those clocks — same reasoning as the loading overlay animation.
 */
class G3DFrameClock
{
public:
  /** @param frameId a value that increments once per frame (e.g. ImGui::GetFrameCount()). */
  double Tick(int frameId)
  {
    if (frameId == this->LastFrame)
    {
      return 0.0; // already ticked this frame
    }
    const double now =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double delta =
      this->LastSec < 0.0 ? 0.0 : std::clamp(now - this->LastSec, 0.0, this->MaxDelta);
    this->LastSec = now;
    this->LastFrame = frameId;
    return delta;
  }

private:
  int LastFrame = -1;
  double LastSec = -1.0;
  double MaxDelta = 0.1; ///< clamp so a long stall can't make animations jump in one step
};

#endif
