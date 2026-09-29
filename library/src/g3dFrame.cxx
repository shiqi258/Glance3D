#include "g3dFrame.h"

#include "window_impl.h"

#include "vtkF3DRenderer.h"

namespace g3d
{
//----------------------------------------------------------------------------
f3d::image frame::presented(f3d::window& win)
{
  // Every f3d::window is a window_impl; the facade exists so the application (and bindings) can
  // reach this without the upstream window interface growing a Glance3D-only virtual.
  return static_cast<f3d::detail::window_impl&>(win).CapturePresentedImage();
}

//----------------------------------------------------------------------------
frame::stats frame::renderStats(f3d::window& win)
{
  const vtkF3DRenderer::G3DRenderStats& s =
    static_cast<f3d::detail::window_impl&>(win).GetRenderer()->GetG3DRenderStats();
  return { s.full, s.uiOnly, s.upgraded };
}
}
