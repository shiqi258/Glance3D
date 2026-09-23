/**
 * @file G3DIconRaster.h
 * @brief Software rasterizer that turns an ImGui triangle list into a coverage bitmap.
 *
 * ImGui anti-aliases by extruding a 1px feathered fringe around each stroke rather than by
 * computing per-pixel coverage. At icon sizes (12-20px) that fringe is a large fraction of the
 * stroke, so glyphs drawn straight into an ImDrawList come out fat and soft next to the same
 * geometry rendered by a browser. The fix is to draw the glyph once at a multiple of its final
 * size, resolve it here, and box-downsample: at 4x the fringe is a quarter of a final pixel and
 * what is left is effectively coverage.
 *
 * This reproduces the GPU's own model rather than inventing a new one -- a top-left fill rule so a
 * shared edge is shaded exactly once, per-vertex alpha interpolated barycentrically, and
 * "source over" accumulation, which is what ImGui's blend state does. So the baked glyph is the
 * same drawing the vector path produces, only sampled densely.
 *
 * Pure C++ over ImDrawVert: no VTK, no GL, no ImGui context, so it is unit-testable off-screen.
 * The sibling of G3DLoupeRaster.h, which does the same job analytically for the eyedropper loupe.
 */

#ifndef G3DIconRaster_h
#define G3DIconRaster_h

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace G3DIconRaster
{
/// Twice the signed area of (a, b, c); positive for the winding ImGui emits in y-down space.
inline float EdgeFunction(const ImVec2& a, const ImVec2& b, float px, float py)
{
  return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

/**
 * Top-left rule for an edge a->b of a positive-area triangle in y-down space.
 *
 * A sample sitting exactly on an edge belongs to the triangle on one side only; without this two
 * triangles sharing that edge would both composite into the pixel and leave a bright seam.
 */
inline bool IsTopLeftEdge(const ImVec2& a, const ImVec2& b)
{
  return (a.y == b.y) ? (b.x > a.x) : (b.y < a.y);
}

/**
 * Composite one indexed triangle list into @p cov, a @p w x @p h buffer of 0..1 coverage.
 *
 * Only the vertex alpha is read: the baker strokes in opaque white, so colour carries no
 * information and the caller tints the finished quad instead. @p cov is not cleared.
 */
inline void CompositeTriangles(
  float* cov, int w, int h, const ImDrawVert* vtx, const ImDrawIdx* idx, int idxCount)
{
  if (!cov || !vtx || !idx || w <= 0 || h <= 0)
  {
    return;
  }

  for (int t = 0; t + 2 < idxCount; t += 3)
  {
    ImVec2 p0 = vtx[idx[t + 0]].pos;
    ImVec2 p1 = vtx[idx[t + 1]].pos;
    ImVec2 p2 = vtx[idx[t + 2]].pos;
    float a0 = ((vtx[idx[t + 0]].col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
    float a1 = ((vtx[idx[t + 1]].col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
    float a2 = ((vtx[idx[t + 2]].col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;

    float area = EdgeFunction(p0, p1, p2.x, p2.y);
    if (area < 0.f)
    {
      std::swap(p1, p2);
      std::swap(a1, a2);
      area = -area;
    }
    if (area < 1e-6f)
    {
      continue; // degenerate: no pixel can be covered
    }

    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ p0.x, p1.x, p2.x }) - 0.5f)));
    const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max({ p0.x, p1.x, p2.x }))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ p0.y, p1.y, p2.y }) - 0.5f)));
    const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({ p0.y, p1.y, p2.y }))));

    // Which edges a sample lying exactly on them still belongs to.
    const bool tl0 = IsTopLeftEdge(p1, p2);
    const bool tl1 = IsTopLeftEdge(p2, p0);
    const bool tl2 = IsTopLeftEdge(p0, p1);
    const float invArea = 1.f / area;

    for (int y = y0; y <= y1; ++y)
    {
      const float py = y + 0.5f;
      for (int x = x0; x <= x1; ++x)
      {
        const float px = x + 0.5f;
        const float w0 = EdgeFunction(p1, p2, px, py);
        const float w1 = EdgeFunction(p2, p0, px, py);
        const float w2 = EdgeFunction(p0, p1, px, py);
        if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
        {
          continue;
        }
        if ((w0 == 0.f && !tl0) || (w1 == 0.f && !tl1) || (w2 == 0.f && !tl2))
        {
          continue;
        }

        const float src =
          std::clamp((w0 * a0 + w1 * a1 + w2 * a2) * invArea, 0.f, 1.f);
        float& dst = cov[static_cast<std::size_t>(y) * w + x];
        dst = src + dst * (1.f - src);
      }
    }
  }
}

/**
 * Box-filter a (@p dstW * @p ss) x (@p dstH * @p ss) coverage buffer down to @p dstW x @p dstH.
 */
inline void Downsample(const float* src, int dstW, int dstH, int ss, float* dst)
{
  if (!src || !dst || dstW <= 0 || dstH <= 0 || ss <= 0)
  {
    return;
  }
  const int srcW = dstW * ss;
  const float inv = 1.f / static_cast<float>(ss * ss);
  for (int y = 0; y < dstH; ++y)
  {
    for (int x = 0; x < dstW; ++x)
    {
      float sum = 0.f;
      for (int j = 0; j < ss; ++j)
      {
        const float* row = src + static_cast<std::size_t>(y * ss + j) * srcW + x * ss;
        for (int i = 0; i < ss; ++i)
        {
          sum += row[i];
        }
      }
      dst[static_cast<std::size_t>(y) * dstW + x] = sum * inv;
    }
  }
}
}

#endif
