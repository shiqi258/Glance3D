# Coding Style

Glance3D use different coding styles in each component, however there are some common rules

## C++

### Common rules

Overall syntax:

- CamelCase.
- Avoid using `using` for namespaces.
- Initialize variables in header when possible.
- Local variables starts with a lower case char.
- Class members starts with a upper case char.
- Indents are two spaces.
- One space between instruction and parenthesis.
- Always add curly brace after instruction.
- Curly brace one a new line, aligned with instruction.
- Add `//----------------------------------------------------------------------------` before any method in implementation.

Example:

```cpp
//----------------------------------------------------------------------------
void class::method()
{
  if (condition)
  {
    std::vector<int> myIntVector = { 13 };
  }
}
```

Includes:

- Organized by category: `F3DApplication`, `libf3d`, `VTKExtensions`, `other deps`, `std`, `system`.
- Sorted inside category.

### Glance3D Application rules

- Class starts with `F3D`
- Method starts with an uppercase char.

### libf3d rules

- Class starts with a lower case char.
- Method starts with an lower case char.

### VTKExtensions rules

- Follow VTK rules
- Method starts with an uppercase char.
- Class starts with `vtkF3D` if inheriting from vtkObject.
- Class starts with `F3D` if not inheriting from vtkObject.

### Desktop UI sizes

The desktop UI (`vtkext/private/module`, Dear ImGui) scales with the monitor DPI times `ui.scale`,
quantized so that geometry and text share one factor. Two strong types keep every length in its
unit (`G3DUnits.h`):

- A `G3DDp` is always a **logical** length (px at scale 1); a `float` / `ImVec2` length is always
  **physical** (already scaled). The one conversion between them is `dp * scale`.
- Design constants are `G3DDp`: the `G3DTheme` tokens and `12_dp` literals. The widget parameters
  that take them (`IconButton` size, `BeginPropRow`, tooltip padding, `FloatingCardDesc` margin and
  padding, `FieldRowDesc`...) have a deleted `float` overload.
- There is one scale, `G3DWidgets::UiScale()` (`vtkF3DUIActor::GetUiScale()` in the actor). Never
  derive one from `GetFontSize()`; `float * G3DScale` does not compile.
- Right before a native ImGui call, convert with `G3DWidgets::Px()`, or `PxTrunc()` for a value
  pushed in place of a style metric.
- Reserve room with a measuring function (`ButtonSize`, `IconButtonSize`, `SegmentedIconSize`,
  `BadgeSize`, `ToolGroupSize`, `MeasureFieldRow`...), never with a token. Controls sharing a line
  are a `FieldRow`.
- Size and place windows before submitting them: an offscreen `--output` render gets one frame.

Review checklist for a UI change:

- [ ] No bare number is used as a length in an ImGui layout or draw call; new design constants are
      `G3DDp`.
- [ ] No `.Raw()` / `.Factor()` outside a unit boundary, and each one says why:
      `// g3d-units: allow(<rule>) <reason>`.
- [ ] Every reserved width comes from a measuring function.
- [ ] Looked at 1.0, 1.25 and 1.5 or 2.0 (`--dpi-aware` with `CTEST_F3D_FORCE_DPI_SCALE`, or
      `--font-scale`), and `ctest -C Release -L "lint|ui-layout|module"` passes.
- [ ] Every popup opened from a control goes through `G3DWidgets::BeginPopover` (see below).

`ctest -L lint` enforces the rules the compiler cannot see (`scripts/check-ui-units.mjs`, a ratchet
over `scripts/ui-units-baseline.json`) and `ctest -L ui-layout` checks that the whole UI scales by
one factor (see [Testing](06-TESTING.md)). The design side is the styleguide's sizes and scaling
section (`doc/dev/ui-styleguide.html#units`).

### Desktop UI floating surfaces

A popup opened from a control (a dropdown's menu, the color picker, any panel under or over its
trigger) is a `G3DWidgets::BeginPopover`, never an `ImGui::BeginPopup` placed by hand. ImGui hides
a popup on the frame it opens and lays its content out at a reset size, so `GetWindowSize()` then
reports the empty window: a side chosen from a size remembered on an earlier frame draws the first
visible frame on the wrong side, and the next frame jumps. The popover is placed from the size
measured on that hidden frame, below its trigger or else above it, keeps its side for the whole
open with the edge facing the trigger pinned, and when neither side takes it whole shrinks on the
bigger one and scrolls.

The other kinds each have their own component: a menu opened at the pointer is
`BeginContextMenu`, hover help goes through the tooltip helpers, a persistent draggable panel is
`BeginFloatingCard`. `ctest -L lint` flags any other `ImGui::BeginPopup` / `BeginCombo` (rule
`raw-popup`), and `TestG3DPopover` checks the popovers frame by frame. The design side is the
styleguide's layering section (`doc/dev/ui-styleguide.html#layering`).

### Render invalidation

A tick of the event loop ends in one frame: a full render, or a UI-only frame that redraws the UI
over the 3D image the last full frame left. Nobody requests the full one after changing the scene:
the loop finds out itself (`window_impl::IsG3DFrameStale`) whether the options changed, whether the
renderer holds configuration only a full render applies (`vtkF3DRenderer::HasPendingG3DUpdates`),
or whether anything the 3D image was rendered from was modified since
(`vtkF3DRenderer::IsG3DSceneLayerStale`: camera, lights, props with their mappers and inputs,
importer updates, lighting environment, pass chain, viewport). This is the usual on-demand
rendering model, owners marking themselves dirty and the frame loop deciding once per frame, with
VTK's MTime as the dirty mark, like vtkRenderer's own backing store.

UI code (the ImGui frame) does not change the scene or the camera: it runs inside the render pass,
where the scene is being drawn and the active camera is a throwaway copy. It sends a command, which
the loop runs between frames; any value in a command (a path, a name) goes through `G3DCommandLine`
/ `g3d::command`, since the command tokenizer reads a bare backslash as an escape.

Review checklist for a change that touches what is on screen:

- [ ] New state that shows in the 3D image but is not a VTK object of the renderer, an option or a
      `*Configured` flag is covered by `IsG3DSceneLayerStale` / `HasPendingG3DUpdates` (or, for
      state the library cannot see, the change calls `requestRender()`).
- [ ] Idle stays UI-only: `TestSDKRenderInvalidation` passes, and the log shows no repeated
      `[render.stale]` while nothing happens (a check that finds the frame stale every tick turns the
      viewer into a continuous renderer).
- [ ] UI code changes the scene or the camera only through commands (`ctest -L lint`, rule
      `ui-scene-mutation`), built with `G3DCommandLine` when they carry a value.
- [ ] Interaction tests end on a settled frame (the presented-frame guard, see `06-TESTING.md`).

### Automatic formatting

Some of the rules above are enforced using clang-format thanks to a `.clang-format` file.
The continuous integration checks it whenever code is pushed to a pull request.
To fix it locally, you can use:

- single file: `clang-format -i /path/to/file.ext`
- all files: `shopt -s globstar; clang-format -i **/*.{h,cxx}`

Please note there can be small discrepancy between the CI and local run of clang-format depending
on the version in use. You may need to fix these manually.

## Python

All python code is simply formatted using [Black style](https://black.readthedocs.io/en/stable/the_black_code_style/current_style.html).

The continuous integration checks the formatting of python code using `black`.
You can fix it locally by running:

- single file: `black /path/to/file.py`
- all files: `black --include '(\.py|\.py\.in)' .`

## Markdown and others

Markdown, JavaScript, JSON, HTML and YAML files are formatted using [Prettier](https://prettier.io/docs/).

The continuous integration checks the formatting of all these files using `prettier`.
You can fix them locally by running:

- single file: `prettier -w /path/to/file.ext`
- all files: `shopt -s dotglob;shopt -s globstar; prettier -w **/*.{ts,tsx,js,json,md,html,yml}`
