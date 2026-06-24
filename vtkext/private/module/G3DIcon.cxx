#include "G3DIcon.h"

#include <algorithm>
#include <cmath>

namespace
{
// Draw each icon inside the unit box [0,1]x[0,1] mapped to screen via P(), so the routines stay
// resolution-independent and only deal in normalized coordinates.
struct IconCanvas
{
  ImDrawList* dl;
  ImVec2 topLeft;
  float size;
  ImU32 color;
  float th;

  ImVec2 P(float nx, float ny) const
  {
    return ImVec2(this->topLeft.x + nx * this->size, this->topLeft.y + ny * this->size);
  }
  float R(float nr) const { return nr * this->size; }

  void Line(float x0, float y0, float x1, float y1) const
  {
    this->dl->AddLine(this->P(x0, y0), this->P(x1, y1), this->color, this->th);
  }
  void Dot(float x, float y, float nr) const
  {
    this->dl->AddCircleFilled(this->P(x, y), this->R(nr), this->color, 16);
  }
  void Ring(float x, float y, float nr) const
  {
    this->dl->AddCircle(this->P(x, y), this->R(nr), this->color, 0, this->th);
  }
  void Poly(const ImVec2* pts, int n) const
  {
    this->dl->AddPolyline(pts, n, this->color, ImDrawFlags_None, this->th);
  }
};

void DrawSliders(const IconCanvas& c)
{
  const float rows[3] = { 0.30f, 0.50f, 0.70f };
  const float knobX[3] = { 0.66f, 0.40f, 0.58f };
  for (int i = 0; i < 3; ++i)
  {
    c.Line(0.18f, rows[i], 0.82f, rows[i]);
    c.Dot(knobX[i], rows[i], 0.07f);
  }
}

void DrawChevron(const IconCanvas& c, G3DIconId id)
{
  ImVec2 pts[3];
  switch (id)
  {
    case G3DIconId::ChevronRight:
      pts[0] = c.P(0.40f, 0.24f);
      pts[1] = c.P(0.64f, 0.50f);
      pts[2] = c.P(0.40f, 0.76f);
      break;
    case G3DIconId::ChevronLeft:
      pts[0] = c.P(0.60f, 0.24f);
      pts[1] = c.P(0.36f, 0.50f);
      pts[2] = c.P(0.60f, 0.76f);
      break;
    case G3DIconId::ChevronUp:
      pts[0] = c.P(0.24f, 0.60f);
      pts[1] = c.P(0.50f, 0.36f);
      pts[2] = c.P(0.76f, 0.60f);
      break;
    case G3DIconId::ChevronDown:
    default:
      pts[0] = c.P(0.24f, 0.40f);
      pts[1] = c.P(0.50f, 0.64f);
      pts[2] = c.P(0.76f, 0.40f);
      break;
  }
  c.Poly(pts, 3);
}

void DrawEye(const IconCanvas& c, bool off)
{
  c.dl->AddBezierQuadratic(c.P(0.15f, 0.5f), c.P(0.5f, 0.18f), c.P(0.85f, 0.5f), c.color, c.th);
  c.dl->AddBezierQuadratic(c.P(0.85f, 0.5f), c.P(0.5f, 0.82f), c.P(0.15f, 0.5f), c.color, c.th);
  c.Dot(0.5f, 0.5f, 0.12f);
  if (off)
  {
    c.Line(0.18f, 0.18f, 0.82f, 0.82f);
  }
}

void DrawGrid(const IconCanvas& c)
{
  c.dl->AddRect(c.P(0.20f, 0.20f), c.P(0.80f, 0.80f), c.color, 0.f, ImDrawFlags_None, c.th);
  c.Line(0.40f, 0.20f, 0.40f, 0.80f);
  c.Line(0.60f, 0.20f, 0.60f, 0.80f);
  c.Line(0.20f, 0.40f, 0.80f, 0.40f);
  c.Line(0.20f, 0.60f, 0.80f, 0.60f);
}

void DrawAxis(const IconCanvas& c)
{
  const float ox = 0.30f, oy = 0.72f;
  c.Line(ox, oy, ox, 0.22f);   // up
  c.Line(ox, oy, 0.80f, oy);   // right
  c.Line(ox, oy, 0.58f, 0.50f); // depth
}

void DrawFit(const IconCanvas& c)
{
  // Four corner brackets framing the center — the universal "fit / frame to view" glyph.
  const float a = 0.22f, b = 0.78f, k = 0.18f;
  c.Line(a, a, a + k, a);
  c.Line(a, a, a, a + k); // top-left
  c.Line(b, a, b - k, a);
  c.Line(b, a, b, a + k); // top-right
  c.Line(a, b, a + k, b);
  c.Line(a, b, a, b - k); // bottom-left
  c.Line(b, b, b - k, b);
  c.Line(b, b, b, b - k); // bottom-right
}

void DrawCamera(const IconCanvas& c)
{
  c.dl->AddRect(c.P(0.14f, 0.34f), c.P(0.86f, 0.78f), c.color, c.R(0.06f), ImDrawFlags_None, c.th);
  ImVec2 hump[4] = { c.P(0.34f, 0.34f), c.P(0.40f, 0.24f), c.P(0.56f, 0.24f), c.P(0.62f, 0.34f) };
  c.Poly(hump, 4);
  c.Ring(0.50f, 0.56f, 0.13f);
}

void DrawCube(const IconCanvas& c)
{
  const ImVec2 top = c.P(0.50f, 0.16f);
  const ImVec2 ur = c.P(0.84f, 0.34f);
  const ImVec2 lr = c.P(0.84f, 0.66f);
  const ImVec2 bot = c.P(0.50f, 0.84f);
  const ImVec2 ll = c.P(0.16f, 0.66f);
  const ImVec2 ul = c.P(0.16f, 0.34f);
  const ImVec2 ctr = c.P(0.50f, 0.50f);
  ImVec2 hex[7] = { top, ur, lr, bot, ll, ul, top };
  c.Poly(hex, 7);
  c.dl->AddLine(ctr, top, c.color, c.th);
  c.dl->AddLine(ctr, ll, c.color, c.th);
  c.dl->AddLine(ctr, lr, c.color, c.th);
}

void DrawFolder(const IconCanvas& c, bool open)
{
  if (open)
  {
    // Back panel + tab peeking above, then the opened front as a flared tray (both closed shapes so
    // the glyph never reads as an incomplete outline).
    ImVec2 back[7] = { c.P(0.15f, 0.52f), c.P(0.15f, 0.30f), c.P(0.37f, 0.30f), c.P(0.44f, 0.40f),
      c.P(0.85f, 0.40f), c.P(0.85f, 0.52f), c.P(0.15f, 0.52f) };
    c.Poly(back, 7);
    ImVec2 tray[5] = { c.P(0.07f, 0.73f), c.P(0.22f, 0.50f), c.P(0.93f, 0.50f), c.P(0.78f, 0.73f),
      c.P(0.07f, 0.73f) };
    c.Poly(tray, 5);
    return;
  }
  // Closed folder: a raised tab on the upper-left, then the body — one closed outline.
  ImVec2 p[7] = { c.P(0.15f, 0.30f), c.P(0.37f, 0.30f), c.P(0.44f, 0.40f), c.P(0.85f, 0.40f),
    c.P(0.85f, 0.74f), c.P(0.15f, 0.74f), c.P(0.15f, 0.30f) };
  c.Poly(p, 7);
}

void DrawLayers(const IconCanvas& c)
{
  // Three stacked diamonds (isometric sheets).
  auto sheet = [&](float cy)
  {
    ImVec2 d[5] = { c.P(0.50f, cy - 0.13f), c.P(0.82f, cy), c.P(0.50f, cy + 0.13f),
      c.P(0.18f, cy), c.P(0.50f, cy - 0.13f) };
    c.Poly(d, 5);
  };
  sheet(0.30f);
  // Lower two sheets drawn as side rails so the stack reads without clutter.
  c.dl->AddLine(c.P(0.18f, 0.50f), c.P(0.50f, 0.63f), c.color, c.th);
  c.dl->AddLine(c.P(0.82f, 0.50f), c.P(0.50f, 0.63f), c.color, c.th);
  c.dl->AddLine(c.P(0.18f, 0.66f), c.P(0.50f, 0.79f), c.color, c.th);
  c.dl->AddLine(c.P(0.82f, 0.66f), c.P(0.50f, 0.79f), c.color, c.th);
}

void DrawLight(const IconCanvas& c)
{
  // Sun: a small disc with radiating spokes.
  c.Ring(0.50f, 0.46f, 0.16f);
  const float r0 = 0.26f, r1 = 0.36f;
  for (int i = 0; i < 8; ++i)
  {
    const float ang = i * 0.7853981f; // 45° steps
    const float dx = std::cos(ang), dy = std::sin(ang);
    c.Line(0.50f + dx * r0, 0.46f + dy * r0, 0.50f + dx * r1, 0.46f + dy * r1);
  }
}

void DrawImage(const IconCanvas& c)
{
  c.dl->AddRect(c.P(0.16f, 0.22f), c.P(0.84f, 0.78f), c.color, c.R(0.06f), ImDrawFlags_None, c.th);
  c.Dot(0.36f, 0.40f, 0.06f); // sun
  // Mountain range along the bottom edge.
  ImVec2 hill[5] = { c.P(0.16f, 0.70f), c.P(0.38f, 0.50f), c.P(0.52f, 0.62f), c.P(0.66f, 0.46f),
    c.P(0.84f, 0.70f) };
  c.Poly(hill, 5);
}

void DrawEdges(const IconCanvas& c)
{
  // Triangle outline with emphasized vertices — reads as mesh edges / wireframe.
  ImVec2 t[4] = { c.P(0.50f, 0.20f), c.P(0.82f, 0.78f), c.P(0.18f, 0.78f), c.P(0.50f, 0.20f) };
  c.Poly(t, 4);
  c.Dot(0.50f, 0.20f, 0.07f);
  c.Dot(0.82f, 0.78f, 0.07f);
  c.Dot(0.18f, 0.78f, 0.07f);
}

void DrawInfo(const IconCanvas& c)
{
  c.Ring(0.50f, 0.50f, 0.32f);
  c.Dot(0.50f, 0.35f, 0.045f);      // dot of the "i"
  c.Line(0.50f, 0.47f, 0.50f, 0.67f); // stem
}

void DrawLock(const IconCanvas& c)
{
  // Shackle arc above, body rectangle below.
  c.dl->AddBezierQuadratic(
    c.P(0.34f, 0.46f), c.P(0.34f, 0.20f), c.P(0.50f, 0.20f), c.color, c.th);
  c.dl->AddBezierQuadratic(
    c.P(0.50f, 0.20f), c.P(0.66f, 0.20f), c.P(0.66f, 0.46f), c.color, c.th);
  c.dl->AddRect(c.P(0.30f, 0.46f), c.P(0.70f, 0.80f), c.color, c.R(0.05f), ImDrawFlags_None, c.th);
  c.Dot(0.50f, 0.63f, 0.05f);
}
}

//----------------------------------------------------------------------------
void G3DIcon::Draw(
  ImDrawList* drawList, G3DIconId id, const ImVec2& center, float size, ImU32 color, float thickness)
{
  if (!drawList || size < 1.f)
  {
    return;
  }

  IconCanvas c;
  c.dl = drawList;
  c.size = size;
  c.color = color;
  c.topLeft = ImVec2(center.x - size * 0.5f, center.y - size * 0.5f);
  c.th = thickness > 0.f ? thickness : std::max(1.f, size * 0.085f);

  // The global style disables line anti-aliasing; enable it locally so icons stay crisp, then
  // restore (same pattern as the loading overlay / FAB).
  const ImDrawListFlags saved = drawList->Flags;
  drawList->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

  switch (id)
  {
    case G3DIconId::Sliders:
      DrawSliders(c);
      break;
    case G3DIconId::Close:
      c.Line(0.26f, 0.26f, 0.74f, 0.74f);
      c.Line(0.74f, 0.26f, 0.26f, 0.74f);
      break;
    case G3DIconId::ChevronRight:
    case G3DIconId::ChevronDown:
    case G3DIconId::ChevronLeft:
    case G3DIconId::ChevronUp:
      DrawChevron(c, id);
      break;
    case G3DIconId::Plus:
      c.Line(0.50f, 0.22f, 0.50f, 0.78f);
      c.Line(0.22f, 0.50f, 0.78f, 0.50f);
      break;
    case G3DIconId::Search:
      c.Ring(0.42f, 0.42f, 0.22f);
      c.Line(0.59f, 0.59f, 0.80f, 0.80f);
      break;
    case G3DIconId::Dots:
      c.Dot(0.50f, 0.26f, 0.07f);
      c.Dot(0.50f, 0.50f, 0.07f);
      c.Dot(0.50f, 0.74f, 0.07f);
      break;
    case G3DIconId::Eye:
      DrawEye(c, false);
      break;
    case G3DIconId::EyeOff:
      DrawEye(c, true);
      break;
    case G3DIconId::Grid:
      DrawGrid(c);
      break;
    case G3DIconId::Axis:
      DrawAxis(c);
      break;
    case G3DIconId::Fit:
      DrawFit(c);
      break;
    case G3DIconId::Camera:
      DrawCamera(c);
      break;
    case G3DIconId::Cube:
      DrawCube(c);
      break;
    case G3DIconId::Folder:
      DrawFolder(c, false);
      break;
    case G3DIconId::FolderOpen:
      DrawFolder(c, true);
      break;
    case G3DIconId::Layers:
      DrawLayers(c);
      break;
    case G3DIconId::Light:
      DrawLight(c);
      break;
    case G3DIconId::Image:
      DrawImage(c);
      break;
    case G3DIconId::Lock:
      DrawLock(c);
      break;
    case G3DIconId::Info:
      DrawInfo(c);
      break;
    case G3DIconId::Edges:
      DrawEdges(c);
      break;
  }

  drawList->Flags = saved;
}

//----------------------------------------------------------------------------
void G3DIcon::Inline(G3DIconId id, float size, ImU32 color)
{
  const ImVec2 p = ImGui::GetCursorScreenPos();
  G3DIcon::Draw(
    ImGui::GetWindowDrawList(), id, ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size, color);
  ImGui::Dummy(ImVec2(size, size));
}
