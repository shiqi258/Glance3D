#include "G3DScreenSampler.h"

#include "G3DWidgets.h"

#include <vtkRenderWindow.h>

#ifdef _WIN32
#include <vtkWindows.h>

#include "G3DLoupeRaster.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace
{
// half-extent of the captured square around the cursor: the loupe magnifies cursor +/-5 device px,
// the rest is slack for the sub-frame cursor motion between this capture and the UI build reading
// io.MousePos (fast flicks past the slack just show the not-samplable ring for a frame)
constexpr int PATCH_HALF = 64;
constexpr int LOUPE_HALF = 5; // 11x11 texel grid, mirrors the ImGui loupe

// Out-of-window integration. SetCapture is useless for a click-to-pick eyedropper: with no mouse
// button held, Windows only delivers WM_MOUSEMOVE to the capture window while the cursor is over
// windows of the capturing thread — moves over other applications never arrive, and the picking
// click activates them (verified live, see g3d 2026-07-02 12:38 session log). Industry pickers
// (e.g. PowerToys Color Picker) cover the screen with an invisible input window instead; we do the
// same while the eyedropper samples in the foreground window, relaying its mouse events into the
// render window's normal interactor path, plus a small layered loupe window following the cursor
// wherever it goes beyond the render window (inside it, the ImGui loupe takes over).
HWND RenderHwnd = nullptr;   // relay target, refreshed every Update
HWND InputOverlay = nullptr; // virtual-screen invisible input catcher (owns the mouse while armed)
HWND LoupeWnd = nullptr;     // cursor-following magnifier shown outside the render window
bool OverlaysShown = false;
bool ClassesRegistered = false;

// Screen frozen once when the eyedropper opens: the centered loupe reads its desktop patch from here
// (not a live grab), so it never samples — and magnifies — itself. Virtual-screen RGBA, top-down.
std::vector<unsigned char> ScreenSnapshot;
int SnapX = 0, SnapY = 0, SnapW = 0, SnapH = 0;
bool SnapValid = false;

//----------------------------------------------------------------------------
LRESULT CALLBACK G3DEyedropInputProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE; // keyboard focus (Esc to cancel) stays on the render window
    case WM_SETCURSOR:
      SetCursor(nullptr); // hide the OS cursor while sampling — the loupe's center marker is the aim
      return TRUE;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      if (RenderHwnd != nullptr)
      {
        // overlay client -> screen -> render-window client (signed, may be negative or beyond the
        // client area — the interactor reads MAKEPOINTS), then through its normal message path
        const POINTS pts = MAKEPOINTS(lp);
        POINT p{ pts.x, pts.y };
        ClientToScreen(wnd, &p);
        ScreenToClient(RenderHwnd, &p);
        PostMessageW(
          RenderHwnd, msg, wp, MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y)));
      }
      return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      return 0; // swallowed: no stray interaction reaches the applications beneath
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

//----------------------------------------------------------------------------
void EnsureClasses()
{
  if (ClassesRegistered)
  {
    return;
  }
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW ic{};
  ic.lpfnWndProc = G3DEyedropInputProc;
  ic.hInstance = inst;
  ic.hCursor = nullptr; // cursor hidden via WM_SETCURSOR while sampling
  ic.lpszClassName = L"Glance3DEyedropInput";
  RegisterClassW(&ic);
  WNDCLASSW lc{};
  lc.lpfnWndProc = DefWindowProcW;
  lc.hInstance = inst;
  lc.lpszClassName = L"Glance3DEyedropLoupe";
  RegisterClassW(&lc);
  ClassesRegistered = true;
}

//----------------------------------------------------------------------------
void ShowOverlays()
{
  EnsureClasses();
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (InputOverlay == nullptr)
  {
    InputOverlay =
      CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"Glance3DEyedropInput", L"", WS_POPUP, vx, vy, vw, vh, nullptr, nullptr, inst, nullptr);
    if (InputOverlay != nullptr)
    {
      // barely-there alpha keeps every pixel hit-testable; excluded from capture so the tint can
      // never nudge the colors our own BitBlt samples
      SetLayeredWindowAttributes(InputOverlay, 0, 1, LWA_ALPHA);
      SetWindowDisplayAffinity(InputOverlay, WDA_EXCLUDEFROMCAPTURE);
    }
  }
  if (LoupeWnd == nullptr)
  {
    // click-through (WS_EX_TRANSPARENT): picking clicks pass to the input overlay beneath
    LoupeWnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
      L"Glance3DEyedropLoupe", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  }
  if (InputOverlay == nullptr)
  {
    G3DWidgets::Trace("[Trace][cp.eyed] sampler input overlay creation FAILED (%lu)",
      static_cast<unsigned long>(GetLastError()));
    return;
  }
  SetWindowPos(InputOverlay, HWND_TOPMOST, vx, vy, vw, vh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  OverlaysShown = true;
  G3DWidgets::Trace("[Trace][cp.eyed] sampler overlays shown (virtual %d,%d %dx%d)", vx, vy, vw, vh);
}

//----------------------------------------------------------------------------
void HideOverlays()
{
  if (!OverlaysShown)
  {
    return;
  }
  if (InputOverlay != nullptr)
  {
    ShowWindow(InputOverlay, SW_HIDE);
  }
  if (LoupeWnd != nullptr)
  {
    ShowWindow(LoupeWnd, SW_HIDE);
  }
  OverlaysShown = false;
  G3DWidgets::Trace("[Trace][cp.eyed] sampler overlays hidden");
}

//----------------------------------------------------------------------------
// BitBlt the whole virtual screen into ScreenSnapshot (top-down 32bpp -> RGBA). Called once per
// arming (before the loupe is visible), so the frozen snapshot never contains the loupe. Virtual-
// screen coordinates handle multi-monitor setups (negative for monitors left/above the primary);
// plain SRCCOPY (no CAPTUREBLT — its cursor flicker outweighs layered-window coverage).
bool CaptureVirtualScreen()
{
  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (vw <= 0 || vh <= 0)
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
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh; // negative = top-down rows (window / ImGui orientation)
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib != nullptr && bits != nullptr)
    {
      HGDIOBJ old = SelectObject(mem, dib);
      if (BitBlt(mem, 0, 0, vw, vh, screen, vx, vy, SRCCOPY))
      {
        GdiFlush();
        const unsigned char* src = static_cast<const unsigned char*>(bits);
        ScreenSnapshot.resize(static_cast<std::size_t>(vw) * vh * 4);
        for (std::size_t i = 0, n = static_cast<std::size_t>(vw) * vh; i < n; ++i)
        {
          ScreenSnapshot[i * 4 + 0] = src[i * 4 + 2];
          ScreenSnapshot[i * 4 + 1] = src[i * 4 + 1];
          ScreenSnapshot[i * 4 + 2] = src[i * 4 + 0];
          ScreenSnapshot[i * 4 + 3] = 255;
        }
        SnapX = vx;
        SnapY = vy;
        SnapW = vw;
        SnapH = vh;
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

//----------------------------------------------------------------------------
// Copy the cursor +/- PATCH_HALF neighborhood out of the frozen snapshot (clamped to its bounds),
// with the same (rgba, w, h, x0, y0) contract the widget feed + loupe expect. Virtual-screen coords.
bool ExtractPatchFromSnapshot(
  const POINT& cur, std::vector<unsigned char>& rgba, int& w, int& h, int& x0, int& y0)
{
  if (!SnapValid || ScreenSnapshot.empty())
  {
    return false;
  }
  x0 = std::max(static_cast<int>(cur.x) - PATCH_HALF, SnapX);
  y0 = std::max(static_cast<int>(cur.y) - PATCH_HALF, SnapY);
  const int x1 = std::min(static_cast<int>(cur.x) + PATCH_HALF + 1, SnapX + SnapW);
  const int y1 = std::min(static_cast<int>(cur.y) + PATCH_HALF + 1, SnapY + SnapH);
  w = x1 - x0;
  h = y1 - y0;
  if (w <= 0 || h <= 0)
  {
    return false;
  }
  rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
  {
    const unsigned char* srow =
      &ScreenSnapshot[(static_cast<std::size_t>((y0 - SnapY) + y) * SnapW + (x0 - SnapX)) * 4];
    std::memcpy(&rgba[static_cast<std::size_t>(y) * w * 4], srow, static_cast<std::size_t>(w) * 4);
  }
  return true;
}

//----------------------------------------------------------------------------
// Paint + place the OS loupe centered on the (hidden) cursor while it roams beyond the render window:
// a magnified 11x11 texel grid with graph-paper gridlines, a highlighted center texel, a thin rim,
// and a color-chip + hex readout pill below — matching the browser's native EyeDropper loupe that the
// styleguide delegates to. Fully software-rasterized with anti-aliasing (G3DLoupeRaster.h) into a
// premultiplied 32bpp DIB, shown via UpdateLayeredWindow. It can sit centered on the cursor because
// the desktop patch is read from a screen snapshot frozen when the eyedropper opened (Update) — which
// never contains the loupe, so sampling never magnifies the loupe itself.
void UpdateLoupe(HWND renderHwnd, const POINT& cur, const std::vector<unsigned char>& rgba, int pw,
  int ph, int px0, int py0)
{
  if (LoupeWnd == nullptr)
  {
    return;
  }
  POINT c = cur;
  ScreenToClient(renderHwnd, &c);
  RECT rc{};
  GetClientRect(renderHwnd, &rc);
  if (PtInRect(&rc, c))
  {
    ShowWindow(LoupeWnd, SW_HIDE); // inside the window the ImGui loupe takes over
    return;
  }

  const UINT dpi = GetDpiForWindow(renderHwnd);
  const double s = dpi > 0 ? dpi / 96.0 : 1.0;
  const int cell = std::max(6, static_cast<int>(9.0 * s + 0.5));
  const int gridR = cell * LOUPE_HALF + cell / 2;
  const int hair = std::max(1, static_cast<int>(1.25 * s + 0.5));
  const int outer = gridR + 2 * hair + static_cast<int>(4.0 * s + 0.5);
  const int fh = std::max(11, static_cast<int>(14.0 * s + 0.5));
  const int padX = static_cast<int>(9.0 * s + 0.5);
  const int padY = static_cast<int>(5.0 * s + 0.5);
  const int gap = static_cast<int>(8.0 * s + 0.5);
  const int chip = static_cast<int>(fh * 0.8 + 0.5); // sampled-color chip edge (~cap height) in pill
  const int chipGap = static_cast<int>(6.0 * s + 0.5);
  const int W = outer * 2 + 2;
  const int pillH = fh + padY * 2;
  const int H = outer * 2 + gap + pillH + static_cast<int>(3.0 * s + 0.5) + 2;
  const int centerX = W / 2;
  const int centerY = outer + 1;

  const int cx = static_cast<int>(cur.x) - px0;
  const int cy = static_cast<int>(cur.y) - py0;
  auto texel = [&](int dx, int dy) -> const unsigned char*
  {
    const int tx = std::clamp(cx + dx, 0, pw - 1);
    const int ty = std::clamp(cy + dy, 0, ph - 1);
    return &rgba[(static_cast<std::size_t>(ty) * pw + tx) * 4];
  };
  const unsigned char* hp = texel(0, 0);

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = W;
  bmi.bmiHeader.biHeight = -H;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  if (screen == nullptr)
  {
    return;
  }
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib =
    mem != nullptr ? CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (dib == nullptr || bits == nullptr)
  {
    if (dib != nullptr)
    {
      DeleteObject(dib);
    }
    if (mem != nullptr)
    {
      DeleteDC(mem);
    }
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ oldBmp = SelectObject(mem, dib);
  unsigned char* px = static_cast<unsigned char*>(bits);
  std::memset(px, 0, static_cast<std::size_t>(W) * H * 4);

  // Render the loupe disc (soft shadow, circular-clipped magnified grid with graph-paper gridlines,
  // highlighted center texel, thin two-tone rim) with per-pixel analytic anti-aliasing, into the
  // premultiplied BGRA DIB. Matches the browser's native EyeDropper loupe and the in-window ImGui
  // loupe (DrawEyedropOverlay).
  const float sf = static_cast<float>(s);
  G3DLoupe::LoupeSpec sp;
  sp.centerX = static_cast<float>(centerX) + 0.5f;
  sp.centerY = static_cast<float>(centerY) + 0.5f;
  sp.s = sf;
  sp.cell = static_cast<float>(cell);
  sp.half = LOUPE_HALF;
  sp.gridR = static_cast<float>(gridR);
  sp.hair = static_cast<float>(hair);
  G3DLoupe::RenderDisc(px, W, H, sp, texel);

  // readout pill below the loupe: a chip of the sampled color + its hex, on a Surface pill with a
  // soft shadow (colors mirror G3DTheme Surface/Border/Text). The rounded shapes are rasterized with
  // AA here; GDI only lays the anti-aliased glyphs over the pill.
  wchar_t hex[10];
  swprintf(hex, 10, L"#%02X%02X%02X", hp[0], hp[1], hp[2]);
  SetBkMode(mem, TRANSPARENT);
  HFONT font = CreateFontW(-fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
    L"Segoe UI");
  HGDIOBJ oldFont = SelectObject(mem, font);
  // cap height (Segoe UI ~0.72em, matches the rendered all-caps hex) for optical vertical centering.
  // Font-metric APIs proved unreliable here (otmsCapEmHeight under-reports); the ratio matches pixels.
  const int capH = static_cast<int>(fh * 0.72 + 0.5);
  SIZE ts{};
  GetTextExtentPoint32W(mem, hex, static_cast<int>(wcslen(hex)), &ts);
  const int pillW = std::min(W, padX + chip + chipGap + static_cast<int>(ts.cx) + padX);
  const int pillX = centerX - pillW / 2;
  const int pillY = centerY + outer + gap - 1;
  const float pillRR = 7.f * sf;
  const unsigned char shadowCol[4] = { 0, 0, 0, 70 };
  const unsigned char noCol[4] = { 0, 0, 0, 0 };
  const unsigned char pillBg[4] = { 24, 27, 33, 255 };       // G3DTheme::Surface (0x181b21)
  const unsigned char pillBorder[4] = { 255, 255, 255, 23 }; // G3DTheme::Border (white @ 9%)
  G3DLoupe::RenderRoundRect(px, W, H, pillX - 1.f, pillY + 2.f * sf,
    static_cast<float>(pillX + pillW) + 1.f, static_cast<float>(pillY + pillH) + 2.f * sf, pillRR,
    shadowCol, noCol);
  G3DLoupe::RenderRoundRect(px, W, H, static_cast<float>(pillX), static_cast<float>(pillY),
    static_cast<float>(pillX + pillW), static_cast<float>(pillY + pillH), pillRR, pillBg, pillBorder);
  const int chipY = pillY + (pillH - chip) / 2;
  const unsigned char chipBg[4] = { hp[0], hp[1], hp[2], 255 };
  const unsigned char chipBorder[4] = { 255, 255, 255, 46 };
  G3DLoupe::RenderRoundRect(px, W, H, static_cast<float>(pillX + padX), static_cast<float>(chipY),
    static_cast<float>(pillX + padX + chip), static_cast<float>(chipY + chip), 3.f * sf, chipBg,
    chipBorder);

  // GDI blends the anti-aliased glyphs over the opaque pill (premultiplied == straight there). GDI
  // leaves the alpha byte at 0 on glyph pixels, so the text region's opacity is restored below.
  GdiFlush();
  const int textX0 = pillX + padX + chip + chipGap;
  SetTextColor(mem, RGB(235, 235, 235));
  // baseline-align so the caps center on the pill center (matches the vertically-centered chip)
  SetTextAlign(mem, TA_LEFT | TA_BASELINE);
  TextOutW(mem, textX0, pillY + pillH / 2 + capH / 2, hex, static_cast<int>(wcslen(hex)));
  SelectObject(mem, oldFont);
  DeleteObject(font);
  GdiFlush();

  // restore opaque alpha over the text region only (GDI text zeroed the glyph pixels); the chip and
  // the pill's rounded AA corners keep the coverage RenderRoundRect wrote
  for (int y = std::max(0, pillY); y < std::min(H, pillY + pillH); ++y)
  {
    for (int x = std::max(0, textX0); x < std::min(W, pillX + pillW); ++x)
    {
      if (G3DLoupe::RoundRectSDF(x + 0.5f, y + 0.5f, static_cast<float>(pillX),
            static_cast<float>(pillY), static_cast<float>(pillX + pillW),
            static_cast<float>(pillY + pillH), pillRR) < -0.5f)
      {
        px[(static_cast<std::size_t>(y) * W + x) * 4 + 3] = 255;
      }
    }
  }

  // center the disc on the cursor (hidden while sampling), pill hanging below. No trailing offset:
  // the loupe is excluded from capture, so the desktop patch never contains it. Edges clip at the
  // screen boundary, like the native loupe.
  POINT dst{ static_cast<LONG>(cur.x) - centerX, static_cast<LONG>(cur.y) - centerY };
  SIZE size{ W, H };
  POINT srcPt{ 0, 0 };
  BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  UpdateLayeredWindow(LoupeWnd, screen, &dst, &size, mem, &srcPt, 0, &blend, ULW_ALPHA);
  SetWindowPos(LoupeWnd, HWND_TOPMOST, dst.x, dst.y, W, H, SWP_SHOWWINDOW | SWP_NOACTIVATE);

  SelectObject(mem, oldBmp);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
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

  HWND hwnd = renWin != nullptr ? static_cast<HWND>(renWin->GetGenericWindowId()) : nullptr;
  RenderHwnd = hwnd;
  const bool active = !disabled && hwnd != nullptr && G3DWidgets::EyedropperActive();

  // the fullscreen input shield + loupe live only while sampling in the foreground window: a
  // background / offscreen window must never cover the user's screen or reroute input (sampling
  // stays armed then — the viewport source and the patch below keep working through normal
  // in-window mouse moves)
  const bool foreground = active && GetForegroundWindow() == hwnd;
  static bool prevActive = false;
  static bool prevForeground = false;
  if (active != prevActive || foreground != prevForeground)
  {
    G3DWidgets::Trace("[Trace][cp.eyed] sampler armed=%d fg=%d overlays=%d", active ? 1 : 0,
      foreground ? 1 : 0, OverlaysShown ? 1 : 0);
    prevActive = active;
    prevForeground = foreground;
  }
  if (foreground && !OverlaysShown)
  {
    ShowOverlays();
  }
  else if (!foreground && OverlaysShown)
  {
    HideOverlays();
  }
  // Freeze the screen once per arming: the centered loupe reads its desktop patch from this snapshot
  // instead of a live grab, so it never samples (and magnifies) itself. A few ticks of delay let the
  // color-picker popup finish closing so it is not frozen into the snapshot. The in-viewport scene
  // source stays live (submitted separately by the render pass). Track the arm edge before the
  // early-out below so a disarm resets it (else a re-arm would skip the delay).
  static bool prevArmed = false;
  static int armTicks = 0;
  const bool armRising = active && !prevArmed;
  prevArmed = active;

  if (!active)
  {
    // disarmed: drop the frozen snapshot so the next arming re-captures a fresh screen
    SnapValid = false;
    ScreenSnapshot.clear();
    ScreenSnapshot.shrink_to_fit();
    return;
  }
  if (armRising)
  {
    SnapValid = false;
    armTicks = 0;
  }
  if (foreground && !SnapValid && ++armTicks >= 3)
  {
    SnapValid = CaptureVirtualScreen();
    G3DWidgets::Trace("[Trace][cp.eyed] sampler froze screen snapshot valid=%d %dx%d",
      SnapValid ? 1 : 0, SnapW, SnapH);
  }

  // the desktop patch feed reads the frozen snapshot around the real cursor (empty until the snapshot
  // is ready, a few ticks after arming — the in-viewport scene source works immediately regardless)
  POINT cur;
  if (!GetCursorPos(&cur))
  {
    G3DWidgets::Trace("[Trace][cp.eyed] sampler GetCursorPos FAIL");
    return;
  }
  std::vector<unsigned char> rgba;
  int w = 0;
  int h = 0;
  int x0 = 0;
  int y0 = 0;
  if (!ExtractPatchFromSnapshot(cur, rgba, w, h, x0, y0))
  {
    return; // snapshot not ready yet (still within the arm delay), or cursor off the snapshot
  }

  // OS loupe first (it reads the patch), then hand the pixels to the widget library
  if (OverlaysShown)
  {
    UpdateLoupe(hwnd, cur, rgba, w, h, x0, y0);
  }

  // patch origin: virtual-screen -> window client px (== ImGui display px, top-left origin; the
  // process is per-monitor DPI aware so both sides are physical pixels)
  POINT origin{ x0, y0 };
  ScreenToClient(hwnd, &origin);
  G3DWidgets::SubmitEyedropperScreenPatch(std::move(rgba), w, h, origin.x, origin.y);
  static int tick = 0;
  if (++tick % 20 == 0)
  {
    G3DWidgets::Trace("[Trace][cp.eyed] sampler cur=(%ld,%ld) origin=(%ld,%ld) %dx%d ov=%d",
      cur.x, cur.y, origin.x, origin.y, w, h, OverlaysShown ? 1 : 0);
  }
}

#else

//----------------------------------------------------------------------------
void G3DScreenSampler::Update(vtkRenderWindow*)
{
  // no desktop feed on this platform: the eyedropper samples the viewport rect only
}

#endif
