#include "G3DWindowGeometry.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
// Avoid the windows.h min/max macros clobbering std::min/std::max/std::clamp.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace g3d::window_geometry
{
//----------------------------------------------------------------------------
Rect centerInWorkArea(const Rect& workArea, int width, int height)
{
  const int x = workArea.x + (workArea.width - width) / 2;
  const int y = workArea.y + (workArea.height - height) / 2;
  return Rect{ x, y, width, height };
}

//----------------------------------------------------------------------------
Rect defaultCenteredGeometry(
  const Rect& workArea, int minWidth, int minHeight, int frameWidth, int frameHeight)
{
  // Render client size: 70% of the work area on each axis, clamped to
  // [min size, work area size]. Guard against a work area smaller than the minimum
  // size (tiny/odd displays): std::clamp requires lo <= hi, so cap the upper bound
  // at max(workArea, min).
  const int maxWidth = std::max(workArea.width, minWidth);
  const int maxHeight = std::max(workArea.height, minHeight);
  const int clientWidth =
    std::clamp(static_cast<int>(workArea.width * 0.7), std::min(minWidth, maxWidth), maxWidth);
  const int clientHeight =
    std::clamp(static_cast<int>(workArea.height * 0.7), std::min(minHeight, maxHeight), maxHeight);

  // Center the *outer* window (client + non-client frame) inside the work area so
  // the visible window (what GetWindowRect measures) is centered, not just the
  // render area. The returned width/height stay the client size for setSize().
  const Rect outer = centerInWorkArea(workArea, clientWidth + frameWidth, clientHeight + frameHeight);
  return Rect{ outer.x, outer.y, clientWidth, clientHeight };
}

//----------------------------------------------------------------------------
std::optional<std::pair<int, int>> nonClientFrameSize(double dpiScale)
{
#ifdef _WIN32
  // Ask Windows for the frame a standard resizable top-level window (the style
  // VTK's Win32 render window uses) adds around a zero-sized client, at the given
  // DPI. AdjustWindowRectExForDpi (Win10 1607+) keeps the metrics correct on
  // high-DPI monitors.
  const UINT dpi = static_cast<UINT>(std::lround(96.0 * dpiScale));
  RECT r{ 0, 0, 0, 0 };
  if (::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi) != 0)
  {
    return std::make_pair(static_cast<int>(r.right - r.left), static_cast<int>(r.bottom - r.top));
  }
  return std::nullopt;
#else
  (void)dpiScale;
  return std::nullopt;
#endif
}

//----------------------------------------------------------------------------
// The Windows implementation of cursorMonitorWorkArea() lives here. macOS has a
// dedicated Objective-C++ translation unit (G3DWindowGeometryCocoa.mm), so this
// definition is skipped on Apple to avoid a duplicate symbol; every other
// platform falls back to std::nullopt.
#if !defined(__APPLE__)
std::optional<Rect> cursorMonitorWorkArea()
{
#ifdef _WIN32
  POINT cursor{ 0, 0 };
  HMONITOR monitor = nullptr;
  if (::GetCursorPos(&cursor) != 0)
  {
    monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  }
  if (monitor == nullptr)
  {
    // Cursor position unavailable: fall back to the primary monitor.
    monitor = ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
  }
  if (monitor == nullptr)
  {
    return std::nullopt;
  }

  MONITORINFO info{};
  info.cbSize = sizeof(MONITORINFO);
  if (::GetMonitorInfoW(monitor, &info) == 0)
  {
    return std::nullopt;
  }

  const RECT& work = info.rcWork;
  return Rect{ static_cast<int>(work.left), static_cast<int>(work.top),
    static_cast<int>(work.right - work.left), static_cast<int>(work.bottom - work.top) };
#else
  // No portable monitor enumeration; caller keeps the previous VTK default.
  return std::nullopt;
#endif
}
#endif // !__APPLE__
}
