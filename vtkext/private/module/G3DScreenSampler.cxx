#include "G3DScreenSampler.h"

#include "G3DWidgets.h"

#include <vtkRenderWindow.h>

#ifdef _WIN32
#include <vtkWindows.h>

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

//----------------------------------------------------------------------------
LRESULT CALLBACK G3DEyedropInputProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE; // keyboard focus (Esc to cancel) stays on the render window
    case WM_SETCURSOR:
      SetCursor(LoadCursor(nullptr, IDC_CROSS));
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
  ic.hCursor = LoadCursor(nullptr, IDC_CROSS);
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

//----------------------------------------------------------------------------
// Paint + place the OS loupe near the cursor while it roams beyond the render window: an 11x11
// zoomed texel grid in a ring of the hovered color with a hex readout, visually mirroring the
// ImGui loupe (which owns the in-window presentation). Drawn with plain GDI into a 32bpp DIB,
// alpha resolved geometrically (circle + pill), shown via UpdateLayeredWindow. The loupe trails at
// a diagonal offset flipped/clamped to the cursor's monitor — the desktop patch shows the loupe as
// last presented, so its drawing must stay clear of the sampled cursor neighborhood.
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
  const int ringW = std::max(3, static_cast<int>(4.0 * s + 0.5));
  const int hair = std::max(1, static_cast<int>(1.25 * s + 0.5));
  const int outer = gridR + ringW + hair;
  const int fh = std::max(11, static_cast<int>(14.0 * s + 0.5));
  const int padX = static_cast<int>(8.0 * s + 0.5);
  const int padY = static_cast<int>(4.0 * s + 0.5);
  const int gap = static_cast<int>(8.0 * s + 0.5);
  const int W = outer * 2 + 2;
  const int pillH = fh + padY * 2;
  const int H = outer * 2 + gap + pillH + 2;
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

  // zoomed texel grid, masked cell-by-cell to the circle (direct BGRA writes)
  for (int gy = -LOUPE_HALF; gy <= LOUPE_HALF; ++gy)
  {
    for (int gx = -LOUPE_HALF; gx <= LOUPE_HALF; ++gx)
    {
      if (gx * gx + gy * gy > LOUPE_HALF * LOUPE_HALF + LOUPE_HALF)
      {
        continue;
      }
      const unsigned char* t = texel(gx, gy);
      const int rx0 = centerX + gx * cell - cell / 2;
      const int ry0 = centerY + gy * cell - cell / 2;
      for (int y = std::max(0, ry0); y < std::min(H, ry0 + cell); ++y)
      {
        for (int x = std::max(0, rx0); x < std::min(W, rx0 + cell); ++x)
        {
          unsigned char* d = &px[(static_cast<std::size_t>(y) * W + x) * 4];
          d[0] = t[2];
          d[1] = t[1];
          d[2] = t[0];
          d[3] = 255;
        }
      }
    }
  }

  // GDI pass: center texel marker, ring (white inner / hovered-color band / dark outer), pill
  GdiFlush();
  SetBkMode(mem, TRANSPARENT);
  HGDIOBJ nullBrush = GetStockObject(NULL_BRUSH);
  auto circle = [&](int r, COLORREF col, int width)
  {
    HPEN pen = CreatePen(PS_SOLID, width, col);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HGDIOBJ oldBr = SelectObject(mem, nullBrush);
    Ellipse(mem, centerX - r, centerY - r, centerX + r + 1, centerY + r + 1);
    SelectObject(mem, oldBr);
    SelectObject(mem, oldPen);
    DeleteObject(pen);
  };
  auto box = [&](int half, COLORREF col)
  {
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HGDIOBJ oldBr = SelectObject(mem, nullBrush);
    Rectangle(mem, centerX - half, centerY - half, centerX + half + 1, centerY + half + 1);
    SelectObject(mem, oldBr);
    SelectObject(mem, oldPen);
    DeleteObject(pen);
  };
  box(cell / 2 + 1, RGB(20, 20, 20));
  box(cell / 2, RGB(255, 255, 255));
  circle(gridR, RGB(255, 255, 255), hair);
  circle(gridR + ringW / 2, RGB(hp[0], hp[1], hp[2]), ringW);
  circle(gridR + ringW, RGB(20, 20, 20), hair);

  // hex readout pill under the loupe (colors mirror G3DTheme Surface/Border/Text)
  wchar_t hex[10];
  swprintf(hex, 10, L"#%02X%02X%02X", hp[0], hp[1], hp[2]);
  HFONT font = CreateFontW(-fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
    L"Segoe UI");
  HGDIOBJ oldFont = SelectObject(mem, font);
  SIZE ts{};
  GetTextExtentPoint32W(mem, hex, static_cast<int>(wcslen(hex)), &ts);
  const int pillW = std::min(W, static_cast<int>(ts.cx) + padX * 2);
  const int pillX = centerX - pillW / 2;
  const int pillY = centerY + outer + gap - 1;
  {
    HBRUSH bg = CreateSolidBrush(RGB(24, 27, 33));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(46, 49, 56));
    HGDIOBJ oldBr = SelectObject(mem, bg);
    HGDIOBJ oldPen = SelectObject(mem, border);
    const int rr = static_cast<int>(4.0 * s + 0.5) * 2;
    RoundRect(mem, pillX, pillY, pillX + pillW, pillY + pillH, rr, rr);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBr);
    DeleteObject(border);
    DeleteObject(bg);
  }
  SetTextColor(mem, RGB(235, 235, 235));
  TextOutW(mem, centerX - ts.cx / 2, pillY + padY, hex, static_cast<int>(wcslen(hex)));
  SelectObject(mem, oldFont);
  DeleteObject(font);
  GdiFlush();

  // geometric alpha: opaque inside the circle and the pill, fully transparent elsewhere (GDI
  // leaves the alpha byte at 0, so it is resolved here in one pass)
  const double outerR2 = static_cast<double>(outer + 1) * (outer + 1);
  for (int y = 0; y < H; ++y)
  {
    for (int x = 0; x < W; ++x)
    {
      unsigned char* d = &px[(static_cast<std::size_t>(y) * W + x) * 4];
      const double ddx = x - centerX + 0.5;
      const double ddy = y - centerY + 0.5;
      const bool inCircle = ddx * ddx + ddy * ddy <= outerR2;
      const bool inPill = x >= pillX && x < pillX + pillW && y >= pillY && y < pillY + pillH;
      if (inCircle || inPill)
      {
        d[3] = 255;
      }
      else
      {
        d[0] = d[1] = d[2] = d[3] = 0;
      }
    }
  }

  // trail diagonally, flipped then clamped to the cursor's monitor, clear of the sampled texels
  HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  const int margin = static_cast<int>(6.0 * s + 0.5);
  const int diag = static_cast<int>((gridR + ringW + 24.0 * s) * 0.7071 + 0.5);
  int ax = cur.x + diag + (W - centerX) + margin <= mi.rcMonitor.right ? cur.x + diag
                                                                        : cur.x - diag;
  int ay = cur.y + diag + (H - centerY) + margin <= mi.rcMonitor.bottom ? cur.y + diag
                                                                         : cur.y - diag;
  ax = std::clamp(ax, static_cast<int>(mi.rcMonitor.left) + centerX + margin,
    std::max(static_cast<int>(mi.rcMonitor.left) + centerX + margin,
      static_cast<int>(mi.rcMonitor.right) - (W - centerX) - margin));
  ay = std::clamp(ay, static_cast<int>(mi.rcMonitor.top) + centerY + margin,
    std::max(static_cast<int>(mi.rcMonitor.top) + centerY + margin,
      static_cast<int>(mi.rcMonitor.bottom) - (H - centerY) - margin));

  POINT dst{ ax - centerX, ay - centerY };
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
  if (!active)
  {
    return;
  }

  // the desktop patch feed runs regardless of focus: it only reads the screen around the real
  // cursor, so hovering a not-focused window still samples correctly through in-window moves
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
  if (!CaptureDesktopPatch(cur, rgba, w, h, x0, y0))
  {
    G3DWidgets::Trace("[Trace][cp.eyed] sampler patch FAIL cur=(%ld,%ld)", cur.x, cur.y);
    return;
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
