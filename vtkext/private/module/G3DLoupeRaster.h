/**
 * @file G3DLoupeRaster.h
 * @brief Software rasterizer for the eyedropper's cursor-following OS loupe (Win32 layered window).
 *
 * The out-of-window loupe (G3DScreenSampler) presents through UpdateLayeredWindow, which needs a
 * premultiplied-BGRA bitmap. Plain GDI (Ellipse / RoundRect) has no anti-aliasing, so a native-style
 * loupe has to be rasterized by hand. This renders the whole loupe in one analytic-coverage pass over
 * a small buffer, matching the browser's native EyeDropper loupe (which doc/dev/ui-styleguide.html
 * delegates to): a magnified pixel grid with graph-paper gridlines, clipped to a circle, a
 * highlighted center texel, and a thin two-tone rim — every edge anti-aliased, written premultiplied.
 *
 * Pure C++: no Win32 / GDI, so it is verifiable off-screen. The caller owns the layered-window
 * plumbing and draws the hex glyphs with GDI over the pill background RenderRoundRect fills.
 *
 * Coverage model: a filled disc of radius r has coverage clamp(r - d + 0.5) at distance d — full one
 * pixel inside, 0.5 on the ideal edge, zero one pixel out (a 1px analytic edge band). A stroked ring
 * is the difference of two such discs, so both edges are anti-aliased. Colors composite with
 * premultiplied "source over", matching what UpdateLayeredWindow's AC_SRC_ALPHA expects.
 */

#ifndef G3DLoupeRaster_h
#define G3DLoupeRaster_h

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace G3DLoupe
{

/// Analytic coverage of a filled disc of radius @p r at distance @p dist (1px anti-aliased edge).
inline float DiscCoverage(float dist, float r)
{
  return std::clamp(r - dist + 0.5f, 0.f, 1.f);
}

/// Coverage of a @p width-thick stroked ring at center-radius @p cr (a filled disc minus a smaller
/// filled disc), anti-aliased on both edges. @p dist is a euclidean radius for a round ring, or a
/// chebyshev radius (max(|dx|,|dy|)) for a square outline such as the center marker.
inline float RingCoverage(float dist, float cr, float width)
{
  return DiscCoverage(dist, cr + width * 0.5f) - DiscCoverage(dist, cr - width * 0.5f);
}

/// Signed distance to a rounded rectangle [x0,x1]x[y0,y1] with corner radius @p rr (negative inside).
inline float RoundRectSDF(
  float px, float py, float x0, float y0, float x1, float y1, float rr)
{
  const float hx = (x1 - x0) * 0.5f;
  const float hy = (y1 - y0) * 0.5f;
  const float cx = (x0 + x1) * 0.5f;
  const float cy = (y0 + y1) * 0.5f;
  const float qx = std::fabs(px - cx) - (hx - rr);
  const float qy = std::fabs(py - cy) - (hy - rr);
  const float ax = std::max(qx, 0.f);
  const float ay = std::max(qy, 0.f);
  return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.f) - rr;
}

/// Premultiplied "source over" accumulation. Channels are 0..1; the accumulator holds premultiplied
/// color plus straight alpha, seeded transparent.
struct Accum
{
  float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
  void Over(float sr, float sg, float sb, float sa)
  {
    const float ia = 1.f - sa;
    r = sr * sa + r * ia;
    g = sg * sa + g * ia;
    b = sb * sa + b * ia;
    a = sa + a * ia;
  }
};

/// Write a premultiplied accumulator to a top-down BGRA pixel (nothing written if fully transparent).
inline void StorePremul(unsigned char* dst, const Accum& px)
{
  if (px.a <= 0.f)
  {
    return;
  }
  dst[0] = static_cast<unsigned char>(std::lround(std::clamp(px.b, 0.f, 1.f) * 255.f));
  dst[1] = static_cast<unsigned char>(std::lround(std::clamp(px.g, 0.f, 1.f) * 255.f));
  dst[2] = static_cast<unsigned char>(std::lround(std::clamp(px.r, 0.f, 1.f) * 255.f));
  dst[3] = static_cast<unsigned char>(std::lround(std::clamp(px.a, 0.f, 1.f) * 255.f));
}

/// Geometry for one loupe frame. All lengths are device pixels.
struct LoupeSpec
{
  float centerX, centerY; ///< disc center within the buffer (pixel-center coordinates)
  float s;                ///< dpi scale (px per logical unit), for hairline / shadow constants
  float cell;             ///< zoomed texel edge
  int half;               ///< grid half-extent (the grid is 2*half+1 texels across)
  float gridR;            ///< inner disc radius == (half + 0.5) * cell
  float hair;             ///< rim hairline width
};

/// Render the loupe disc (soft shadow, circular-clipped magnified grid with graph-paper gridlines,
/// highlighted center texel, thin two-tone rim) into a premultiplied top-down BGRA buffer.
/// @p texel(gx, gy) returns >= 3 bytes (RGB) for grid cell (gx, gy), gx/gy in [-half, half]. Only the
/// pixels the disc / shadow touch are written; the caller zero-clears the rest.
template <class Texel>
void RenderDisc(unsigned char* bgra, int W, int H, const LoupeSpec& sp, Texel&& texel)
{
  const float whiteR = sp.gridR;               // white inner hairline center-radius
  const float darkR = sp.gridR + sp.hair;      // dark outer hairline center-radius
  const float outerR = darkR + sp.hair * 0.5f; // rim edge
  const float shadowR = outerR + 2.f * sp.s;
  const float shadowDy = 2.f * sp.s;
  const float markerHalf = sp.cell * 0.5f;                // center texel outline (chebyshev radius)
  const float lineW = std::max(1.f, 0.9f * sp.s);         // gridline thickness
  const float bboxR = std::max(outerR, shadowR) + 2.f;

  const int x0 = std::max(0, static_cast<int>(sp.centerX - bboxR));
  const int x1 = std::min(W, static_cast<int>(sp.centerX + bboxR) + 1);
  const int y0 = std::max(0, static_cast<int>(sp.centerY - bboxR - shadowDy));
  const int y1 = std::min(H, static_cast<int>(sp.centerY + bboxR + shadowDy) + 1);

  for (int y = y0; y < y1; ++y)
  {
    for (int x = x0; x < x1; ++x)
    {
      const float ox = x + 0.5f - sp.centerX;
      const float oy = y + 0.5f - sp.centerY;
      const float d = std::sqrt(ox * ox + oy * oy);
      Accum px;

      // soft drop shadow beneath everything, nudged down
      const float sdy = oy - shadowDy;
      const float shadowCov = DiscCoverage(std::sqrt(ox * ox + sdy * sdy), shadowR);
      if (shadowCov > 0.f)
      {
        px.Over(0.f, 0.f, 0.f, 0.22f * shadowCov);
      }

      // magnified texel grid, clipped to a circle (no square cell corner pokes past the rim)
      const float gridCov = DiscCoverage(d, sp.gridR);
      if (gridCov > 0.f)
      {
        const int gx = std::clamp(static_cast<int>(std::lround(ox / sp.cell)), -sp.half, sp.half);
        const int gy = std::clamp(static_cast<int>(std::lround(oy / sp.cell)), -sp.half, sp.half);
        const unsigned char* t = texel(gx, gy);
        const float cr = t[0] / 255.f, cg = t[1] / 255.f, cb = t[2] / 255.f;
        px.Over(cr, cg, cb, gridCov);

        // graph-paper gridlines at cell boundaries, tinted for contrast on any content (dark line
        // over a light texel, light line over a dark one)
        float fx = ox / sp.cell + 0.5f;
        float fy = oy / sp.cell + 0.5f;
        fx -= std::floor(fx);
        fy -= std::floor(fy);
        const float edge = std::min(std::min(fx, 1.f - fx), std::min(fy, 1.f - fy)) * sp.cell;
        const float lineCov = std::clamp(lineW * 0.5f + 0.5f - edge, 0.f, 1.f) * gridCov;
        if (lineCov > 0.f)
        {
          const float luma = 0.299f * cr + 0.587f * cg + 0.114f * cb;
          const float lc = luma > 0.5f ? 0.f : 1.f;
          px.Over(lc, lc, lc, 0.16f * lineCov);
        }
      }

      // thin two-tone rim: white inner hairline (reads on dark content) + dark outer hairline (reads
      // on light content), so the loupe edge stays visible over any desktop color
      const float whiteCov = RingCoverage(d, whiteR, sp.hair);
      if (whiteCov > 0.f)
      {
        px.Over(1.f, 1.f, 1.f, 0.90f * whiteCov);
      }
      const float darkCov = RingCoverage(d, darkR, sp.hair);
      if (darkCov > 0.f)
      {
        px.Over(0.f, 0.f, 0.f, 0.55f * darkCov);
      }

      // highlighted center texel: a dark 1px square outline with a white outline just inside it
      const float m = std::max(std::fabs(ox), std::fabs(oy));
      const float markDark = RingCoverage(m, markerHalf + 1.f * sp.s, 1.f * sp.s);
      if (markDark > 0.f)
      {
        px.Over(0.f, 0.f, 0.f, 0.60f * markDark);
      }
      const float markWhite = RingCoverage(m, markerHalf, 1.25f * sp.s);
      if (markWhite > 0.f)
      {
        px.Over(1.f, 1.f, 1.f, markWhite);
      }

      StorePremul(&bgra[(static_cast<std::size_t>(y) * W + x) * 4], px);
    }
  }
}

/// Fill an anti-aliased rounded rectangle (@p bg fill with a 1px inner @p border) into a
/// premultiplied top-down BGRA buffer. @p bg and @p border are RGBA (straight); composited over
/// whatever is already there. Reused for the readout pill, its drop shadow, and the color chip.
inline void RenderRoundRect(unsigned char* bgra, int W, int H, float x0, float y0, float x1,
  float y1, float rr, const unsigned char bg[4], const unsigned char border[4])
{
  const int ix0 = std::max(0, static_cast<int>(x0) - 1);
  const int iy0 = std::max(0, static_cast<int>(y0) - 1);
  const int ix1 = std::min(W, static_cast<int>(x1) + 1);
  const int iy1 = std::min(H, static_cast<int>(y1) + 1);
  for (int y = iy0; y < iy1; ++y)
  {
    for (int x = ix0; x < ix1; ++x)
    {
      const float sd = RoundRectSDF(x + 0.5f, y + 0.5f, x0, y0, x1, y1, rr);
      const float shape = std::clamp(0.5f - sd, 0.f, 1.f);
      if (shape <= 0.f)
      {
        continue;
      }
      // 1px border shell hugging the inside edge: coverage of the (-1, 0] SDF band
      const float borderCov = shape - std::clamp(-0.5f - sd, 0.f, 1.f);
      Accum px;
      px.Over(bg[0] / 255.f, bg[1] / 255.f, bg[2] / 255.f, bg[3] / 255.f * shape);
      px.Over(border[0] / 255.f, border[1] / 255.f, border[2] / 255.f, border[3] / 255.f * borderCov);

      unsigned char* o = &bgra[(static_cast<std::size_t>(y) * W + x) * 4];
      const float dstA = o[3] / 255.f;
      if (dstA > 0.f)
      {
        const float ia = 1.f - px.a;
        px.r += o[2] / 255.f * ia;
        px.g += o[1] / 255.f * ia;
        px.b += o[0] / 255.f * ia;
        px.a += dstA * ia;
      }
      StorePremul(o, px);
    }
  }
}

} // namespace G3DLoupe

#endif
