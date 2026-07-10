#ifndef f3d_window_h
#define f3d_window_h

#include "camera.h"
#include "export.h"
#include "image.h"

/// @cond
#include <array>
#include <string>
/// @endcond

namespace f3d
{
/**
 * @class   window
 * @brief   Abstract class to render in a window or an image
 *
 * A class to render things in a window or an image.
 * An icon and windowName can be set which can be shown by a window manager.
 */
class F3D_EXPORT window
{
public:
  /**
   * Enumeration of supported window types
   * - NONE: A mock window without rendering capabilities.
   * - EXTERNAL: An external window that assume the OpenGL context would have been created by
   * another framework.
   * - GLX: A window using GLX.
   * - WGL: A window using WGL.
   * - COCOA: A window using COCOA.
   * - EGL: An offscreen window using hardware acceleration that can run headless.
   * - OSMESA: An offscreen window using software rendering that is always run headless.
   * - WASM: A webassembly window (when libf3d is built with emscripten).
   * - UNKNOWN: An unknown window (should not happen).
   */
  enum class Type : unsigned char
  {
    NONE,
    EXTERNAL,
    GLX,
    WGL,
    COCOA,
    EGL,
    OSMESA,
    WASM,
    UNKNOWN
  };

  /**
   * Get the type of the window.
   */
  [[nodiscard]] virtual Type getType() = 0;

  /**
   * Is the window offscreen.
   */
  [[nodiscard]] virtual bool isOffscreen() = 0;

  /**
   * Get the camera provided by the window.
   */
  [[nodiscard]] virtual camera& getCamera() = 0;

  /**
   * Perform a render of the window to the screen.
   * All dynamic options are updated if needed.
   * Return true on success, false otherwise.
   */
  virtual bool render() = 0;

  /**
   * Perform a render of the window to the screen and save the result in a f3d::image.
   * of ChannelType BYTE and 3 or 4 components (RGB or RGBA).
   * Set noBackground to true to have a transparent background.
   * Returns the resulting f3d::image.
   */
  [[nodiscard]] virtual image renderToImage(bool noBackground = false) = 0;

  /**
   * Set the size of the window.
   */
  virtual window& setSize(int width, int height) = 0;

  /**
   * Get the width of the window.
   */
  [[nodiscard]] virtual int getWidth() const = 0;

  /**
   * Get the height of the window.
   */
  [[nodiscard]] virtual int getHeight() const = 0;

  /**
   * Set the position of the window.
   */
  virtual window& setPosition(int x, int y) = 0;

  /**
   * Get the position of the window as a `{ x, y }` array, in the same
   * top-left-origin screen coordinate convention accepted by setPosition().
   *
   * For on-screen windows this reflects the current placement. For offscreen,
   * mock (NONE) and WASM windows that have no real on-screen placement, this
   * returns the last value passed to setPosition() (or `{ 0, 0 }` if it was
   * never called). Never throws.
   */
  [[nodiscard]] virtual std::array<int, 2> getPosition() const = 0;

  /**
   * Get whether the window is currently maximized.
   *
   * Only meaningful on Windows, where it is queried through the native window
   * handle. Always returns false on other platforms, for offscreen/mock/WASM
   * windows, and whenever no native window handle is available. Never throws.
   */
  [[nodiscard]] virtual bool isMaximized() const = 0;

  /**
   * Request the window to be maximized (true) or restored to its normal rect
   * (false). To restore a maximized window onto the correct monitor, first set
   * the normal size/position with setSize()/setPosition(), then call
   * setMaximized(true).
   *
   * Only effective on Windows, where it is applied through the native window
   * handle. A no-op on other platforms, for offscreen/mock/WASM windows, and
   * whenever no native window handle is available. Never throws.
   */
  virtual window& setMaximized(bool maximized) = 0;

  /**
   * Set the icon to be shown by a window manager.
   * icon should be an unsigned char array.
   * iconSize should be the sizeof(icon).
   */
  virtual window& setIcon(const unsigned char* icon, size_t iconSize) = 0;

  /**
   * Set the window name to be shown by a window manager.
   */
  virtual window& setWindowName(std::string_view windowName) = 0;

  /**
   * Set the state of the centered loading overlay drawn over the viewport.
   * When visible is true, a branded loading indicator is shown; progress is clamped to [0, 1]
   * and message is the status line displayed below it. Call with visible=false to hide it.
   * Intended to be driven each frame while polling an asynchronous scene load (scene::addAsync).
   * Has no visible effect when the UI module (F3D_MODULE_UI) is disabled.
   */
  virtual window& setLoadingState(bool visible, double progress, const std::string& message) = 0;

  /**
   * Convert a point in display coordinate to world coordinate.
   */
  [[nodiscard]] virtual point3_t getWorldFromDisplay(const point3_t& displayPoint) const = 0;

  /**
   * Convert a point in world coordinate to display coordinate.
   */
  [[nodiscard]] virtual point3_t getDisplayFromWorld(const point3_t& worldPoint) const = 0;

protected:
  //! @cond
  window() = default;
  virtual ~window() = default;
  window(const window&) = delete;
  window(window&&) = delete;
  window& operator=(const window&) = delete;
  window& operator=(window&&) = delete;
  //! @endcond
};
}

#endif
