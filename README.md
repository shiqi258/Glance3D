[![CI](https://img.shields.io/github/actions/workflow/status/glance3d-app/glance3d/ci.yml?label=CI&logo=github)](https://github.com/glance3d-app/glance3d/actions/workflows/ci.yml)
[![Packaging](https://img.shields.io/github/actions/workflow/status/glance3d-app/glance3d-superbuild/nightly.yml?label=Packaging&logo=github)](https://github.com/glance3d-app/glance3d-superbuild)
[![codecov](https://codecov.io/gh/glance3d-app/glance3d/branch/master/graph/badge.svg?token=siwG82IXK7)](https://codecov.io/gh/glance3d-app/glance3d)
[![Downloads](https://img.shields.io/github/downloads/glance3d-app/glance3d/total.svg)](https://github.com/glance3d-app/glance3d/releases)
[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-2.1-4baaaa.svg)](CODE_OF_CONDUCT.md)

English | [简体中文](README.zh-CN.md)

# Glance3D

Glance3D is a 3D viewer focused on fast file previews and rich interaction around preview settings.

It is derived from [F3D](https://f3d.app/) and keeps the strong foundation of `f3d`/`libf3d`: broad format support, VTK-based rendering, command-line automation, configuration files, thumbnail generation, and embeddable rendering APIs. On top of that foundation, Glance3D aims to make the preview workflow more direct, tunable, and comfortable for real production use.

![Glance3D rendering example](https://media.githubusercontent.com/media/glance3d-app/glance3d-website/refs/heads/main/static/images/typical.png)

## Positioning

Glance3D is designed for people who need to inspect many 3D assets quickly while still being able to control how those assets are previewed.

- Open and preview 3D models, CAD/BIM data, point clouds, volumes, scientific datasets, images, and environment maps.
- Adjust preview-related rendering options such as materials, textures, edges, lighting, HDRI, scalar coloring, volume rendering, tone mapping, SSAO, and optional ray tracing.
- Use interactive controls, hotkeys, drag and drop, command scripts, and configuration files to keep preview workflows repeatable.
- Generate screenshots and file-manager thumbnails with dedicated preview configuration.
- Reuse the `libf3d` rendering stack from C++, C, Python, Java, and JavaScript/WebAssembly.

## Relationship With F3D

Glance3D is built on the original F3D codebase. During the transition, some executable names, CMake options, configuration paths, and library APIs still use the `f3d` or `F3D_` names. This is intentional for compatibility with the existing ecosystem and build system.

In practice:

- The project identity is Glance3D.
- The current command-line executable is still `f3d`.
- The core library is still exposed as `libf3d`.
- Many build options are still named `F3D_*`.

## Quick Start

Open a model from your file manager, or run:

```bash
f3d /path/to/file.ext
```

Save the current preview as an image:

```bash
f3d /path/to/file.ext --output=/path/to/preview.png
```

Print the available options:

```bash
f3d --help
```

After opening a file, press `H` to show the interaction cheat sheet.

Common interactions:

- Left mouse drag: rotate the view.
- Mouse wheel or right mouse drag: zoom.
- Middle mouse drag: pan.
- `Enter`: reset the camera.
- `Space`: play or pause animation.
- `G`: toggle the grid.
- `K`: cycle interaction style.

See the [Quickstart Guide](doc/user/01-QUICKSTART.md) for more details.

## Get Glance3D

Use the project download page or release page for prebuilt packages:

- [glance3d.app/download](https://glance3d.app/download)
- [GitHub Releases](https://github.com/glance3d-app/glance3d/releases)

## Preview Configuration

Glance3D can be driven by command-line options, configuration files, and interactive commands. This makes it possible to keep different preview presets for different asset types or workflows.

Examples:

```bash
# Preview a model and save a screenshot
f3d model.glb --output=preview.png

# List readers enabled in the current build
f3d --list-readers

# Use a specific interaction style
f3d drawing.png --interaction-style=2d

# Configure a reader option, for example VDB downsampling
f3d volume.vdb -DVDB.downsampling_factor=0.5

# Play all animations
f3d animated.glb --animation-indices=-1
```

Useful documentation:

- [Options](doc/user/03-OPTIONS.md)
- [Interactions](doc/user/04-INTERACTIONS.md)
- [Configuration files](doc/user/06-CONFIGURATION_FILE.md)
- [Commands and scripts](doc/user/07-COMMANDS.md)
- [Desktop integration and thumbnails](doc/user/11-DESKTOP_INTEGRATION.md)

## Supported Formats

Supported formats depend on build options and enabled plugins. Common formats include:

- General 3D: `.gltf`, `.glb`, `.obj`, `.fbx`, `.dae`, `.3ds`, `.3mf`, `.off`, `.x`
- CAD/BIM: `.step`, `.stp`, `.iges`, `.igs`, `.brep`, `.xbf`, `.ifc`
- Scientific and volume data: `.vtk`, `.vtp`, `.vtu`, `.vtr`, `.vti`, `.vts`, `.vtm`, `.vtkhdf`, `.vdb`, `.nc`, `.nrrd`, `.mhd`
- Point clouds and scans: `.ply`, `.pts`, `.las`, `.laz`, `.pcd`, `.ptx`
- Images and environment maps: `.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga`, `.hdr`, `.webp`, `.exr`
- 3D Gaussian Splatting: `.splat`, `.spz`

Use [Supported Formats](doc/user/02-SUPPORTED_FORMATS.md) and `f3d --list-readers` as the source of truth for your build.

## Build

Glance3D uses CMake. Basic requirements include:

- CMake
- A C++20 compiler
- A CMake-compatible build system such as Ninja, Make, Visual Studio, or Xcode
- VTK 9.4.0 or newer

Typical local build:

```bash
cmake -S /path/to/source -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

With the repository presets:

```bash
cmake --preset=dev /path/to/source
cmake --preset=vcpkg /path/to/source
```

Common CMake options:

- `F3D_BUILD_APPLICATION`: build the Glance3D application.
- `BUILD_TESTING`: enable tests.
- `F3D_MODULE_UI`: enable ImGui UI support.
- `F3D_MODULE_RAYTRACING`: enable ray tracing support, when VTK supports OSPRay.
- `F3D_PLUGIN_BUILD_ASSIMP`: enable Assimp formats.
- `F3D_PLUGIN_BUILD_OCCT`: enable Open CASCADE formats.
- `F3D_PLUGIN_BUILD_USD`: enable OpenUSD formats.
- `F3D_PLUGIN_BUILD_VDB`: enable OpenVDB formats.
- `F3D_BINDINGS_PYTHON`: build Python bindings.
- `F3D_BINDINGS_C`: build C bindings.
- `F3D_BINDINGS_JAVA`: build Java bindings.

See the [build documentation](doc/dev/05-BUILD.md) and [testing documentation](doc/dev/06-TESTING.md) for the full workflow.

## Project Layout

```text
application/     Command-line and desktop application entry points
library/         libf3d core C++ API
c/               C bindings
python/          Python bindings
java/            Java bindings
webassembly/     JavaScript/WebAssembly bindings and tests
plugins/         Optional format reader plugins
vtkext/          VTK-based extension modules
doc/             User, developer, and libf3d documentation
examples/        libf3d examples
testing/         Test data, interaction scripts, and rendering baselines
resources/       Icons, configuration, shell completion, and bundled resources
cmake/           CMake modules and install configuration
```

## Documentation

- [User quickstart](doc/user/01-QUICKSTART.md)
- [Supported formats](doc/user/02-SUPPORTED_FORMATS.md)
- [Command-line options](doc/user/03-OPTIONS.md)
- [Interactions](doc/user/04-INTERACTIONS.md)
- [Configuration files](doc/user/06-CONFIGURATION_FILE.md)
- [Plugins](doc/user/12-PLUGINS.md)
- [libf3d overview](doc/libf3d/01-OVERVIEW.md)
- [Developer getting started](doc/dev/04-GETTING_STARTED.md)
- [Architecture](doc/dev/08-ARCHITECTURE.md)
- [Changelog](doc/CHANGELOG.md)

## Contributing

Glance3D is community-driven and welcomes bug reports, documentation improvements, feature work, and preview-workflow ideas.

Before contributing, please read the [contribution guide](CONTRIBUTING.md), [code of conduct](CODE_OF_CONDUCT.md), and [AI policy](AI_POLICY.md).

## Acknowledgments

Glance3D is derived from F3D, which was initially created by [Kitware SAS](https://www.kitware.eu/), Joachim Pouderoux, Michael Migliore, and Mathieu Westphal, and later maintained by the F3D-APP Foundation. Glance3D relies on many open source projects, including [VTK](https://vtk.org/), [OCCT](https://dev.opencascade.org/), [Assimp](https://www.assimp.org/), [Alembic](http://www.alembic.io/), [Draco](https://google.github.io/draco/), [web-ifc](https://github.com/ThatOpen/engine_web-ifc), [OpenUSD](https://openusd.org/release/index.html), [OpenVDB](https://www.openvdb.org/), [PDAL](https://pdal.org), [OSPRay](https://www.ospray.org/), and [ImGui](https://github.com/ocornut/imgui/).

## License

Glance3D is distributed under the 3-Clause BSD License. See [LICENSE.md](LICENSE.md).

Third-party libraries and tools are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
