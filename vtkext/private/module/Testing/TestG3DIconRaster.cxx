#include "G3DIconRaster.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Check(bool ok, const std::string& label)
{
  if (!ok)
  {
    std::cerr << "FAIL [" << label << "]\n";
    ++failures;
  }
}

void ExpectNear(float got, float want, float tol, const std::string& label)
{
  if (!(got >= want - tol && got <= want + tol))
  {
    std::cerr << "FAIL [" << label << "]: got " << got << " want " << want << " +/- " << tol
              << "\n";
    ++failures;
  }
}

ImDrawVert Vert(float x, float y, unsigned char alpha)
{
  ImDrawVert v;
  v.pos = ImVec2(x, y);
  v.uv = ImVec2(0.f, 0.f);
  v.col = IM_COL32(255, 255, 255, alpha);
  return v;
}

/// Axis-aligned opaque quad [x0,x1] x [y0,y1] as two triangles sharing the diagonal.
void PushQuad(std::vector<ImDrawVert>& vtx, std::vector<ImDrawIdx>& idx, float x0, float y0,
  float x1, float y1, unsigned char alpha)
{
  const ImDrawIdx base = static_cast<ImDrawIdx>(vtx.size());
  vtx.push_back(Vert(x0, y0, alpha));
  vtx.push_back(Vert(x1, y0, alpha));
  vtx.push_back(Vert(x1, y1, alpha));
  vtx.push_back(Vert(x0, y1, alpha));
  const ImDrawIdx order[6] = { 0, 1, 2, 0, 2, 3 };
  for (ImDrawIdx o : order)
  {
    idx.push_back(static_cast<ImDrawIdx>(base + o));
  }
}
}

int TestG3DIconRaster(int, char*[])
{
  // --- a fully covered pixel reads 1, an untouched one reads 0 -----------------------------------
  {
    std::vector<ImDrawVert> vtx;
    std::vector<ImDrawIdx> idx;
    PushQuad(vtx, idx, 1.f, 1.f, 3.f, 3.f, 255);
    std::vector<float> cov(16, 0.f);
    G3DIconRaster::CompositeTriangles(cov.data(), 4, 4, vtx.data(), idx.data(),
      static_cast<int>(idx.size()));
    ExpectNear(cov[1 * 4 + 1], 1.f, 1e-5f, "inside pixel is opaque");
    ExpectNear(cov[2 * 4 + 2], 1.f, 1e-5f, "inside pixel is opaque (2)");
    ExpectNear(cov[0 * 4 + 0], 0.f, 1e-5f, "outside pixel stays clear");
    ExpectNear(cov[3 * 4 + 3], 0.f, 1e-5f, "outside pixel stays clear (2)");
  }

  // --- the shared diagonal is shaded exactly once (top-left rule) --------------------------------
  // Two half-transparent triangles sharing an edge must composite to 0.5, not 0.75: a seam along
  // the diagonal is the classic symptom of a missing fill rule.
  {
    std::vector<ImDrawVert> vtx;
    std::vector<ImDrawIdx> idx;
    PushQuad(vtx, idx, 0.f, 0.f, 4.f, 4.f, 128);
    std::vector<float> cov(16, 0.f);
    G3DIconRaster::CompositeTriangles(cov.data(), 4, 4, vtx.data(), idx.data(),
      static_cast<int>(idx.size()));
    const float want = 128.f / 255.f;
    for (int i = 0; i < 16; ++i)
    {
      ExpectNear(cov[i], want, 1e-5f, "half-alpha quad has no diagonal seam");
    }
  }

  // --- vertex alpha interpolates across the triangle ---------------------------------------------
  {
    std::vector<ImDrawVert> vtx = { Vert(0.f, 0.f, 0), Vert(8.f, 0.f, 255), Vert(8.f, 8.f, 255) };
    std::vector<ImDrawIdx> idx = { 0, 1, 2 };
    std::vector<float> cov(64, 0.f);
    G3DIconRaster::CompositeTriangles(cov.data(), 8, 8, vtx.data(), idx.data(), 3);
    // Coverage must not decrease left to right along a row inside the triangle.
    bool monotonic = true;
    for (int x = 1; x < 8; ++x)
    {
      if (cov[3 * 8 + x] < cov[3 * 8 + x - 1] - 1e-4f)
      {
        monotonic = false;
      }
    }
    Check(monotonic, "alpha gradient is monotonic along the ramp");
    Check(cov[3 * 8 + 7] > cov[3 * 8 + 1], "alpha gradient actually ramps");
  }

  // --- source-over accumulates, it does not add ---------------------------------------------------
  {
    std::vector<ImDrawVert> vtx;
    std::vector<ImDrawIdx> idx;
    PushQuad(vtx, idx, 0.f, 0.f, 2.f, 2.f, 128);
    PushQuad(vtx, idx, 0.f, 0.f, 2.f, 2.f, 128);
    std::vector<float> cov(4, 0.f);
    G3DIconRaster::CompositeTriangles(cov.data(), 2, 2, vtx.data(), idx.data(),
      static_cast<int>(idx.size()));
    const float a = 128.f / 255.f;
    ExpectNear(cov[0], a + a * (1.f - a), 1e-5f, "overlap composites source-over");
    Check(cov[0] <= 1.f, "coverage never exceeds 1");
  }

  // --- geometry outside the buffer is clipped, not wrapped ---------------------------------------
  {
    std::vector<ImDrawVert> vtx;
    std::vector<ImDrawIdx> idx;
    PushQuad(vtx, idx, -10.f, -10.f, 2.f, 2.f, 255);
    std::vector<float> cov(16, 0.f);
    G3DIconRaster::CompositeTriangles(cov.data(), 4, 4, vtx.data(), idx.data(),
      static_cast<int>(idx.size()));
    ExpectNear(cov[0], 1.f, 1e-5f, "clipped quad still fills its visible corner");
    ExpectNear(cov[3 * 4 + 3], 0.f, 1e-5f, "clipped quad does not wrap around");
  }

  // --- downsample averages each ss x ss block ----------------------------------------------------
  {
    const int ss = 4;
    std::vector<float> src(8 * 8, 0.f);
    // Fill the top-left 4x4 block fully, leave the rest clear -> 1.0 and 0.0 after the box filter.
    for (int y = 0; y < 4; ++y)
    {
      for (int x = 0; x < 4; ++x)
      {
        src[static_cast<std::size_t>(y) * 8 + x] = 1.f;
      }
    }
    // Half of the top-right block -> 0.5.
    for (int y = 0; y < 2; ++y)
    {
      for (int x = 4; x < 8; ++x)
      {
        src[static_cast<std::size_t>(y) * 8 + x] = 1.f;
      }
    }
    std::vector<float> dst(4, -1.f);
    G3DIconRaster::Downsample(src.data(), 2, 2, ss, dst.data());
    ExpectNear(dst[0], 1.f, 1e-5f, "downsample: full block");
    ExpectNear(dst[1], 0.5f, 1e-5f, "downsample: half block");
    ExpectNear(dst[2], 0.f, 1e-5f, "downsample: empty block");
    ExpectNear(dst[3], 0.f, 1e-5f, "downsample: empty block (2)");
  }

  if (failures > 0)
  {
    std::cerr << failures << " icon-raster check(s) failed\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
