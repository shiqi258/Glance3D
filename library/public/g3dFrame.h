#ifndef g3d_frame_h
#define g3d_frame_h

#include "export.h"
#include "image.h"

namespace f3d
{
class window;
}

namespace g3d
{
/**
 * @class   frame
 * @brief   Glance3D view of what a window actually put on screen.
 *
 * f3d::window::renderToImage renders a fresh frame before reading it back, so it always shows what
 * the scene *should* look like. That hides the one failure an on-demand viewer is prone to: a frame
 * that reached the screen without the latest change, because nothing asked for the full render the
 * change needed and the viewer only redrew its UI over the previous 3D image.
 *
 * Reading back the presented frame instead is what lets a test catch that, the way the user would.
 */
class F3D_EXPORT frame
{
public:
  /**
   * The frame last presented by @p win, read back without rendering anything.
   * Always RGB (no transparent background); same row order as window::renderToImage.
   */
  static f3d::image presented(f3d::window& win);
};
}

#endif
