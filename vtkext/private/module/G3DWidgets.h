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

struct ImFont;

namespace G3DWidgets
{

/// Register the DATA font (monospace — numeric values, filenames, array names, timecodes, key
/// chips). The host loads it next to the UI font and injects it here; widgets push it for data
/// cells at the ambient size. Null (or never calling this) falls back to the ambient font.
void SetDataFont(ImFont* font);
/// The registered data font, or null when none.
ImFont* DataFont();

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

/// How an IconButton renders its persistent "on" state.
enum class IconOnStyle
{
  Fill,  ///< accent-soft filled chip + accent icon (structural toggles, e.g. panel visibility)
  Dot,   ///< ghost background, accent icon + small underline dot (lightweight display toggles)
  Well,  ///< persistent recessed key + hairline rim in BOTH on/off states, so an OFF toggle still
         ///< reads as a switch (not a momentary action button); accent wash + accent icon + dot on
  Solid, ///< solid accent fill + white icon regardless of @p on — the ONE primary action of a
         ///< bar (e.g. transport play); everything around it stays ghost-quiet
};

/// Square icon-only button (toolbar / FAB style). @p size <= 0 uses the icon-button token; @p round
/// makes it a pill/circle. @p on renders the persistent active state for toggle-style toolbar
/// buttons — mirrors styleguide .iconbtn.on; @p onStyle picks the emphasis (filled chip vs accent
/// icon + underline dot). @p shortcut, when non-null, appends a dimmed keycap to the tooltip (the
/// keyboard accelerator) — turns the icon-only bar into an on-ramp for the keyboard-first workflow.
/// Returns true on click.
bool IconButton(const char* id, G3DIconId icon, float size = -1.f, bool round = false,
  const char* tooltip = nullptr, bool on = false, IconOnStyle onStyle = IconOnStyle::Fill,
  const char* shortcut = nullptr);

/// One segment of a SegmentedIcon group.
struct SegmentedIconItem
{
  G3DIconId icon = G3DIconId::Cube;
  const char* tooltip = nullptr;
  bool on = false;       ///< persistent active state (accent-soft fill + accent icon)
  bool disabled = false; ///< not clickable, dimmed (tooltip still shows on hover)
};

/// A group of related icon toggles in one shared container (styleguide .segmented): one surface +
/// hairline shell, 1px separators between segments, per-segment accent-soft active fill. The quiet
/// grouping for panel-visibility style toggles — reads as one control instead of a row of chips.
/// Returns the index of the segment clicked this frame, else -1.
int SegmentedIcon(const char* id, const SegmentedIconItem* items, int count);

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
/// Closable variant: adds a ghost close button at the right end of the header band (the band keeps
/// its height — the button nests inside it). Returns true when the close button is clicked.
bool PanelHeader(const char* title, G3DIconId icon, bool closable);

//----------------------------------------------------------------------------
// Scroll regions — the expanding scrollbar affordance
//
// Desktop convention (macOS overlay scrollbars, VS Code, browsers): the thumb rests as a hairline
// so it never competes with content, and widens under the pointer so it is comfortable to grab.
// The gutter it lives in is a constant (G3DTheme::Scrollbar::Gutter) — ImGui carves the gutter out
// of the content region, so animating *it* would re-wrap text on mouse-over. Only the thumb moves.
//
// RULE — A SCROLL REGION IS FULL-BLEED. It spans its container edge to edge, so the gutter rides
// the panel edge with no dead strip beside it (and stays reachable by throwing the pointer at that
// edge). Horizontal padding belongs INSIDE the region, not around it: BeginScrollRegion takes the
// container's padding over and re-applies it as the region's own content inset, so content does not
// move — only the scrollbar does. Never wrap a scroll region in a padded container and call it
// done; pass ScrollBleed::Inline only for a list genuinely embedded among other content.
//
// The transition needs NO opt-in: InstallScrollbarStyle() hooks it into ImGui once (io's
// ScrollbarStyleFn) and from then on EVERY scrollbar animates — including the ones ImGui opens for
// itself (combo popups, list boxes, tables) that no call site of ours could reach. Scroll
// containers only ever call:
//
//   if (G3DWidgets::BeginScrollRegion("##my.region")) { ...content... }
//   G3DWidgets::EndScrollRegion();                      // like EndChild: call it either way
//
// ...and that pair exists for the full-bleed rule above, not for the scrollbar itself.
//----------------------------------------------------------------------------

/// Hook the expanding-scrollbar transition into ImGui. Call once, after the context and style
/// exist; every scrollbar drawn afterwards animates, and nothing else has to know about it.
void InstallScrollbarStyle();

/// Where a scroll region sits relative to its container's padding.
enum class ScrollBleed
{
  Container, ///< default: edge to edge, padding moved inside — the gutter rides the panel edge
  Inline,    ///< stay within the container's padding: a list embedded among other content
};

/// ImGui::BeginChild with the full-bleed treatment (see the RULE above): the region takes the
/// container's horizontal padding over so its gutter lands on the panel edge, and re-applies that
/// padding inside itself so content does not move. Bleeding applies when @p size.x is not an
/// explicit width. Returns BeginChild's visibility (skip content when false); EndScrollRegion()
/// must be called either way, exactly like ImGui::EndChild().
bool BeginScrollRegion(const char* id, const ImVec2& size = ImVec2(0.f, 0.f),
  ImGuiWindowFlags flags = 0, ScrollBleed bleed = ScrollBleed::Container);
void EndScrollRegion();

//----------------------------------------------------------------------------
// Floating card — the reusable chrome for every draggable overlay panel
//
// One component, three calls:
//
//   G3DWidgets::FloatingCardDesc d;  d.id = "MyCard"; d.title = ...; d.size = ...;
//   const auto card = G3DWidgets::BeginFloatingCard(state, d);   // title bar: grip + drag + close
//   ...pinned content (search field, toolbar) — stays put while the body scrolls...
//   G3DWidgets::BeginFloatingCardBody();
//   ...scrolling content...
//   G3DWidgets::EndFloatingCardBody();
//   G3DWidgets::EndFloatingCard();
//   if (card.closed) { ...hide the card... }
//
// The component owns: window setup (position/size/flags/rounding/rim/elevation shadow), the title
// bar anatomy (grip affordance, icon, title, close button), drag-to-move with clamping, and the
// pinned-header / scrolling-body split. It deliberately knows nothing about the app layout: the
// default anchor and the clamp bounds are injected per frame, the state only carries what the user
// did. Reuse it for any new floating panel instead of hand-rolling a Begin() + drag handle.
//----------------------------------------------------------------------------

/// Session state of one floating card (owned by the caller, one instance per card).
struct FloatingCardState
{
  ImVec2 dragOffset = ImVec2(0.f, 0.f); ///< user drag, nominal px (divided by the UI scale)
  bool dragging = false;                ///< the drag handle is held this frame
  bool moved = false;                   ///< the user has dragged this card at least once
};

/// Per-frame description of a floating card. Only `id`, `title` and `size` are mandatory; the rest
/// have sane defaults (centered-anchor callers still pass `defaultPos` / `bounds`).
struct FloatingCardDesc
{
  const char* id = "##g3d.card";              ///< window id, unique + stable per card
  const char* title = "";                     ///< title-bar label
  G3DIconId icon = G3DIconId::Info;           ///< title-bar identity glyph
  bool closable = true;                       ///< show the title-bar close button
  const char* dragTooltip = nullptr;          ///< hover hint on the title bar
  ImVec2 size = ImVec2(0.f, 0.f);             ///< card size in px (caller sizes it from content)
  ImVec2 defaultPos = ImVec2(0.f, 0.f);       ///< anchor used until the user drags
  ImVec4 bounds = ImVec4(0.f, 0.f, 0.f, 0.f); ///< clamp rect: x,y = origin, z,w = size
  float margin = 8.f;                         ///< gap kept between the card and the bounds
  float padding = -1.f;                       ///< content padding (<= 0: theme default)
  const ImVec4* background = nullptr;         ///< window fill override (null: ImGui WindowBg)
  ImGuiWindowFlags extraFlags = 0;            ///< extra window flags OR-ed in
};

/// What the card reported this frame.
struct FloatingCardResult
{
  bool closed = false;   ///< the title-bar close button was clicked
  bool dragging = false; ///< the title bar is held — OR this into a force-render condition so the
                         ///< drag stays frame-continuous
};

/// Open a floating card. Submits the window (positioned from the anchor + the user's drag, clamped
/// into `bounds`) and its title bar, then leaves the cursor below the title bar ready for content.
/// Always pair with EndFloatingCard().
FloatingCardResult BeginFloatingCard(FloatingCardState& st, const FloatingCardDesc& desc);
void EndFloatingCard();

/// Height of a floating card's title bar (px, already UI-scaled) — for callers sizing their card
/// from their content height.
float FloatingCardHeaderHeight();

/// Scrolling region filling the card's remaining height: everything submitted between
/// BeginFloatingCard() and this call is PINNED (title bar, search field, tabs), everything inside
/// scrolls. The outer card window never scrolls, so the title can not be pushed out of view.
/// @p horizontalScroll adds a horizontal scrollbar for content wider than the card.
bool BeginFloatingCardBody(const char* id = "##g3d.card.body", bool horizontalScroll = false);
void EndFloatingCardBody();

/// Resolve a floating card's window position: default anchor + drag offset, clamped into @p bounds
/// (x,y = origin, z,w = size) with @p margin breathing room. The clamped result is written back so
/// the stored offset never exceeds what is shown — a window shrink would otherwise leave a dead
/// zone before reverse dragging takes visible effect. BeginFloatingCard() calls this itself; it is
/// exposed for callers that need the resolved rect (hit-testing, non-window cards) before drawing.
ImVec2 FloatingCardPos(
  FloatingCardState& st, ImVec2 defaultPos, ImVec2 size, const ImVec4& bounds, float margin);

/// Read-only key/value row for inspectors / stat panels: muted label on the left, primary value
/// right-aligned on the same line (mirrors styleguide .proprow used read-only).
void StatRow(const char* key, const char* value);

/// Editable property row (mirrors styleguide .proprow inside collapse bodies): a fixed-width muted
/// label column on the left, then the value column where the caller draws exactly one control
/// (Toggle with an empty label, SliderFloat/BeginSelect with a "##v" label, ColorEdit with grow).
/// BeginPropRow positions the cursor at the value column and pre-sets the next item width to fill
/// it; EndPropRow draws the label vertically centered on the resulting row, normalizes the row to
/// at least the control-height rhythm, and returns the cursor to the row's left edge.
/// @p labelW <= 0 uses the styleguide collapse-body label column (88). Pass the control's height as
/// @p ctrlH (e.g. G3DTheme::Size::Icon for Toggle) to vertically center controls shorter than the
/// standard control row; <= 0 assumes standard control height (no centering offset).
void BeginPropRow(const char* label, float labelW = -1.f, float ctrlH = -1.f);
void EndPropRow();

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
/// @p emphasizeValue draws the readout in full-strength text instead of muted — for sliders whose
/// value IS the primary information (e.g. the timeline's current time), not a secondary detail.
/// @p tickUnit > 0 draws faint tick marks under the track at automatic multiples of that domain
/// unit (the timeline passes 1 == one second; the step ladder keeps ticks ≥ ~40px apart). Only the
/// slider knows its track span (the readout column eats part of the item width), so ticks must be
/// drawn here, not by the caller. 0 = no ticks.
bool SliderFloat(const char* label, float* v, float vMin, float vMax, const char* format = "%.2f",
  bool emphasizeValue = false, float tickUnit = 0.f);

/// Styled dual-handle interval slider: one track, two grabs, the span between them filled with the
/// accent (the standard "range" control — e.g. scalar coloring min/max). Dragging a handle moves the
/// nearer bound; handles may meet but never cross (the pair is re-ordered live). The readout on the
/// right shows "lo~hi" (ASCII separator — the atlas has no en dash) with @p format applied to each
/// bound. Returns true when either value changed.
bool RangeSliderFloat(const char* label, float* lo, float* hi, float vMin, float vMax,
  const char* format = "%.2f");

/// Styled single-line text input (focus underline + border highlight). Returns true when edited.
bool InputText(const char* label, char* buf, std::size_t bufSize, const char* hint = nullptr);

//----------------------------------------------------------------------------
// Tooltips
//
// Always open tooltips through these helpers rather than ImGui::SetTooltip / ImGui::BeginTooltip:
// an ImGui tooltip is a plain window, so it inherits whatever WindowPadding is pushed around the
// *trigger* — inside the full-bleed inspector bar (WindowPadding.x = 0) that left the text glued to
// the bubble edge. Each helper pins the house padding (G3DTheme::Tooltip) for the duration of the
// tooltip window, and takes an optional per-call override.
//----------------------------------------------------------------------------

/// Padding sentinel meaning "use the theme default" (G3DTheme::Tooltip::PadX / PadY). Resolved per
/// axis, so a caller may override only x or only y and leave the other on the default.
constexpr ImVec2 TooltipThemePadding = ImVec2(-1.f, -1.f);

/// Hover flags every G3D tooltip triggers on — the unified delay, not shared with neighbours (each
/// item earns its own dwell, so sweeping the pointer across a toolbar does not flash tooltips).
constexpr ImGuiHoveredFlags TooltipHoveredFlags =
  ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay;

/// Open a tooltip window with the house padding; submit any widgets, then call EndTooltip().
/// @p padding is nominal px (scaled here); negative components fall back to the theme default.
/// Always returns true — the bool mirrors ImGui::BeginTooltip() so call sites read the same.
bool BeginTooltip(const ImVec2& padding = TooltipThemePadding);

/// BeginTooltip() gated on the last item being hovered on the house delay (TooltipHoveredFlags).
/// @p extraHoveredFlags is OR-ed in — e.g. ImGuiHoveredFlags_AllowWhenDisabled to explain why a
/// grayed control is inert. Call EndTooltip() only when this returned true.
bool BeginItemTooltip(
  const ImVec2& padding = TooltipThemePadding, ImGuiHoveredFlags extraHoveredFlags = 0);

/// Close a tooltip opened by BeginTooltip() / BeginItemTooltip().
void EndTooltip();

/// Padded drop-in for ImGui::SetTooltip("%s", text): opens the tooltip unconditionally, for callers
/// that own the hover test (custom hit boxes, disabled items, flash states).
void SetTooltip(const char* text, const ImVec2& padding = TooltipThemePadding);

/// Tooltip for the last item, with a unified hover delay.
void ItemTooltip(const char* text, const ImVec2& padding = TooltipThemePadding);

/// Draw @p text at @p pos, truncated with a trailing "..." when wider than @p maxW (UTF-8 safe —
/// never splits a multi-byte glyph). Pure draw helper: does not advance the layout cursor.
///
/// @p dropWhenUnreadable governs the degenerate end of the budget, where not even one glyph plus
/// the "..." fits: off (the default) the bare "..." is still drawn, on nothing is drawn at all.
/// Opt in for a cell whose value is the ellipsis-free string or nothing — a deeply indented tree
/// name, where a stranded ".." says less than blank space does. Leave it off wherever the mere
/// presence of text is information (a control's current value must not read as empty).
///
/// @return true when the text had to be truncated — INCLUDING when it was dropped entirely, so a
/// caller keyed on this still reveals the full string on hover.
bool TextEllipsis(ImDrawList* dl, const ImVec2& pos, float maxW, ImU32 col, const char* text,
  bool dropWhenUnreadable = false);

/// Draw @p text at an explicit pixel size — the design system's 11px overline / badge sizes, below
/// the base UI font (ImGui scales the glyphs to it). @p mono uses the DATA font. Pure draw helper:
/// does not advance the layout cursor.
void TextSized(ImDrawList* dl, const ImVec2& pos, ImU32 col, const char* text, float px,
  bool mono = false);
/// Size TextSized() would occupy — for fit tests and right-alignment before drawing.
ImVec2 CalcTextSizedPx(const char* text, float px, bool mono = false);

//----------------------------------------------------------------------------
// Select / dropdown
//
// The styleguide dropdown (doc/dev/ui-styleguide.html: <g3d-select> / .dropdown + .menu) — an
// input-look trigger showing the current value with a rotating chevron, opening a floating menu of
// check-marked items. The menu is a real ImGui popup (an overlay window), so like the styleguide's
// body-portaled .menu it escapes any clipping ancestor (accordion, inspector scroll) and closes on
// an outside click / Esc. Left-aligned under the trigger, trigger-width, flips above when there is
// no room below; long lists scroll. Usage mirrors ImGui::BeginCombo/EndCombo so call sites migrate
// mechanically:
//
//   // Trigger width = CalcItemWidth(). Inside a BeginCollapse body it already fills the padded
//   // row (the container's default) -- NEVER force it back to the window edge with
//   // SetNextItemWidth(-1)/PushItemWidth(-1); only set a width to deviate (e.g. a fixed 120px).
//   if (G3DWidgets::BeginSelect("##id", preview))     // true while the menu is open
//   {
//     for (const auto& opt : options)
//     {
//       if (G3DWidgets::SelectItem(opt.label, opt.isCurrent))
//       {
//         // apply opt (the menu closes itself)
//       }
//     }
//     G3DWidgets::EndSelect();                        // ONLY when BeginSelect() returned true
//   }
//----------------------------------------------------------------------------

/// Dropdown trigger + menu begin (mirrors styleguide <g3d-select>). @p preview is the value shown
/// in the trigger; when it is empty, the optional @p hint shows as a subtle placeholder instead.
/// Trigger width follows ImGui::CalcItemWidth() (SetNextItemWidth / PushItemWidth). Returns true
/// while the menu is open — then emit SelectItem()s and close with EndSelect().
bool BeginSelect(const char* id, const char* preview, const char* hint = nullptr);

/// One menu entry (mirrors styleguide .menu-item): hover-tinted row, accent text + trailing check
/// when @p selected. A click applies and closes the menu. Returns true on the click frame.
bool SelectItem(const char* label, bool selected = false);

/// Close the menu opened by a true-returning BeginSelect(). Call exactly then, like EndCombo.
void EndSelect();

/// Flat colormap control points: (t, r, g, b) quadruples, t ascending (the layout
/// G3DParseColormapTokens-style parsers produce). count is the number of doubles; anything that is
/// not a non-empty multiple of 4 renders as a neutral placeholder strip.
struct GradientStops
{
  const double* data = nullptr;
  int count = 0;
};

/// Multi-stop gradient bar between @p p0 / @p p1: piecewise AddRectFilledMultiColor segments with
/// the dark inset ring color chips use (square corners, like the picker's SV/hue bars). @p alpha
/// multiplies every color (menu fade-in bypasses style.Alpha for custom draws). @p vertical maps
/// t = max to the top edge (scalar-bar orientation) instead of left-to-right. @p muted desaturates
/// and darkens at full alpha — for a ramp whose feature is present but not currently active (the
/// strip must go quiet without looking disabled or translucent).
void DrawGradientStrip(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1,
  const GradientStops& stops, float alpha = 1.f, bool vertical = false, bool muted = false);

/// BeginSelect variant whose trigger shows a small gradient strip of the current colormap before
/// the value text. Same open/close contract as BeginSelect (close with EndSelect()). @p muted
/// quiets the trigger strip (see DrawGradientStrip) — menu preset rows always stay full-color.
bool BeginSelectColormap(const char* id, const char* preview, const GradientStops& stops,
  const char* hint = nullptr, bool muted = false);

/// SelectItem variant with a leading gradient strip (colormap preset rows).
bool SelectItemColormap(const char* label, const GradientStops& stops, bool selected = false);

/// Colormap legend: a full-width ramp with the mapped range's ends labelled beneath it (11px mono,
/// subtle). This is the "what do these colors mean" key for a list — a legend, not a control: it
/// takes no input and shows no state. One per section, never one per row. Pass nullptr for both
/// @p lo / @p hi to omit the labels (a degenerate range). Consumes its height in the layout flow.
void ColormapLegend(const GradientStops& stops, const char* lo, const char* hi);

//----------------------------------------------------------------------------
// Context menu
//
// A right-click menu sharing the exact styleguide .menu chrome as the <g3d-select> dropdown (same
// surface-3 popup, r-popup rounding, shadow + AA border, hover-tinted rows) — but with no trigger
// and action rows instead of a checked value list. Open it on the PREVIOUS item's right-click; the
// popup auto-positions at the cursor and closes on outside-click / Esc / item-click. Usage:
//
//   ImGui::InvisibleButton(...);                 // the item the menu attaches to
//   if (G3DWidgets::BeginContextMenu("##id"))     // true while the menu is open
//   {
//     if (G3DWidgets::MenuAction("Copy path")) { /* do it (the menu closes itself) */ }
//     G3DWidgets::EndContextMenu();               // ONLY when BeginContextMenu() returned true
//   }
//----------------------------------------------------------------------------

/// Right-click context menu begin: opens on the previous item's right-click, styled like the
/// dropdown .menu. Returns true while open — then emit MenuAction()s and close with EndContextMenu().
bool BeginContextMenu(const char* id);

/// One context-menu action row (mirrors .menu-item, no check): hover-tinted, plain text; a click
/// applies and closes the menu. Returns true on the click frame.
bool MenuAction(const char* label);

/// Close the menu opened by a true-returning BeginContextMenu(). Call exactly then.
void EndContextMenu();

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

/// Row height for @p density in scaled px (what BeginTree() pushes). @p scale <= 0 uses the live UI
/// scale. For callers that need the height before drawing — an explicit TreeRowChrome::height, or
/// reserving exactly one row for an empty state so a list does not jump when it empties.
float TreeRowHeight(TreeDensity density, float scale = 0.f);

/// Structural description of a row — everything the headless row owns (no cell content).
struct TreeRowChrome
{
  int depth = 0;                        ///< indentation level (number of rails drawn)
  TreeTwisty twisty = TreeTwisty::Leaf; ///< none / open / collapsed
  bool selected = false;                ///< full-row selection background + left accent bar
  bool focused = false;                 ///< selected and focused (deeper background)
  bool disabled = false;                ///< not interactive, dimmed
  int activeGuide = -1;                 ///< indent rail column to highlight (selection guide), -1 none

  /// This row is an ancestor of whatever the tree is currently focused on (selected or hovered).
  ///
  /// Drawn as a dimmed copy of the selection's edge bar. Exists because `activeGuide` cannot carry
  /// this once the indent column saturates: past the ceiling every column shares one x, so
  /// highlighting "the selection's parent column" highlights a line that a dozen other depths also
  /// sit on. Marking the ancestor ROWS says the same thing in a channel that does not run out of
  /// horizontal room, which is the only channel left at depth 20 in a 240px bar.
  bool ancestorOfFocus = false;

  /// Explicit row height in scaled px. 0 = the active BeginTree() density (22px outside any
  /// BeginTree scope) — see TreeRowHeight(). Rows of a non-uniform height are NOT usable under
  /// TreeVirtual(): its clipper positions every row at the density height.
  float height = 0.f;

  /// Content-rail mode: the row's content rails ARE the container's. No twisty/indent column on the
  /// left and no inset on the right, so the label starts and the trailing cells end exactly on the
  /// caller's content edges — a list row then shares one rail with the StatRow / BeginPropRow labels
  /// stacked above it. For selectable list rows inside a padded panel body (the inspector's array
  /// list); the outliner keeps the default. `twisty` must stay Leaf: there is no column to draw in.
  bool contentRail = false;

  /// Paint AND hit the row band this many scaled px wider on each side than the content rails, so the
  /// band edges can land on the container's own chrome edges (a Flat section body pads by 12 while
  /// its header band insets by 8 — bleed 4 makes the two flush). The band, the hit rect and the
  /// content extent stay one rectangle, which is the whole point. 0 = band == row box (the outliner).
  float bleed = 0.f;
};

/// What the user clicked on a row this frame.
struct TreeRowResult
{
  bool rowClicked = false;    ///< the row body was clicked (use for selection)
  bool twistyClicked = false; ///< the expand/collapse twisty was toggled
  bool hovered = false;       ///< the row is hovered (drives trailing-action reveal, etc.)
  /// The row body was double-clicked. Reported on the second PRESS (ImGui's own double-click
  /// timing), and never for the twisty column -- opening a node twice is not a gesture, and a
  /// caller that acts on both would fire its double-click action every time someone toggled fast.
  bool rowDoubleClicked = false;
};

/// Begin a row: paints the chrome, hit-tests the twisty and the row body, places the ImGui cursor at
/// the content start, and leaves the row's hit item as the current ImGui item (so the caller may
/// call ImGui::BeginDragDropSource()/Target() before EndTreeRow()). @p id must be unique per row.
TreeRowResult BeginTreeRow(const char* id, const TreeRowChrome& chrome);
void EndTreeRow();

/// Node type icon at the content cursor. @p dim fades it (hidden node).
void TreeRowIcon(G3DIconId icon, TreeIconVariant variant = TreeIconVariant::Default, bool dim = false);
/// Node label at the content cursor. @p group brightens it; @p dim fades it (hidden node).
/// @return true when the name had to be ellipsized. The slot emits no tooltip of its own — a
/// headless row leaves that to its caller (TreeRow() reveals the full name; a list row that
/// composes one tooltip from several facts folds it in there instead).
bool TreeRowLabel(const char* text, bool group = false, bool dim = false);
/// Right-aligned metadata (e.g. child count). Place after the label. @p px draws at an explicit
/// pixel size (styleguide .tree-meta is 11px overline); 0 = the ambient font size.
/// Trailing value cell (a child count, an instance's product). Returns true when it had to be
/// ellipsized to keep the label readable, so a caller can reveal it on hover.
bool TreeRowMeta(const char* text, float px = 0.f);
/// Trailing icon action button (right-aligned, reveals on row hover). @p on tints it with the accent.
/// Returns true when clicked. @p id unique within the row.
bool TreeRowAction(const char* id, G3DIconId icon, bool on = false);

/// Which part of a convenience TreeRow() was clicked this frame.
enum class TreeRowHit
{
  None,
  Row,            ///< row body (select)
  RowDoubleClick, ///< row body, double-clicked (the trailing actions and the twisty are excluded)
  Twisty,         ///< expand/collapse
  Visibility,     ///< the eye action
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
  bool ancestorOfFocus = false; ///< see TreeRowChrome::ancestorOfFocus

  /// Extra lines for the hover tooltip, under the label. Set it and the tooltip appears whether or
  /// not the label had to be ellipsized.
  ///
  /// Supplied by the caller rather than composed here because what is worth saying about a row is
  /// domain knowledge (a scene node says how deep it sits and what it hangs under) and because the
  /// wording has to be translated, which the widget layer has no business deciding for it.
  const char* tooltipDetail = nullptr;
};

/// Draw the common row in one call (built on BeginTreeRow + slot helpers). Returns the click hit.
/// This is the outliner row: it deliberately does not expose TreeRowChrome::height / contentRail /
/// bleed — compose those with BeginTreeRow() + the slot helpers.
/// @p outHovered, when given, reports whether the pointer is over the row BODY this frame (the
/// trailing eye action is its own item and does not count). A caller that reacts to the hovered row
/// while drawing the rows themselves has to remember it across frames -- the row is not known to be
/// hovered until it has been drawn, by which point everything above it is already on screen.
TreeRowHit TreeRow(const char* id, const TreeRowDesc& desc, bool* outHovered = nullptr);

/// First indent column the active tree can no longer move to the right, or INT_MAX when its indent
/// column never caps (the default outside any BeginTree scope). A row deeper than this is drawn at
/// the same x as its parent, so a caller that wants depth to remain answerable has to say it some
/// other way -- a tooltip, a trailing cell -- for exactly those rows.
int TreeIndentSaturationLevel();

/// Virtualized tree body for large node counts: only the rows currently visible in the scroll region
/// are emitted (wraps ImGuiListClipper using the row height of the active BeginTree density), so a
/// 100k-node tree stays O(on-screen rows) per frame. Call inside a scrolling region, between
/// BeginTree/EndTree. @p drawRow(i) draws the i-th currently-visible row (0-based) with BeginTreeRow/
/// TreeRow — the caller maps i to its (already flattened, collapse-resolved) node. All rows must be
/// the uniform tree row height: do NOT set TreeRowChrome::height inside @p drawRow, the clipper
/// positions rows at the density height and a taller/shorter row desynchronizes every row below it.
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
  Flat,     ///< docked full-bleed section (Blender/UE5 category): no shell, subtle full-width
            ///< header band, hairline between sections — for panel bars, not floating windows
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
/// Paints the header and, when open, opens the padded body region for the caller's content. The
/// body is a real child window content box: its padding structurally narrows the content region,
/// so full-width items (the body default), GetContentRegionAvail-based layouts and right-aligned
/// content all stop at the padded edge -- callers never manage the card insets themselves. Nest
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
// Eyedropper service (desktop == screen-wide pixel sampling)
//
// The styleguide eyedropper is the browser EyeDropper on the web; on desktop it maps to picking a
// pixel from anywhere on screen. This widget library is pure ImGui and cannot read GL or the OS
// itself, so the render/platform integration feeds it while EyedropperActive():
// - SubmitEyedropperFrame: the scene texture (exactly the composited central viewport the user
//   sees) once per frame — the sample source inside the viewport rect;
// - SubmitEyedropperScreenPatch: a small live desktop capture around the cursor once per frame —
//   the sample source everywhere else (app UI chrome, outside the window, other monitors). The
//   integration also covers the screen with an invisible input overlay then, relaying cursor
//   moves and the picking click into this window while the cursor roams beyond the client area,
//   and shows an OS-level loupe following the cursor out there.
// Without a screen feed (non-Windows) sampling gracefully falls back to the viewport rect only.
//----------------------------------------------------------------------------

/// Whether a color picker is in eyedropper sampling mode this frame (poll before reading pixels
/// back). Safe to call without an ImGui context (returns false).
bool EyedropperActive();

/// Cancel eyedropper sampling — exit the mode and reopen the picker. Invoked by the platform input
/// overlay on a right-click (which it swallows, so the click never reaches ImGui). No-op when not
/// sampling; consumed on the next frame.
void CancelEyedropper();

/// Provide the current viewport pixels for eyedropper sampling. @p rgba is tightly packed RGBA8 in
/// GL layout (row 0 = bottom row), sized w*h*4. @p rectX / @p rectY locate the viewport rect origin
/// in window device pixels (GL bottom-left origin); @p winW / @p winH are the full window device
/// size. Ignored when no eyedropper is active.
void SubmitEyedropperFrame(
  std::vector<unsigned char>&& rgba, int w, int h, int rectX, int rectY, int winW, int winH);

/// Provide a live desktop capture around the cursor for eyedropper sampling beyond the viewport.
/// @p rgba is tightly packed RGBA8 in top-down rows (ImGui orientation), sized w*h*4;
/// @p originX / @p originY locate the patch's top-left corner in window device pixels (ImGui
/// top-left origin; may be negative or beyond the window — the patch follows the OS cursor).
/// Submit once per frame while EyedropperActive(): a patch is sampled for one UI frame only, a
/// stale one means the desktop feed stopped and sampling falls back to the viewport rect. Ignored
/// when no eyedropper is active.
void SubmitEyedropperScreenPatch(
  std::vector<unsigned char>&& rgba, int w, int h, int originX, int originY);

/// Observation-log sink (this widget library is integration-agnostic and cannot log itself). The
/// render integration injects a sink that routes to the session log; nullptr (default) disables.
/// Currently feeds the color-picker drag/sync trace ("[Trace][cp.*]" lines).
void SetTraceSink(void (*sink)(const char*));

/// printf-style write through the injected trace sink (no-op when unset) — lets the platform
/// integrations (e.g. the desktop screen sampler) share the widget library's observation channel.
void Trace(const char* fmt, ...);

} // namespace G3DWidgets

#endif
