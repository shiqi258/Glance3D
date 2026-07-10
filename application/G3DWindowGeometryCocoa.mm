#include "G3DWindowGeometry.h"

#import <Cocoa/Cocoa.h>

// macOS implementation of cursorMonitorWorkArea(). Compiled only on Apple (see
// application/CMakeLists.txt); the cross-platform G3DWindowGeometry.cxx skips its
// own definition on __APPLE__ to avoid a duplicate symbol.
//
// NOTE: this translation unit is not built on the Windows host used to author the
// change, so it is verified by code review only (see US-006 notes). The logic is
// standard NSScreen usage kept intentionally minimal.
namespace g3d::window_geometry
{
std::optional<Rect> cursorMonitorWorkArea()
{
  @autoreleasepool
  {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (screens.count == 0)
    {
      return std::nullopt;
    }

    // Mouse location is in the global (bottom-left origin) coordinate space.
    const NSPoint mouse = [NSEvent mouseLocation];

    NSScreen* target = nil;
    for (NSScreen* screen in screens)
    {
      if (NSMouseInRect(mouse, [screen frame], NO))
      {
        target = screen;
        break;
      }
    }
    if (target == nil)
    {
      target = [NSScreen mainScreen];
    }
    if (target == nil)
    {
      target = [screens objectAtIndex:0];
    }

    // visibleFrame excludes the menu bar and the Dock (i.e. the work area).
    const NSRect visible = [target visibleFrame];

    // Convert from Cocoa bottom-left origin to the top-left origin convention that
    // f3d::window::setPosition expects. window_impl flips the Y against the primary
    // screen height (RenWin->GetScreenSize()), so mirror that here using screens[0].
    const CGFloat primaryHeight = [[screens objectAtIndex:0] frame].size.height;
    const int topLeftY =
      static_cast<int>(primaryHeight - (visible.origin.y + visible.size.height));

    return Rect{ static_cast<int>(visible.origin.x), topLeftY,
      static_cast<int>(visible.size.width), static_cast<int>(visible.size.height) };
  }
}
}
