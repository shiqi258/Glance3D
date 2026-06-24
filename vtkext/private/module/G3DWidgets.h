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
#include <functional>

namespace G3DWidgets
{

/// Visual emphasis of a button.
enum class ButtonVariant
{
  Default, ///< neutral raised surface
  Primary, ///< accent filled (main action)
  Soft,    ///< low-intensity accent fill + accent text
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

//----------------------------------------------------------------------------
// Tree / outliner
//
// A reusable, data-source-agnostic outliner that mirrors the styleguide tree (doc/dev/
// ui-styleguide.html: <g3d-tree> / <g3d-trow>). It follows the industry "headless + slots" split:
// the widget owns *structure* (indentation rails, twisty, selection background, hit-testing, the
// drag-drop anchor) while the caller composes the *cell content* from ordinary widgets between
// BeginTreeRow()/EndTreeRow(). Three layers, smallest-to-largest:
//   1. BeginTreeRow()/EndTreeRow()  — headless row; draw any content (icon, label, an InputText for
//      inline rename, extra IconButtons, badges) in between. The row's hit area is the current ImGui
//      item right after BeginTreeRow(), so ImGui::BeginDragDropSource()/Target() attach to it.
//   2. TreeRowIcon()/TreeRowLabel()/TreeRowMeta()/TreeRowAction()  — slot helpers for the styleguide
//      default cells (the right-aligned ones handle their own placement).
//   3. TreeRow()  — convenience wrapper drawing the common "icon + label + meta + eye" row at once.
//----------------------------------------------------------------------------

/// Row-height density, mirroring styleguide <g3d-tree den="...">.
enum class TreeDensity
{
  Standard, ///< 24px rows
  Compact,  ///< 22px rows (styleguide tree default)
  Dense,    ///< 20px rows
  Comfy,    ///< 28px rows
};

/// Expand/collapse affordance state for a row.
enum class TreeTwisty
{
  Leaf,     ///< no children — no twisty drawn
  Open,     ///< expanded (chevron down)
  Collapsed ///< collapsed (chevron right)
};

/// Semantic tint for the node type icon, mirroring styleguide icv.
enum class TreeIconVariant
{
  Default, ///< muted text
  Folder,  ///< subtle (group)
  Light,   ///< warning/amber
  Tex,     ///< accent (texture/material)
  Root,    ///< accent (collection root)
};

/// Set the density (and indentation baseline) for the following tree rows. Pushes a small state
/// frame; pair each BeginTree() with one EndTree(). May nest.
void BeginTree(TreeDensity density = TreeDensity::Compact);
void EndTree();

/// Structural description of a row — everything the headless row owns (no cell content).
struct TreeRowChrome
{
  int depth = 0;                        ///< indentation level (number of rails drawn)
  TreeTwisty twisty = TreeTwisty::Leaf; ///< none / open / collapsed
  bool selected = false;                ///< full-row selection background + left accent bar
  bool focused = false;                 ///< selected and focused (deeper background)
  bool disabled = false;                ///< not interactive, dimmed
  int activeGuide = -1;                 ///< indent rail column to highlight (selection guide), -1 none
};

/// What the user clicked on a row this frame.
struct TreeRowResult
{
  bool rowClicked = false;    ///< the row body was clicked (use for selection)
  bool twistyClicked = false; ///< the expand/collapse twisty was toggled
  bool hovered = false;       ///< the row is hovered (drives trailing-action reveal, etc.)
};

/// Begin a row: paints the chrome, hit-tests the twisty and the row body, places the ImGui cursor at
/// the content start, and leaves the row's hit item as the current ImGui item (so the caller may
/// call ImGui::BeginDragDropSource()/Target() before EndTreeRow()). @p id must be unique per row.
TreeRowResult BeginTreeRow(const char* id, const TreeRowChrome& chrome);
void EndTreeRow();

/// Node type icon at the content cursor. @p dim fades it (hidden node).
void TreeRowIcon(G3DIconId icon, TreeIconVariant variant = TreeIconVariant::Default, bool dim = false);
/// Node label at the content cursor. @p group brightens it; @p dim fades it (hidden node).
void TreeRowLabel(const char* text, bool group = false, bool dim = false);
/// Right-aligned metadata (e.g. child count). Place after the label.
void TreeRowMeta(const char* text);
/// Trailing icon action button (right-aligned, reveals on row hover). @p on tints it with the accent.
/// Returns true when clicked. @p id unique within the row.
bool TreeRowAction(const char* id, G3DIconId icon, bool on = false);

/// Which part of a convenience TreeRow() was clicked this frame.
enum class TreeRowHit
{
  None,
  Row,        ///< row body (select)
  Twisty,     ///< expand/collapse
  Visibility, ///< the eye action
};

/// Convenience full row description (icon + label + optional meta + optional eye).
struct TreeRowDesc
{
  int depth = 0;
  TreeTwisty twisty = TreeTwisty::Leaf;
  G3DIconId icon = G3DIconId::Cube;
  TreeIconVariant iconVariant = TreeIconVariant::Default;
  const char* label = "";
  const char* meta = nullptr; ///< optional right-aligned metadata
  bool selected = false;
  bool focused = false;
  bool group = false;          ///< collection/group header (brighter label)
  bool hidden = false;         ///< not visible (icon + label dimmed)
  bool locked = false;         ///< reserved (no extra interaction)
  bool disabled = false;
  bool showVisibility = false; ///< include the eye/eyeoff action
  bool visible = true;         ///< eye state when showVisibility
  int activeGuide = -1;        ///< indent rail column to highlight, -1 none
};

/// Draw the common row in one call (built on BeginTreeRow + slot helpers). Returns the click hit.
TreeRowHit TreeRow(const char* id, const TreeRowDesc& desc);

/// Virtualized tree body for large node counts: only the rows currently visible in the scroll region
/// are emitted (wraps ImGuiListClipper using the row height of the active BeginTree density), so a
/// 100k-node tree stays O(on-screen rows) per frame. Call inside a scrolling region, between
/// BeginTree/EndTree. @p drawRow(i) draws the i-th currently-visible row (0-based) with BeginTreeRow/
/// TreeRow — the caller maps i to its (already flattened, collapse-resolved) node. All rows must be
/// the uniform tree row height.
void TreeVirtual(int rowCount, const std::function<void(int)>& drawRow);

} // namespace G3DWidgets

#endif
