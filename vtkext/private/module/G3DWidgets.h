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
 *
 * ROUNDED CORNERS — exactly two sanctioned ways, pick by fill type:
 *  - SOLID fill: ImDrawList::AddRectFilled(..., rounding) under an AAGuard. Native, smooth, one
 *    draw. Never carve a solid fill.
 *  - NON-SOLID fill (gradients via AddRectFilledMultiColor, checkerboards, any layered content):
 *    ImGui cannot round those. Draw the content SQUARE, then call CarveRoundedCorners() (in
 *    G3DWidgets.cxx) to clip the corners back to the color the shape sits on. Do NOT hand-roll a
 *    corner mask with PathFillConcave / arc strokes — the notch shape degenerates ImGui's AA fill
 *    (miter blow-up at the tangent points), its concave triangulator (45-degree mis-ear) and open
 *    stroke caps (steps); CarveRoundedCorners exists because all three were hit and measured.
 */

#ifndef G3DWidgets_h
#define G3DWidgets_h

#include "G3DIcon.h"

#include <cstddef>
#include <functional>
#include <vector>

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

/// Overline group heading (uppercase-feeling, subtle, letter-spaced) — mirrors styleguide
/// .sectiontitle. Use to label a group of rows inside a panel.
void SectionTitle(const char* text);

/// Full-width hairline divider with vertical breathing room (mirrors styleguide .divider).
void Divider();

/// Panel header: a title with a full-width hairline beneath it, spanning the whole panel width
/// (ignores window padding). Use at the very top of a docked panel / sidebar to title it (mirrors
/// styleguide .tree-toolbar title row). The second overload prefixes an accent-tinted leading icon.
void PanelHeader(const char* title);
void PanelHeader(const char* title, G3DIconId icon);

/// Read-only key/value row for inspectors / stat panels: muted label on the left, primary value
/// right-aligned on the same line (mirrors styleguide .proprow used read-only).
void StatRow(const char* key, const char* value);

/// Collapsible property-panel header (the signature DCC inspector panel, e.g. Blender's Transform /
/// Relations): a full-width clickable header with a disclosure triangle + title on a subtle raised
/// surface; clicking toggles @p open. Returns whether the section is open, so the caller guards its
/// content with `if (CollapsingSection(...)) { ... }`. @p open persists the state across frames.
/// Lightweight header-only helper; for the full styleguide panel (card chrome, icon, count, hover
/// actions, enable toggle, accordion, variants) use BeginCollapse()/EndCollapse() below.
bool CollapsingSection(const char* label, bool* open);

/// Pill badge tint.
enum class BadgeVariant
{
  Neutral, ///< subtle surface fill, muted text
  Accent,  ///< accent-soft fill, accent text
};

/// Small inline pill badge (mirrors styleguide g3d-badge). Advances the layout cursor like a normal
/// item, so it composes with ImGui::SameLine().
void Badge(const char* text, BadgeVariant variant = BadgeVariant::Neutral);

/// Width a Badge() would occupy for @p text (for right-aligning a trailing badge).
float BadgeWidth(const char* text);

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

//----------------------------------------------------------------------------
// Collapse / accordion
//
// The full styleguide collapsible panel (doc/dev/ui-styleguide.html: <g3d-collapse> /
// <g3d-accordion>) — the compact, editor-style disclosure group used to organize an inspector.
// A clickable header (twisty + optional type icon + title + optional count pill + hover-revealed
// actions + optional enable toggle) controls a collapsible body, whose content the caller composes
// from ordinary widgets between BeginCollapse()/EndCollapse() (guarded by the returned open flag).
// BeginAccordion()/EndAccordion() stitch a run of panels into one seamless bordered list.
//
// Body open/close is instant (the styleguide's smooth height tween maps to ImGui's immediate-mode
// CollapsingHeader); the header hover and chevron follow the shared animation clock like every
// other widget. The node-icon tint reuses TreeIconVariant (icv) for consistency with the tree.
//----------------------------------------------------------------------------

/// Visual style of a collapsible panel — mirrors styleguide <g3d-collapse v="...">.
enum class CollapseVariant
{
  Card,     ///< standalone card: surface-2 fill, hairline border, rounded (the default)
  Sub,      ///< nested sub-panel (Blender sub-panels): transparent, indented body, lighter title
  Ghost,    ///< borderless, transparent — inline grouping inside another container
  Overline, ///< card chrome but an uppercase-feeling tiny subtle title (VS Code sidebar section)
};

/// Header / body density — mirrors styleguide den="compact|dense" (header 36 / 30 / 26 px).
enum class CollapseDensity
{
  Default,
  Compact,
  Dense,
};

/// A trailing header action button, revealed on header hover (mirrors styleguide act="..."). The
/// Blender / Figma "actions appear on hover" affordance.
struct CollapseAction
{
  const char* id = "";              ///< unique within this header
  G3DIconId icon = G3DIconId::Dots;
  bool on = false;                  ///< accent-tinted active state (then kept visible)
};

/// Everything the header owns (the body content is drawn by the caller).
struct CollapseDesc
{
  const char* title = "";
  bool hasIcon = false;                                   ///< draw a leading type icon
  G3DIconId icon = G3DIconId::Cube;
  TreeIconVariant iconVariant = TreeIconVariant::Default; ///< icv tint (accent for root/tex, etc.)
  const char* count = nullptr;                            ///< trailing count pill (optional)
  CollapseVariant variant = CollapseVariant::Card;
  CollapseDensity density = CollapseDensity::Default;
  bool* open = nullptr;                    ///< persisted open/closed state (toggled on header click)
  bool* enable = nullptr;        ///< optional Unity-style enable switch on the right; off dims the body
  const CollapseAction* actions = nullptr; ///< trailing hover-revealed actions (right-aligned)
  int actionCount = 0;
};

/// What happened on a collapse header this frame.
struct CollapseResult
{
  bool open = false;          ///< whether to draw the body (guard content with `if (r.open)`)
  int clickedAction = -1;     ///< index into desc.actions clicked this frame, else -1
  bool enableChanged = false; ///< the enable toggle flipped this frame
};

/// Begin a collapsible panel (mirrors <g3d-collapse>). ALWAYS pair with EndCollapse(), open or not.
/// Paints the header and, when open, opens the padded body region for the caller's content. Nest
/// only Sub/Ghost panels inside another panel's body (Card/Overline own a wrapping border that is
/// not re-entrant). Composes inside BeginAccordion() (then renders as a flush list item).
CollapseResult BeginCollapse(const char* id, const CollapseDesc& desc);
void EndCollapse();

/// Continuous accordion list (mirrors <g3d-accordion>): wraps a run of BeginCollapse panels in one
/// seamless surface-1 card with a hairline between items. ALWAYS pair with EndAccordion().
/// @p exclusive keeps at most one panel open — opening one closes the others.
void BeginAccordion(const char* id, bool exclusive = false);
void EndAccordion();

//----------------------------------------------------------------------------
// Color picker
//
// The styleguide color picker (doc/dev/ui-styleguide.html: <g3d-colorswatch> / <g3d-colorpicker>) —
// a professional, HDR-aware color field. An inline checkerboard *swatch* trigger opens a popup
// *picker* panel: an SV square + hue / alpha bars + an eyedropper + a live preview, plus the pro
// capabilities of a 3D / scientific-visualization editor — an HDR intensity multiplier, sRGB<->linear
// space, 0–255 / 0–1 float ranges, a format cycle (HEX / RGB / HSB / HSL) with per-channel editable
// inputs (arrow-key nudge, Shift x10), 8-digit #RRGGBBAA, copy, and preset / recent swatches.
// Mirrors ImGui ColorEdit4 / ColorPicker4 (+ an intensity field for HDR colors). The picker owns all
// color math once; the popup is a real ImGui overlay, so it is never clipped by the host panel.
//----------------------------------------------------------------------------

/// Numeric presentation of the picker's editable channels — mirrors the styleguide format cycle.
enum class ColorFormat
{
  Hex, ///< single #RRGGBB / #RRGGBBAA field
  Rgb, ///< R / G / B (/ A) channels, honoring space + float range
  Hsb, ///< H / S / B (== HSV) channels
  Hsl, ///< H / S / L channels
};

/// Color space for the RGB / float readouts — mirrors the styleguide sRGB|linear segment (PBR and
/// scientific-visualization workflows pick colors in the linear domain).
enum class ColorSpace
{
  Srgb,
  Linear,
};

/// Trigger-swatch presentation (the closed color field). Mirrors <g3d-colorswatch>.
struct ColorSwatchDesc
{
  bool compact = false;   ///< 26px ultra-compact field (styleguide v="compact")
  bool noChevron = false; ///< hide the trailing chevron (nochev)
  bool noLabel = false;   ///< chip only, no hex value (nolabel)
  bool grow = true;       ///< fill the value column like a sibling select / slider (proprow grow)
  bool alpha = false;     ///< the value carries alpha (8-digit hex shown when < 1)
  bool disabled = false;
};

/// Inline color trigger: a checkerboard-backed color chip + hex value (+ chevron), sized like an
/// input. @p col is RGBA in 0..1 (col[3] honored only when desc.alpha). Returns true when clicked.
/// Use when you drive the popup yourself; ColorEdit() wires it to the picker for you.
bool ColorSwatch(
  const char* id, const float col[4], const ColorSwatchDesc& desc = ColorSwatchDesc());

/// Options for an inline color field (trigger swatch + popup picker). Mirrors <g3d-colorpicker>.
struct ColorEditDesc
{
  bool alpha = false;                    ///< edit / show the alpha channel
  bool hdr = false;                      ///< show the HDR intensity row (multiplier > 1)
  bool presets = true;                   ///< show the preset + recent swatch rows
  ColorFormat format = ColorFormat::Hex; ///< initial numeric format
  ColorSpace space = ColorSpace::Srgb;   ///< initial color space
  bool floatMode = false;                ///< initial RGB range: 0–1 float vs 0–255 integer
  // trigger presentation (forwarded to ColorSwatchDesc):
  bool compact = false;
  bool noChevron = false;
  bool noLabel = false;
  bool grow = true;
};

/// Inline color field: a swatch trigger that opens a popup color picker (SV area + hue / alpha + HDR
/// intensity + format / space / float controls + per-channel inputs + presets / recent + eyedropper +
/// copy). @p col is RGBA in 0..1 (col[3] used only when desc.alpha). Returns true on the frames the
/// color changed. Mirrors ImGui ColorEdit4 / ColorPicker4 and the styleguide color picker.
bool ColorEdit(const char* id, float col[4], const ColorEditDesc& desc = ColorEditDesc());

//----------------------------------------------------------------------------
// Eyedropper service (desktop == viewport pixel sampling)
//
// The styleguide eyedropper is the browser EyeDropper on the web; on desktop it maps to picking a
// pixel from the rendered viewport. This widget library is pure ImGui and cannot read GL itself, so
// the render integration feeds it: while EyedropperActive() the overlay render pass submits the
// scene texture (exactly the composited central viewport the user sees) once per frame through
// SubmitEyedropperFrame, and the sampling overlay reads that buffer under the cursor.
//----------------------------------------------------------------------------

/// Whether a color picker is in eyedropper sampling mode this frame (poll before reading pixels
/// back). Safe to call without an ImGui context (returns false).
bool EyedropperActive();

/// Provide the current viewport pixels for eyedropper sampling. @p rgba is tightly packed RGBA8 in
/// GL layout (row 0 = bottom row), sized w*h*4. @p rectX / @p rectY locate the viewport rect origin
/// in window device pixels (GL bottom-left origin); @p winW / @p winH are the full window device
/// size. Ignored when no eyedropper is active.
void SubmitEyedropperFrame(
  std::vector<unsigned char>&& rgba, int w, int h, int rectX, int rectY, int winW, int winH);

/// Observation-log sink (this widget library is integration-agnostic and cannot log itself). The
/// render integration injects a sink that routes to the session log; nullptr (default) disables.
/// Currently feeds the color-picker drag/sync trace ("[Trace][cp.*]" lines).
void SetTraceSink(void (*sink)(const char*));

} // namespace G3DWidgets

#endif
