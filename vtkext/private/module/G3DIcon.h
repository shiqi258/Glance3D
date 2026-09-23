/**
 * @file G3DIcon.h
 * @brief Unified vector-icon display logic for the Glance3D ImGui UI.
 *
 * Icons are authored locally as small vector routines (no icon font / external asset): each id maps
 * to a function that strokes the glyph inside a normalized box. All icons go through one display path
 * (Draw / Inline) that handles sizing, centering, color and anti-aliasing, so every component shows
 * icons consistently and a new icon is just one enum value + one draw case (in G3DIcon.cxx).
 *
 * Color follows a "currentColor" convention: callers pass the color of the surrounding context
 * (text role, button foreground, ...), so an icon dims/tints with its container automatically.
 */

#ifndef G3DIcon_h
#define G3DIcon_h

#include <imgui.h>

/// Available icons. Add a value here and a matching case in G3DIcon.cxx to introduce a new icon.
enum class G3DIconId
{
  Sliders, ///< three horizontal sliders (control panel)
  Close,   ///< ✕
  ChevronRight,
  ChevronDown,
  ChevronLeft,
  ChevronUp,
  Plus,
  Search,
  Dots, ///< vertical ellipsis (overflow menu)
  Eye,
  EyeOff,
  Grid,
  Axis,
  Fit,  ///< four corner brackets (fit — e.g. reset a data range to its extent)
  Home, ///< house silhouette (reset / home view — the default camera framing)
  Camera,
  Cube,
  Component,  ///< four diamonds in a diamond (an instance: one product, many occurrences)
  Surface,    ///< a quad seen at an angle (one B-rep face of a solid)
  Skeleton,   ///< a three-bone chain (an armature root)
  Joint,      ///< one bone: two sockets and the shaft between them
  Folder,     ///< closed folder (tree group)
  FolderOpen, ///< open folder (expanded tree group)
  Layers,     ///< stacked layers (scene collection / root)
  Light,      ///< light source (sun)
  Image,      ///< picture / texture / material
  Lock,       ///< padlock (locked node)
  Info,       ///< circled "i" (data / details panel)
  Warning,    ///< triangle + exclamation (a message that needs attention)
  Error,      ///< circled cross (a message about something that failed)
  Success,    ///< circled check (a message confirming an action worked)
  Bell,       ///< notification bell (the message center)
  BellDot,    ///< bell + unread dot (the message center has something unread)
  ExternalLink, ///< box + out-arrow (opens something outside this window: a folder, a log, a page)
  Help,       ///< circled "?" (shortcuts / cheatsheet)
  Edges,      ///< triangle with vertex dots (mesh edges / wireframe)
  Play,       ///< filled right-pointing triangle (animation play)
  Pause,      ///< two filled bars (animation pause)
  StepForward, ///< triangle + bar (step one frame forward)
  SkipBack,    ///< bar + left-pointing triangle (step one frame back)
  SkipToStart, ///< bar + two left-pointing triangles (jump back to the animation start)
  SkipToEnd,   ///< two right-pointing triangles + bar (jump forward to the animation end)
  Repeat,      ///< two arrowed tracks forming a loop (animation loop toggle) — Lucide repeat
  Replay,      ///< circular arrow (restart a finished play-once clip)
  Check,       ///< checkmark (confirm / copied feedback / swatch selected)
  Copy,        ///< two overlapping rounded rects (copy to clipboard)
  UpDown,      ///< stacked up/down chevrons (cycle / spinner affordance)
  Eyedropper,  ///< pipette (screen / viewport color sampling)
  PanelLeft,   ///< frame + left divider (toggle the left dock bar) — Lucide panel-left
  PanelRight,  ///< frame + right divider (toggle the right dock bar) — Lucide panel-right
  PanelBottom, ///< frame + bottom divider (toggle the bottom dock bar) — Lucide panel-bottom
  PanelClose, ///< panel-right frame + inward chevron (collapse the panel chrome) — Lucide panel-right-close
  PanelOpen, ///< panel-right frame + outward chevron (bring the panel chrome back) — Lucide panel-right-open
};

namespace G3DIcon
{
/**
 * Draw @p id centered at @p center, fitting a square of edge @p size (px), tinted @p color.
 * @p thickness <= 0 selects a size-proportional stroke.
 *
 * This is the entry point for all UI code. It snaps the glyph to the pixel grid, then blits it
 * from the baked cache (G3DIconAtlas) and only strokes it live if no baked glyph is available.
 */
void Draw(ImDrawList* drawList, G3DIconId id, const ImVec2& center, float size, ImU32 color,
  float thickness = -1.f);

/**
 * Stroke @p id into the box [@p topLeft, @p topLeft + @p size] with an exact @p thickness and no
 * pixel snapping.
 *
 * The glyph table itself — every icon's geometry is written once, here. Two callers: Draw()'s
 * fallback, and the baker, which runs it supersampled to produce the cached bitmap. UI code should
 * not call this; use Draw().
 */
void DrawUnsnapped(ImDrawList* drawList, G3DIconId id, const ImVec2& topLeft, float size,
  ImU32 color, float thickness);
}

#endif
