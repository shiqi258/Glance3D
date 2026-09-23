#include "G3DIconAtlas.h"

#include "F3DLog.h"
#include "G3DIconRaster.h"

// ImDrawListSharedData lives here. The baker owns a private one rather than borrowing the live
// context's: see Scratch below.
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
/// Samples per final pixel edge, so ss*ss samples per pixel.
constexpr int kSuperSample = 4;

/// Final-pixel margin around the glyph box. Two, not one: one for the anti-aliased edge (glyphs
/// graze the normalized box, e.g. the warning triangle's base corners at 0.095) and one for the
/// half-pixel the glyph may be shifted by inside the bitmap.
constexpr int kPad = 2;

/// Above this a rect costs more atlas than the stroke costs geometry; stroke it instead.
/// (`--font-scale 4` drives the largest icon, the FAB glyph, to ~128px.)
constexpr int kMaxSize = 256;

/// Hard stop so a caller animating sizes can never grow the atlas without bound.
constexpr std::size_t kMaxEntries = 512;

/// Baking is the default. `G3D_ICON_RASTER=0` forces the vector path, which is there for
/// bisecting a suspected regression in the field without a rebuild, and for A/B screenshots.
constexpr bool kDefaultEnabled = true;

struct Entry
{
  ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid;
  int pad = kPad;
  float subPixel = 0.f;
};

/**
 * The scratch draw list the glyph is stroked into, plus its own ImDrawListSharedData.
 *
 * Private on purpose. Borrowing ImGui::GetDrawListSharedData() would make a baked glyph depend on
 * the live style, initial flags and tessellation settings — the same icon could then rasterize
 * differently depending on when it was first drawn, which is exactly what image baselines cannot
 * tolerate. A private one also keeps this list out of the atlas's draw-list registry, so a
 * mid-frame atlas grow never walks into it, and it lets the bake run with no ImGui context at all.
 *
 * `list` is declared after `shared` so it destructs first and de-registers itself.
 */
struct Scratch
{
  ImDrawListSharedData shared;
  ImDrawList list{ &shared };

  Scratch()
  {
    // All tolerances are in supersampled pixels, so each is ss times finer than the live path.
    this->shared.SetCircleTessellationMaxError(0.30f);
    this->shared.CurveTessellationTol = 1.25f;
    // The one setting that really matters. ImGui widens a stroke by a whole AA_SIZE of feathering
    // (AA_SIZE == _FringeScale), which is what makes icons read fat. Scaling it with the
    // supersample factor keeps the bias at 1/ss of a final pixel instead of a whole one, and the
    // box filter downstream is what actually does the anti-aliasing.
    this->shared.InitialFringeScale = 1.f / static_cast<float>(kSuperSample);
    // Textured AA lines would make geometry sample the font atlas, and a vertex offset would make
    // the index buffer non-flat; the rasterizer wants neither. AntiAliasedLines itself stays on:
    // without it ImGui leaves thick-line corners unjoined.
    this->shared.InitialFlags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    this->shared.ClipRectFullscreen = ImVec4(-8192.f, -8192.f, 8192.f, 8192.f);
  }
};

std::unordered_map<std::uint32_t, Entry>& Cache()
{
  static std::unordered_map<std::uint32_t, Entry> cache;
  return cache;
}

/// The atlas the cached ids belong to, so a missed Invalidate() cannot go unnoticed.
const ImFontAtlas*& OwnerAtlas()
{
  static const ImFontAtlas* owner = nullptr;
  return owner;
}

/// id (11 bits) | size (12 bits) | half-pixel stroke units (8 bits) | sub-pixel phase (1 bit).
std::uint32_t MakeKey(G3DIconId id, int size, int th2, bool halfPhase)
{
  return (static_cast<std::uint32_t>(id) << 21) | (static_cast<std::uint32_t>(size) << 9) |
    (static_cast<std::uint32_t>(th2) << 1) | (halfPhase ? 1u : 0u);
}

bool Enabled()
{
  static const bool enabled = []
  {
    const char* env = std::getenv("G3D_ICON_RASTER");
    return env ? (std::string(env) != "0") : kDefaultEnabled;
  }();
  return enabled;
}

/**
 * Stroke the glyph supersampled, resolve it to coverage, and hand it to the font atlas.
 *
 * @p subPixel is the fractional part the glyph sits at on screen (0 or 0.5, decided by the size and
 * stroke parity). It is baked into the bitmap so the quad itself can always be drawn on a whole
 * pixel — a quad on a half pixel would be resampled by the bilinear filter and undo the whole
 * point of baking.
 *
 * The geometry comes from the one place it lives (G3DIcon::DrawUnsnapped), so a baked glyph cannot
 * drift from the stroked fallback: the bake is the same drawing, sampled densely.
 */
bool Bake(G3DIconId id, int size, float thickness, float subPixel, Entry& out)
{
  ImFontAtlas* atlas = ImGui::GetCurrentContext() ? ImGui::GetIO().Fonts : nullptr;
  if (atlas == nullptr || atlas->TexData == nullptr || atlas->TexData->Pixels == nullptr)
  {
    return false; // atlas not built yet: stroke this one, bake the next
  }

  const int edge = size + 2 * kPad;
  const int hiEdge = edge * kSuperSample;
  const float ss = static_cast<float>(kSuperSample);

  // 1. Stroke into the scratch list at kSuperSample x.
  static Scratch scratch;
  ImDrawList& dl = scratch.list;
  dl._ResetForNewFrame();
  dl.PushTexture(ImTextureRef());
  dl.PushClipRect(ImVec2(0.f, 0.f), ImVec2(static_cast<float>(hiEdge), static_cast<float>(hiEdge)),
    false);
  const float origin = (static_cast<float>(kPad) + subPixel) * ss;
  G3DIcon::DrawUnsnapped(
    &dl, id, ImVec2(origin, origin), size * ss, IM_COL32_WHITE, thickness * ss);

  // 2. Resolve to coverage and box-filter back down.
  std::vector<float> hi(static_cast<std::size_t>(hiEdge) * hiEdge, 0.f);
  for (const ImDrawCmd& cmd : dl.CmdBuffer)
  {
    if (cmd.ElemCount > 0)
    {
      G3DIconRaster::CompositeTriangles(hi.data(), hiEdge, hiEdge,
        dl.VtxBuffer.Data + cmd.VtxOffset, dl.IdxBuffer.Data + cmd.IdxOffset,
        static_cast<int>(cmd.ElemCount));
    }
  }
  std::vector<float> cov(static_cast<std::size_t>(edge) * edge, 0.f);
  G3DIconRaster::Downsample(hi.data(), edge, edge, kSuperSample, cov.data());

  // 3. Park the coverage in the font atlas. AddCustomRect packs immediately and queues the dirty
  //    block itself, so writing the pixels right here is enough for the backend to pick them up in
  //    this same frame.
  ImFontAtlasRect rect;
  const ImFontAtlasRectId rectId = atlas->AddCustomRect(edge, edge, &rect);
  if (rectId == ImFontAtlasRectId_Invalid)
  {
    return false;
  }

  ImTextureData* tex = atlas->TexData;
  for (int y = 0; y < edge; ++y)
  {
    void* row = tex->GetPixelsAt(rect.x, rect.y + y);
    for (int x = 0; x < edge; ++x)
    {
      const float c = std::clamp(cov[static_cast<std::size_t>(y) * edge + x], 0.f, 1.f);
      const int a = static_cast<int>(std::lround(c * 255.f));
      if (tex->BytesPerPixel == 4)
      {
        // White everywhere, including the fully transparent margin. The shader multiplies by the
        // vertex colour, so white is what makes the quad tintable; and holding RGB at white where
        // alpha is 0 keeps a bilinear tap across the edge from dragging the colour toward black.
        static_cast<unsigned int*>(row)[x] = IM_COL32(255, 255, 255, a);
      }
      else
      {
        static_cast<unsigned char*>(row)[x] = static_cast<unsigned char>(a);
      }
    }
  }

  out.rect = rectId;
  out.pad = kPad;
  out.subPixel = subPixel;
  return true;
}
}

//----------------------------------------------------------------------------
bool G3DIconAtlas::Blit(ImDrawList* drawList, G3DIconId id, const ImVec2& topLeft, float size,
  float thickness, ImU32 color)
{
  if (!Enabled() || drawList == nullptr)
  {
    return false;
  }

  const int sz = static_cast<int>(size);
  const int th2 = static_cast<int>(std::lround(thickness * 2.f));
  if (sz < 1 || sz > kMaxSize || th2 < 1 || th2 > 255)
  {
    return false;
  }

  // G3DIcon::Draw parks the glyph on a whole or a half pixel depending on stroke parity; that
  // fraction is part of the bitmap, so it is part of the key.
  const float frac = topLeft.x - std::floor(topLeft.x);
  const bool halfPhase = frac > 0.25f && frac < 0.75f;
  const float subPixel = halfPhase ? 0.5f : 0.f;

  ImFontAtlas* atlas = ImGui::GetCurrentContext() ? ImGui::GetIO().Fonts : nullptr;
  if (atlas == nullptr)
  {
    return false;
  }
  // Second line of defence behind the explicit Invalidate() calls in vtkF3DImguiActor: if the
  // atlas object changed under us, every cached id belongs to a dead packer.
  if (OwnerAtlas() != atlas)
  {
    Cache().clear();
    OwnerAtlas() = atlas;
  }

  std::unordered_map<std::uint32_t, Entry>& cache = Cache();
  const std::uint32_t key = MakeKey(id, sz, th2, halfPhase);
  auto it = cache.find(key);
  if (it == cache.end())
  {
    Entry entry;
    if (cache.size() >= kMaxEntries || !Bake(id, sz, thickness, subPixel, entry))
    {
      // Remember the miss too: a full atlas must not mean a fresh bake attempt every frame.
      entry.rect = ImFontAtlasRectId_Invalid;
      F3DLog::Print(F3DLog::Severity::Debug,
        "Icon atlas: could not bake glyph " + std::to_string(static_cast<int>(id)) + " at " +
          std::to_string(sz) + "px, falling back to stroking it");
    }
    it = cache.emplace(key, entry).first;
  }
  if (it->second.rect == ImFontAtlasRectId_Invalid)
  {
    return false;
  }

  ImFontAtlasRect rect;
  if (!atlas->GetCustomRect(it->second.rect, &rect))
  {
    return false;
  }
  // Third line of defence: a recycled id would resolve to a rectangle of the wrong shape (a font
  // glyph, say). Bail to the vector path rather than paint a slice of the letter "g".
  const int edge = sz + 2 * it->second.pad;
  if (rect.w != edge || rect.h != edge)
  {
    cache.clear();
    return false;
  }

  // UVs are re-read every frame on purpose: an atlas grow repacks every rect, which keeps the id
  // and the pixels but moves them, so a cached uv0/uv1 would point at someone else's glyph.
  // Subtracting the baked sub-pixel shift lands the quad on a whole pixel, texel for texel.
  const float off = static_cast<float>(it->second.pad) + it->second.subPixel;
  const ImVec2 p0(topLeft.x - off, topLeft.y - off);
  const ImVec2 p1(p0.x + rect.w, p0.y + rect.h);
  drawList->AddImage(atlas->TexRef, p0, p1, rect.uv0, rect.uv1, color);
  return true;
}

//----------------------------------------------------------------------------
void G3DIconAtlas::Invalidate()
{
  // No RemoveCustomRect: this runs when the atlas that owned those rects is already gone.
  Cache().clear();
  OwnerAtlas() = nullptr;
}
