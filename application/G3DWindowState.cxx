#include "G3DWindowState.h"

#include "utils.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace g3d::window_state
{
namespace
{
//----------------------------------------------------------------------------
// Per-user directory that physically holds window-state.json. On Windows this is
// the same root as the log files (%LOCALAPPDATA%\Glance3D). On Linux it follows
// the XDG *state* base dir (state, not cache/config); on macOS the Application
// Support dir. Returns an empty path when it cannot be resolved (the feature then
// stays disabled rather than crashing).
fs::path stateDirectory()
{
  const std::string app = "Glance3D";
#if defined(_WIN32)
  const std::optional<std::string> localAppData =
    f3d::utils::getKnownFolder(f3d::utils::KnownFolder::LOCALAPPDATA);
  if (!localAppData.has_value() || localAppData.value().empty())
  {
    return {};
  }
  return fs::path(localAppData.value()) / app;
#elif defined(__APPLE__)
  const std::optional<std::string> home = f3d::utils::getEnv("HOME");
  if (!home.has_value() || home.value().empty())
  {
    return {};
  }
  return fs::path(home.value()) / "Library" / "Application Support" / app;
#elif defined(__unix__)
  const std::optional<std::string> xdgState = f3d::utils::getEnv("XDG_STATE_HOME");
  if (xdgState.has_value() && !xdgState.value().empty())
  {
    return fs::path(xdgState.value()) / app;
  }
  const std::optional<std::string> home = f3d::utils::getEnv("HOME");
  if (!home.has_value() || home.value().empty())
  {
    return {};
  }
  return fs::path(home.value()) / ".local" / "state" / app;
#else
  return {};
#endif
}

//----------------------------------------------------------------------------
// Intersection of two rects; a non-positive width or height means no overlap.
g3d::window_geometry::Rect intersect(
  const g3d::window_geometry::Rect& a, const g3d::window_geometry::Rect& b)
{
  const int left = std::max(a.x, b.x);
  const int top = std::max(a.y, b.y);
  const int right = std::min(a.x + a.width, b.x + b.width);
  const int bottom = std::min(a.y + a.height, b.y + b.height);
  return g3d::window_geometry::Rect{ left, top, right - left, bottom - top };
}
}

//----------------------------------------------------------------------------
bool operator==(const WindowState& a, const WindowState& b)
{
  return a.version == b.version && a.x == b.x && a.y == b.y && a.width == b.width &&
    a.height == b.height && a.maximized == b.maximized;
}

//----------------------------------------------------------------------------
std::optional<fs::path> resolveStateFilePath()
{
  const std::optional<std::string> env = f3d::utils::getEnv("G3D_WINDOW_STATE");
  if (env.has_value())
  {
    if (env.value() == "0")
    {
      return std::nullopt; // explicitly disabled
    }
    if (!env.value().empty())
    {
      return fs::path(env.value()); // redirect to an explicit path (tests / relocation)
    }
  }
  const fs::path dir = stateDirectory();
  if (dir.empty())
  {
    return std::nullopt;
  }
  return dir / "window-state.json";
}

//----------------------------------------------------------------------------
LoadResult load(const fs::path& path)
{
  std::error_code ec;
  if (!fs::exists(path, ec))
  {
    return { LoadStatus::Missing, {} };
  }
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
  {
    return { LoadStatus::Missing, {} };
  }
  try
  {
    nlohmann::json json;
    in >> json;
    WindowState state;
    state.version = json.at("version").get<int>();
    state.x = json.at("x").get<int>();
    state.y = json.at("y").get<int>();
    state.width = json.at("width").get<int>();
    state.height = json.at("height").get<int>();
    state.maximized = json.at("maximized").get<bool>();
    // Only version 1 is understood; a positive size is required. Anything else is
    // treated as corrupt so the caller warns and falls back to the default.
    if (state.version != 1 || state.width <= 0 || state.height <= 0)
    {
      return { LoadStatus::Corrupt, {} };
    }
    return { LoadStatus::Ok, state };
  }
  catch (const std::exception&)
  {
    return { LoadStatus::Corrupt, {} };
  }
}

//----------------------------------------------------------------------------
bool save(const fs::path& path, const WindowState& state)
{
  try
  {
    if (path.has_parent_path())
    {
      fs::create_directories(path.parent_path());
    }

    nlohmann::ordered_json json;
    json["version"] = state.version;
    json["x"] = state.x;
    json["y"] = state.y;
    json["width"] = state.width;
    json["height"] = state.height;
    json["maximized"] = state.maximized;

    // Atomic write: dump to a sibling temp file, flush, then rename over the target
    // so a crash mid-write never leaves a truncated window-state.json.
    fs::path tmp = path;
    tmp += ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out.is_open())
      {
        return false;
      }
      out << json.dump(2);
      out.flush();
      if (!out.good())
      {
        return false;
      }
    }
    fs::rename(tmp, path); // replaces an existing file on both Windows and POSIX
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}

//----------------------------------------------------------------------------
std::optional<WindowState> validate(
  const WindowState& state, const std::vector<g3d::window_geometry::Rect>& workAreas)
{
  if (workAreas.empty())
  {
    return std::nullopt; // no monitor enumeration: fail closed, keep default geometry
  }

  const g3d::window_geometry::Rect windowRect{ state.x, state.y, state.width, state.height };

  // Pick the monitor with the largest intersection area with the saved window.
  const g3d::window_geometry::Rect* best = nullptr;
  long long bestArea = -1;
  g3d::window_geometry::Rect bestInter{};
  for (const auto& work : workAreas)
  {
    const g3d::window_geometry::Rect inter = intersect(windowRect, work);
    if (inter.width > 0 && inter.height > 0)
    {
      const long long area = static_cast<long long>(inter.width) * inter.height;
      if (area > bestArea)
      {
        bestArea = area;
        best = &work;
        bestInter = inter;
      }
    }
  }

  // Require a sizable visible overlap so the window can actually be found and grabbed.
  // A wildly off-screen rect (e.g. x=-99999) produces no overlap and is rejected here.
  constexpr int kMinVisible = 100;
  if (best == nullptr || bestInter.width < kMinVisible || bestInter.height < kMinVisible)
  {
    return std::nullopt;
  }

  // Title bar reachability: the top edge must sit within the target work area. A
  // small tolerance above it is allowed for maximized/snapped window borders; it
  // must not be so low that the title bar falls past the bottom of the work area.
  constexpr int kTopTolerance = 40;
  constexpr int kMinTitleBar = 30;
  if (state.y < best->y - kTopTolerance || state.y > best->y + best->height - kMinTitleBar)
  {
    return std::nullopt;
  }

  // Clamp the size to the target monitor work area (FR-12).
  WindowState out = state;
  out.width = std::min(state.width, best->width);
  out.height = std::min(state.height, best->height);
  return out;
}
}
