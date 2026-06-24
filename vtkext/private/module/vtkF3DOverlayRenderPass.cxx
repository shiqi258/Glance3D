#include "vtkF3DOverlayRenderPass.h"

#include <vtkCameraPass.h>
#include <vtkDefaultPass.h>
#include <vtkObjectFactory.h>
#include <vtkOpenGLError.h>
#include <vtkOpenGLFramebufferObject.h>
#include <vtkOpenGLQuadHelper.h>
#include <vtkOpenGLRenderUtilities.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkOpenGLShaderCache.h>
#include <vtkOpenGLState.h>
#include <vtkProp.h>
#include <vtkRenderState.h>
#include <vtkRenderer.h>
#include <vtkShaderProgram.h>
#include <vtkSkybox.h>
#include <vtkTextureObject.h>

vtkStandardNewMacro(vtkF3DOverlayRenderPass);

// ----------------------------------------------------------------------------
void vtkF3DOverlayRenderPass::Render(const vtkRenderState* s)
{
  this->Initialize(s);

  double bgColor[3];

  vtkRenderer* r = s->GetRenderer();
  r->GetBackground(bgColor);

  // force background to full black when generating offscreen layers to avoid blending
  // problems when compositing layers in the Blend() function
  r->SetBackground(0.0, 0.0, 0.0);

  vtkRenderState overlayState(s->GetRenderer());
  overlayState.SetPropArrayAndCount(
    this->OverlayProps.data(), static_cast<int>(this->OverlayProps.size()));
  overlayState.SetFrameBuffer(s->GetFrameBuffer());

  // The control-panel "push" shrinks the renderer viewport to the central rect, but the UI (ImGui,
  // sized to the full window) must stay full-window so the docked bars are not clipped. Render the
  // overlay props into a full-window texture regardless of the (possibly central) renderer viewport,
  // then restore it for the scene composite below. ImGui's DisplaySize already uses the full window.
  double savedViewport[4];
  r->GetViewport(savedViewport);
  r->SetViewport(0.0, 0.0, 1.0, 1.0);

  this->OverlayPass->Render(&overlayState);

  r->SetViewport(savedViewport);
  r->SetBackground(bgColor);

  this->CompositeOverlay(s);
}

// ----------------------------------------------------------------------------
void vtkF3DOverlayRenderPass::ReleaseGraphicsResources(vtkWindow* w)
{
  this->Superclass::ReleaseGraphicsResources(w);

  if (this->QuadHelper)
  {
    this->QuadHelper->ReleaseGraphicsResources(w);
  }
  if (this->OverlayPass)
  {
    this->OverlayPass->ReleaseGraphicsResources(w);
  }
  if (this->FrameBufferObject)
  {
    this->FrameBufferObject->ReleaseGraphicsResources(w);
  }
  if (this->ColorTexture)
  {
    this->ColorTexture->ReleaseGraphicsResources(w);
  }
}

// ----------------------------------------------------------------------------
void vtkF3DOverlayRenderPass::Initialize(const vtkRenderState* s)
{
  this->OverlayProps.clear();

  // assign props to the overlay pass
  vtkProp** props = s->GetPropArray();
  for (int i = 0; i < s->GetPropArrayCount(); i++)
  {
    vtkProp* prop = props[i];
    if (!vtkProp3D::SafeDownCast(prop))
    {
      this->OverlayProps.push_back(prop);
    }
  }

  // clear the frame buffers instead of releasing all resources
  if (this->OverlayPass)
  {
    this->OverlayPass->ReleaseGraphicsResources(s->GetRenderer()->GetRenderWindow());
  }
  if (this->FrameBufferObject)
  {
    this->FrameBufferObject->ReleaseGraphicsResources(s->GetRenderer()->GetRenderWindow());
  }

  vtkNew<vtkDefaultPass> overlayP;
  vtkNew<vtkCameraPass> overlayCamP;
  overlayCamP->SetDelegatePass(overlayP);

  this->OverlayPass = vtkSmartPointer<vtkFramebufferPass>::New();
  this->OverlayPass->SetDelegatePass(overlayCamP);
}

// ----------------------------------------------------------------------------
void vtkF3DOverlayRenderPass::CompositeOverlay(const vtkRenderState* s)
{
  vtkOpenGLClearErrorMacro();

  this->NumberOfRenderedProps = 0;

  vtkRenderer* r = s->GetRenderer();
  vtkOpenGLRenderWindow* renWin = static_cast<vtkOpenGLRenderWindow*>(r->GetRenderWindow());
  vtkOpenGLState* ostate = renWin->GetState();

  vtkOpenGLState::ScopedglEnableDisable bsaver(ostate, GL_BLEND);
  vtkOpenGLState::ScopedglEnableDisable dsaver(ostate, GL_DEPTH_TEST);

  assert(this->DelegatePass != nullptr);

  // create FBO and texture
  int pos[2];
  int size[2];
  r->GetTiledSizeAndOrigin(&size[0], &size[1], &pos[0], &pos[1]);

  if (this->ColorTexture == nullptr)
  {
    this->ColorTexture = vtkSmartPointer<vtkTextureObject>::New();
    this->ColorTexture->SetContext(renWin);
    this->ColorTexture->SetMinificationFilter(vtkTextureObject::Linear);
    this->ColorTexture->SetMagnificationFilter(vtkTextureObject::Linear);
    this->ColorTexture->SetWrapS(vtkTextureObject::ClampToEdge);
    this->ColorTexture->SetWrapT(vtkTextureObject::ClampToEdge);
    this->ColorTexture->Allocate2D(size[0], size[1], 4, VTK_UNSIGNED_CHAR);
  }
  this->ColorTexture->Resize(size[0], size[1]);

  if (this->FrameBufferObject == nullptr)
  {
    this->FrameBufferObject = vtkSmartPointer<vtkOpenGLFramebufferObject>::New();
    this->FrameBufferObject->SetContext(renWin);
  }

  // Render the 3D scene delegate into ColorTexture at the central rect's SIZE but with the renderer
  // viewport ORIGIN zeroed. The inner chain (vtkOpenGLCamera::Render, FXAA, SSAA, the F3D blend
  // quad) re-applies the renderer's window-space origin via glViewport; against the central-sized
  // FBO that would shift the scene out of bounds. Zeroing the origin (size, hence aspect ratio,
  // unchanged) keeps them correct — the central offset is re-applied only at the final full-window
  // composite below (the scene texcoord remap).
  double savedViewport[4];
  r->GetViewport(savedViewport);
  r->SetViewport(0.0, 0.0, savedViewport[2] - savedViewport[0], savedViewport[3] - savedViewport[1]);

  renWin->GetState()->PushFramebufferBindings();
  this->RenderDelegate(
    s, size[0], size[1], size[0], size[1], this->FrameBufferObject, this->ColorTexture);
  renWin->GetState()->PopFramebufferBindings();

  r->SetViewport(savedViewport);

  if (!this->QuadHelper)
  {
    std::string FSSource = vtkOpenGLRenderUtilities::GetFullScreenQuadFragmentShaderTemplate();

    vtkShaderProgram::Substitute(FSSource, "//VTK::FSQ::Decl",
      "uniform sampler2D texScene;\n"
      "uniform sampler2D texOverlay;\n"
      // Central rect (normalized window coords) the (central-sized) scene texture maps into. {0,0}
      // and {1,1} when the panel is closed, i.e. the scene fills the whole window as before.
      "uniform vec2 uSceneMin;\n"
      "uniform vec2 uSceneSize;\n"
      "vec3 toLinear(vec3 color) { return pow(color.rgb, vec3(2.2)); }\n"
      "vec3 toSRGB(vec3 color) { return pow(color.rgb, vec3(1.0 / 2.2)); }\n"
      "//VTK::FSQ::Decl");

    // the overlay is alpha premultiplied, we need to divide it, then
    // convert to linear space, and finally premultiply back by the alpha value
    vtkShaderProgram::Substitute(FSSource, "//VTK::FSQ::Impl",
      "  vec4 ovlSample = texture(texOverlay, texCoord);\n"
      "  if (ovlSample.a > 0.0)\n"
      "    ovlSample.rgb = toLinear(ovlSample.rgb / ovlSample.a);\n"
      "  ovlSample.rgb *= ovlSample.a;\n"
      "//VTK::FSQ::Impl");

    // the scene texture is not alpha premultiplied, so it should not be divided.
    // The quad is drawn full-window (so the full-window overlay/bars composite), but the scene
    // texture only covers the central rect, so its texcoord is remapped into [uSceneMin, +uSceneSize].
    // Outside that rect we are under an opaque bar (overlay alpha == 1), so the clamped scene sample
    // there is fully covered and never seen.
    vtkShaderProgram::Substitute(FSSource, "//VTK::FSQ::Impl",
      "  vec2 sceneCoord = (texCoord - uSceneMin) / uSceneSize;\n"
      "  vec4 sceneSample = texture(texScene, sceneCoord);\n"
      "  vec3 initialSceneColor = sceneSample.rgb;\n"
      "  sceneSample.rgb = toLinear(sceneSample.rgb);\n"
      "  sceneSample.rgb *= sceneSample.a;\n"
      "//VTK::FSQ::Impl");

    // blend overlay frame with current frame
    vtkShaderProgram::Substitute(FSSource, "//VTK::FSQ::Impl",
      "  sceneSample.rgb = (1.0 - ovlSample.a) * sceneSample.rgb + ovlSample.rgb;\n"
      "  sceneSample.a = (1.0 - ovlSample.a) * sceneSample.a + ovlSample.a;\n"
      "//VTK::FSQ::Impl");

    // divide by alpha and convert back to sRGB
    // we shouldn't premultiply by alpha again here because the OpenGL blending
    // function is expecting the source fragment not premultiplied
    vtkShaderProgram::Substitute(FSSource, "//VTK::FSQ::Impl",
      "  if (sceneSample.a > 0.0)\n"
      "    sceneSample.rgb = sceneSample.rgb / sceneSample.a;\n"
      "  sceneSample.rgb = toSRGB(sceneSample.rgb);\n"
      "//VTK::FSQ::Impl");

    // mix the sRGB result with the original render to add the background back in
    vtkShaderProgram::Substitute(FSSource, "//VTK::FSQ::Impl",
      "  sceneSample.rgb = (1.0 - sceneSample.a) * initialSceneColor + sceneSample.a * "
      "sceneSample.rgb;\n"
      "  gl_FragData[0] = sceneSample;\n"
      "//VTK::FSQ::Impl");

    this->QuadHelper =
      std::make_shared<vtkOpenGLQuadHelper>(renWin, nullptr, FSSource.c_str(), nullptr);
    this->QuadHelper->ShaderChangeValue = this->GetMTime();
  }
  else
  {
    renWin->GetShaderCache()->ReadyShaderProgram(this->QuadHelper->Program);
  }

  this->OverlayPass->GetColorTexture()->Activate();
  this->ColorTexture->Activate();

  this->QuadHelper->Program->SetUniformi(
    "texOverlay", this->OverlayPass->GetColorTexture()->GetTextureUnit());
  this->QuadHelper->Program->SetUniformi("texScene", this->ColorTexture->GetTextureUnit());

  // The composite quad covers the FULL window so the full-window overlay (and the docked bars
  // outside the central rect) are drawn. The central rect [pos,size] — where the central-sized scene
  // texture lives — is passed normalized so the shader maps the scene into it. When the panel is
  // closed pos/size span the whole window, so this is identical to a plain full-window blit.
  const int* winSize = renWin->GetSize();
  const float fw = static_cast<float>(winSize[0] > 0 ? winSize[0] : 1);
  const float fh = static_cast<float>(winSize[1] > 0 ? winSize[1] : 1);
  const float sceneMin[2] = { pos[0] / fw, pos[1] / fh };
  const float sceneSize[2] = { size[0] / fw, size[1] / fh };
  this->QuadHelper->Program->SetUniform2f("uSceneMin", sceneMin);
  this->QuadHelper->Program->SetUniform2f("uSceneSize", sceneSize);

  ostate->vtkglDisable(GL_BLEND);
  ostate->vtkglDisable(GL_DEPTH_TEST);
  ostate->vtkglViewport(0, 0, winSize[0], winSize[1]);
  ostate->vtkglScissor(0, 0, winSize[0], winSize[1]);

  this->QuadHelper->Render();

  this->OverlayPass->GetColorTexture()->Deactivate();
  this->ColorTexture->Deactivate();

  vtkOpenGLCheckErrorMacro("failed after Render");
}
