#ifndef G3DWindowGeometry_h
#define G3DWindowGeometry_h

/**
 * @namespace g3d::window_geometry
 * @brief Applicative helpers to compute the default startup window geometry.
 *
 * These live in the application layer (not the shared libf3d core) because they
 * rely on platform window-system queries (monitor enumeration / work areas) that
 * only make sense for the desktop app. All platform specific code is guarded by
 * platform macros; unsupported platforms degrade gracefully (return std::nullopt)
 * so callers keep the previous behavior instead of crashing.
 */

#include <optional>
#include <utility>
#include <vector>

namespace g3d::window_geometry
{
/**
 * A screen rectangle in physical pixels, top-left origin. This matches the Win32
 * convention and the `f3d::window` setPosition/getPosition convention (which flips
 * the Cocoa bottom-left origin internally), so a Rect produced here can be fed to
 * `window.setPosition` / `window.setSize` directly.
 */
struct Rect
{
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

/**
 * Return the work area (the desktop area excluding the taskbar / dock / menu bar)
 * of the monitor currently under the mouse cursor, in physical pixels with a
 * top-left origin. Falls back to the primary monitor when the cursor monitor
 * cannot be resolved.
 *
 * - Windows: Win32 MonitorFromPoint / GetMonitorInfo (real, tested).
 * - macOS: NSScreen visibleFrame of the screen under the cursor (see the .mm;
 *   review-only, compiled on macOS only).
 * - Other platforms: std::nullopt (no enumeration; caller keeps VTK defaults).
 *
 * Never throws; any query failure yields std::nullopt.
 */
[[nodiscard]] std::optional<Rect> cursorMonitorWorkArea();

/**
 * Return the work area of every connected monitor, in physical pixels with a
 * top-left origin. Used to validate a restored window rect against the current
 * display layout (US-007): a saved rect is only usable if it still intersects
 * some monitor's work area.
 *
 * - Windows: Win32 EnumDisplayMonitors / GetMonitorInfo (real, tested).
 * - Other platforms: an empty vector (no enumeration; caller then falls back to
 *   the default centered geometry instead of restoring).
 *
 * Never throws.
 */
[[nodiscard]] std::vector<Rect> allMonitorWorkAreas();

/**
 * Center a window of the given size inside a work area, returning the top-left
 * position (physical pixels, top-left origin). If the window is larger than the
 * work area on an axis the offset is negative (window overhangs), which is the
 * expected "min size wins on a tiny display" degradation.
 */
[[nodiscard]] Rect centerInWorkArea(const Rect& workArea, int width, int height);

/**
 * The size (physical pixels) added by the non-client window frame (title bar +
 * borders) for a standard resizable top-level window at the given DPI scale.
 * Returned as {frameWidth, frameHeight}. Used to center the *outer* window rect
 * (what GetWindowRect measures) rather than just the render client area, so the
 * visible window is truly centered. Windows only; std::nullopt elsewhere (callers
 * then center the client area as a best-effort).
 */
[[nodiscard]] std::optional<std::pair<int, int>> nonClientFrameSize(double dpiScale);

/**
 * Compute the default interactive startup geometry.
 *
 * The render client size = 70% of the work area (each axis independently),
 * clamped to [minWidth x minHeight, work area size]; minWidth/minHeight are
 * expected to already include DPI scaling (e.g. 1000*dpiScale x 600*dpiScale).
 *
 * frameWidth/frameHeight describe the non-client frame (see nonClientFrameSize);
 * the returned Rect's x/y center the *outer* window (client + frame) inside the
 * work area, while width/height carry the *client* size to pass to setSize().
 * Pass frameWidth=frameHeight=0 to center the client area directly.
 */
[[nodiscard]] Rect defaultCenteredGeometry(
  const Rect& workArea, int minWidth, int minHeight, int frameWidth, int frameHeight);
}

#endif
