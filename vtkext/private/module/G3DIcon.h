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
  Camera,
  Cube,
  Folder,     ///< closed folder (tree group)
  FolderOpen, ///< open folder (expanded tree group)
  Layers,     ///< stacked layers (scene collection / root)
  Light,      ///< light source (sun)
  Image,      ///< picture / texture / material
  Lock,       ///< padlock (locked node)
};

namespace G3DIcon
{
/**
 * Draw @p id centered at @p center, fitting a square of edge @p size (px), stroked in @p color.
 * @p thickness <= 0 selects a size-proportional stroke.
 */
void Draw(ImDrawList* drawList, G3DIconId id, const ImVec2& center, float size, ImU32 color,
  float thickness = -1.f);

/**
 * Lay an icon out inline like text: reserves a @p size × @p size item at the cursor and draws into
 * it (so it participates in ImGui layout / SameLine).
 */
void Inline(G3DIconId id, float size, ImU32 color);
}

#endif
