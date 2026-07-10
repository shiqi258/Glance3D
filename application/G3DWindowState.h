#ifndef G3DWindowState_h
#define G3DWindowState_h

/**
 * @namespace g3d::window_state
 * @brief Cross-session persistence of the desktop window geometry (US-007).
 *
 * These helpers live in the application layer (not the shared libf3d core): they
 * own a per-user state file and validate a saved rect against the current monitor
 * layout, both of which only make sense for the desktop app. The save/restore
 * *policy* (debounce, suppression rules, exit flush) lives in F3DStarter; this
 * module provides the pure / IO building blocks:
 *  - resolve the state file path (with the G3D_WINDOW_STATE env override/disable),
 *  - atomic JSON load/save of window-state.json,
 *  - a validity check + size clamp of a saved rect against monitor work areas.
 *
 * All geometry is stored in physical pixels (no cross-session DPI conversion; the
 * validity check is the safety net when displays/DPI change, see FR-12/FR-17).
 */

#include "G3DWindowGeometry.h" // g3d::window_geometry::Rect

#include <filesystem>
#include <optional>
#include <vector>

namespace g3d::window_state
{
/**
 * Persisted window geometry, mirroring window-state.json (physical pixels). x/y is
 * the top-left of the outer window and width/height its render client size, in the
 * same convention as f3d::window setPosition/getPosition and setSize/getWidth.
 * When maximized is true, x/y/width/height carry the *normal* (restore) rect.
 */
struct WindowState
{
  int version = 1;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool maximized = false;
};

[[nodiscard]] bool operator==(const WindowState& a, const WindowState& b);
[[nodiscard]] inline bool operator!=(const WindowState& a, const WindowState& b)
{
  return !(a == b);
}

/** Outcome of load(). */
enum class LoadStatus
{
  Missing, ///< No state file yet (fresh user) - not an error.
  Corrupt, ///< File exists but could not be parsed / has invalid fields.
  Ok       ///< Parsed successfully.
};

struct LoadResult
{
  LoadStatus status = LoadStatus::Missing;
  WindowState state;
};

/**
 * Resolve the window-state file path.
 * - G3D_WINDOW_STATE=0      -> std::nullopt (feature disabled).
 * - G3D_WINDOW_STATE=<path> -> that path (test redirect / relocation).
 * - unset/empty             -> <state dir>/window-state.json, where the state dir is
 *   Windows: %LOCALAPPDATA%\Glance3D (same root as the logs); Linux: $XDG_STATE_HOME
 *   or ~/.local/state/Glance3D; macOS: ~/Library/Application Support/Glance3D.
 * Returns std::nullopt when disabled or the directory cannot be resolved.
 */
[[nodiscard]] std::optional<std::filesystem::path> resolveStateFilePath();

/** Read and parse the state file. Never throws. */
[[nodiscard]] LoadResult load(const std::filesystem::path& path);

/**
 * Atomically write the state file (sibling temp file + rename over the target).
 * Creates the parent directory if needed. Returns false on any IO failure.
 */
[[nodiscard]] bool save(const std::filesystem::path& path, const WindowState& state);

/**
 * Validate a saved rect against the given monitor work areas (physical px, top-left
 * origin): requires at least a 100x100 intersection with some monitor and a
 * reachable title bar, and clamps the size to that monitor's work area (FR-12).
 * Returns the (possibly clamped) state, or std::nullopt when it cannot be made
 * valid (caller then falls back to the default centered geometry). An empty
 * workAreas list yields std::nullopt (fail closed).
 */
[[nodiscard]] std::optional<WindowState> validate(
  const WindowState& state, const std::vector<g3d::window_geometry::Rect>& workAreas);
}

#endif
