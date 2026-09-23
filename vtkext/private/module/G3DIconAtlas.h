/**
 * @file G3DIconAtlas.h
 * @brief Baked-glyph cache for the Glance3D icon set.
 *
 * Icons used to be stroked into the frame's ImDrawList on every frame, which pinned their quality
 * to ImGui's 1px geometric AA fringe: fat, soft, not quite symmetric at 12-20px. This bakes each
 * (icon, size, stroke) once -- rendered supersampled and resolved by G3DIconRaster -- into a
 * rectangle of ImGui's own font atlas, so drawing an icon becomes a single tinted quad.
 *
 * Same move the eyedropper's loupe made when GDI turned out to have no anti-aliasing at all
 * (G3DLoupeRaster.h): rasterize it properly once, then just blit.
 *
 * Callers go through G3DIcon::Draw, which falls back to stroking whenever this declines.
 */

#ifndef G3DIconAtlas_h
#define G3DIconAtlas_h

#include "G3DIcon.h"

#include <imgui.h>

namespace G3DIconAtlas
{
/**
 * Draw @p id as one quad out of the baked cache, baking it on first use.
 *
 * @p topLeft and @p size must already be pixel-snapped and @p thickness already quantized (they
 * are the cache key). Returns false when no baked glyph is available -- baking disabled, size out
 * of range, atlas full -- and the caller must stroke the vector path instead.
 */
bool Blit(ImDrawList* drawList, G3DIconId id, const ImVec2& topLeft, float size, float thickness,
  ImU32 color);

/**
 * Drop every cached glyph.
 *
 * Mandatory whenever the ImGui context or its font atlas goes away: the cache holds
 * ImFontAtlasRectIds, which only mean anything to the atlas that issued them, and a fresh atlas
 * hands out the same ids again. Comparing atlas pointers is NOT a safe substitute — a new
 * ImFontAtlas can land on the address the old one had.
 */
void Invalidate();
}

#endif
