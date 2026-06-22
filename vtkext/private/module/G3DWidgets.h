/**
 * @file G3DWidgets.h
 * @brief Lightweight ImGui widget library for Glance3D, with built-in hover/press/state transitions.
 *
 * Immediate-mode widgets that animate: each interactive widget keeps a small per-ImGuiID animation
 * state (hover / press / on-or-open) persisted across frames and advanced by a shared frame clock,
 * so hover, press, toggle and similar transitions are smooth without callers writing any tween code.
 * Visuals are self-drawn with ImDrawList using the design tokens in G3DTheme and icons from G3DIcon,
 * and built on the G3DAnimation primitives. Public ImGui API only (no imgui_internal).
 *
 * Extending: add a button variant to ButtonVariant; add an icon in G3DIcon; tune feel via the motion
 * presets in G3DTheme. New widgets follow the same pattern (InvisibleButton hit-test -> Interact()
 * state -> ImDrawList paint with tokens).
 */

#ifndef G3DWidgets_h
#define G3DWidgets_h

#include "G3DIcon.h"

#include <cstddef>

namespace G3DWidgets
{

/// Visual emphasis of a button.
enum class ButtonVariant
{
  Default, ///< neutral raised surface
  Primary, ///< accent filled (main action)
  Ghost,   ///< transparent until hovered (toolbars)
  Danger,  ///< destructive action
};

/// Text button. Returns true on click.
bool Button(const char* label, ButtonVariant variant = ButtonVariant::Default);

/// Text button with a leading icon. Returns true on click.
bool ButtonIcon(const char* label, G3DIconId icon, ButtonVariant variant = ButtonVariant::Default);

/// Square icon-only button (toolbar / FAB style). @p size <= 0 uses the icon-button token; @p round
/// makes it a pill/circle. Returns true on click.
bool IconButton(
  const char* id, G3DIconId icon, float size = -1.f, bool round = false, const char* tooltip = nullptr);

/// Styled card container. Call EndCard() exactly once for each BeginCard(). @p hoverable adds a hover
/// tint and makes EndCard() return whether the card was clicked. Always returns true (draw content).
bool BeginCard(const char* id, bool hoverable = false, float padding = -1.f);
bool EndCard();

/// Muted, smaller-feeling group heading.
void SectionTitle(const char* text);

/// Animated on/off switch. Returns true when toggled this frame.
bool Toggle(const char* label, bool* v);

/// Checkbox with an animated check. Returns true when toggled this frame.
bool Checkbox(const char* label, bool* v);

/// Styled slider (pill track + fill + grab, hover glow). Returns true when the value changed.
bool SliderFloat(
  const char* label, float* v, float vMin, float vMax, const char* format = "%.2f");

/// Styled single-line text input (focus underline + border highlight). Returns true when edited.
bool InputText(const char* label, char* buf, std::size_t bufSize, const char* hint = nullptr);

/// Tooltip for the last item, with a unified hover delay.
void ItemTooltip(const char* text);

} // namespace G3DWidgets

#endif
