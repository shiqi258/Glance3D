#include "G3DScreenSampler.h"

#include "G3DWidgets.h"

#include <vtkRenderWindow.h>

#ifdef _WIN32
#include <vtkWindows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
// half-extent of the captured square around the cursor: the loupe magnifies cursor +/-5 device px,
// the rest is slack for the sub-frame cursor motion between this capture and the UI build reading
// io.MousePos (fast flicks past the slack just show the not-samplable ring for a frame)
constexpr int PATCH_HALF = 64;

bool CaptureHeld = false;
bool CursorOverridden = false;

// BitBlt the desktop around the cursor into a top-down 32bpp DIB and repack BGRA -> RGBA.
// Virtual-screen coordinates handle multi-monitor setups (negative for monitors left/above the
// primary); plain SRCCOPY (no CAPTUREBLT — its per-frame cursor flicker outweighs layered-window
// coverage).
bool CaptureDesktopPatch(
  const POINT& cur, std::vector<unsigned char>& rgba, int& w, int& h, int& x0, int& y0)
{
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  const int cx = static_cast<int>(cur.x);
  const int cy = static_cast<int>(cur.y);
  x0 = std::max(cx - PATCH_HALF, vx);
  y0 = std::max(cy - PATCH_HALF, vy);
  const int x1 = std::min(cx + PATCH_HALF + 1, vx + vw);
  const int y1 = std::min(cy + PATCH_HALF + 1, vy + vh);
  w = x1 - x0;
  h = y1 - y0;
  if (w <= 0 || h <= 0)
  {
    return false;
  }

  HDC screen = GetDC(nullptr);
  if (screen == nullptr)
  {
    return false;
  }
  bool ok = false;
  HDC mem = CreateCompatibleDC(screen);
  if (mem != nullptr)
  {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // negative = top-down rows (window / ImGui orientation)
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib != nullptr && bits != nullptr)
    {
      HGDIOBJ old = SelectObject(mem, dib);
      if (BitBlt(mem, 0, 0, w, h, screen, x0, y0, SRCCOPY))
      {
        GdiFlush();
        const unsigned char* src = static_cast<const unsigned char*>(bits);
        rgba.resize(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i)
        {
          rgba[i * 4 + 0] = src[i * 4 + 2];
          rgba[i * 4 + 1] = src[i * 4 + 1];
          rgba[i * 4 + 2] = src[i * 4 + 0];
          rgba[i * 4 + 3] = 255;
        }
        ok = true;
      }
      SelectObject(mem, old);
    }
    if (dib != nullptr)
    {
      DeleteObject(dib);
    }
    DeleteDC(mem);
  }
  ReleaseDC(nullptr, screen);
  return ok;
}
} // namespace

//----------------------------------------------------------------------------
void G3DScreenSampler::Update(vtkRenderWindow* renWin)
{
  static const bool disabled = []
  {
    const char* env = std::getenv("G3D_EYEDROP_SCREEN");
    return env != nullptr && std::strcmp(env, "0") == 0;
  }();

  HWND hwnd =
    renWin != nullptr ? static_cast<HWND>(renWin->GetGenericWindowId()) : nullptr;
  const bool active = !disabled && hwnd != nullptr && G3DWidgets::EyedropperActive();

  // OS mouse capture + crosshair are foreground-only: they reroute the user's real input, which a
  // background / offscreen window must never do (sampling stays armed then — the viewport source
  // and the patch below keep working through normal in-window mouse moves)
  const bool foreground = active && GetForegroundWindow() == hwnd;
  if (!foreground)
  {
    if (CaptureHeld)
    {
      if (GetCapture() == hwnd)
      {
        ReleaseCapture();
      }
      CaptureHeld = false;
    }
    if (CursorOverridden)
    {
      renWin->SetCurrentCursor(VTK_CURSOR_DEFAULT);
      CursorOverridden = false;
    }
  }
  else
  {
    // capture keeps WM_MOUSEMOVE / button messages flowing to the interactor while the cursor
    // roams outside the client area (VTK re-captures on the picking click itself; the mode ends
    // then, so its matching release needs no handling here)
    if (GetCapture() != hwnd)
    {
      SetCapture(hwnd);
    }
    CaptureHeld = true;
    // crosshair as the aim point wherever the loupe is not centered on the cursor; with the mouse
    // captured no WM_SETCURSOR arrives, so the immediate SetCursor in SetCurrentCursor sticks
    // everywhere until restored above
    if (!CursorOverridden)
    {
      renWin->SetCurrentCursor(VTK_CURSOR_CROSSHAIR);
      CursorOverridden = true;
    }
  }
  if (!active)
  {
    return;
  }

  // the desktop patch feed runs regardless of focus: it only reads the screen around the real
  // cursor, so hovering a not-focused window still samples correctly through in-window moves
  POINT cur;
  if (!GetCursorPos(&cur))
  {
    return;
  }
  std::vector<unsigned char> rgba;
  int w = 0;
  int h = 0;
  int x0 = 0;
  int y0 = 0;
  if (!CaptureDesktopPatch(cur, rgba, w, h, x0, y0))
  {
    return;
  }
  // patch origin: virtual-screen -> window client px (== ImGui display px, top-left origin; the
  // process is per-monitor DPI aware so both sides are physical pixels)
  POINT origin{ x0, y0 };
  ScreenToClient(hwnd, &origin);
  G3DWidgets::SubmitEyedropperScreenPatch(std::move(rgba), w, h, origin.x, origin.y);
}

#else

//----------------------------------------------------------------------------
void G3DScreenSampler::Update(vtkRenderWindow*)
{
  // no desktop feed on this platform: the eyedropper samples the viewport rect only
}

#endif
