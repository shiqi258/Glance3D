[![CI](https://img.shields.io/github/actions/workflow/status/glance3d-app/glance3d/ci.yml?label=CI&logo=github)](https://github.com/glance3d-app/glance3d/actions/workflows/ci.yml) [![Packaging](https://img.shields.io/github/actions/workflow/status/glance3d-app/glance3d-superbuild/nightly.yml?label=Packaging&logo=github)](https://github.com/glance3d-app/glance3d-superbuild) [![codecov](https://codecov.io/gh/glance3d-app/glance3d/branch/master/graph/badge.svg?token=siwG82IXK7)](https://codecov.io/gh/glance3d-app/glance3d) [![Downloads](https://img.shields.io/github/downloads/glance3d-app/glance3d/total.svg)](https://github.com/glance3d-app/glance3d/releases) [![Sponsors](https://img.shields.io/static/v1?label=Sponsor&message=%E2%9D%A4&logo=GitHub&color=%23fe8e86)](https://glance3d.app/thanks) [![Discord](https://discordapp.com/api/guilds/1046005690809978911/widget.png?style=shield)](https://discord.glance3d.app) [![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-2.1-4baaaa.svg)](CODE_OF_CONDUCT.md)

English | [简体中文](README.zh-CN.md)

# Glance3D - Fast and minimalist 3D viewer

By the Glance3D Foundation.

<img src="https://raw.githubusercontent.com/glance3d-app/glance3d/master/resources/logo.svg" align="left" width="20px"/>
Glance3D is a fast and minimalist 3D viewer desktop application. It supports many file formats, from digital content to scientific datasets (including glTF, USD, STL, STEP, PLY, OBJ, FBX, Alembic), can show animations and support thumbnails and many rendering and texturing options including real time physically based rendering and raytracing.
<br clear="left"/>

It is fully controllable from the command line and support configuration files. It can provide thumbnails, support interactive hotkeys, drag&drop and integration into file managers.

Glance3D also contains libf3d, a simple library to render meshes, with a C++17 API, C, Python, Java and Javascript Bindings.

<img src="https://media.githubusercontent.com/media/glance3d-app/glance3d-website/refs/heads/main/static/images/typical.png" width="640" />

_A typical render by Glance3D_

<img src="https://user-images.githubusercontent.com/3129530/194735261-dd6f1c1c-fa57-47b0-9d27-f735d18ccd5e.gif" width="640" />

_Animation of a glTF file within Glance3D_

<img src="https://media.githubusercontent.com/media/glance3d-app/glance3d-website/refs/heads/main/static/images/directScalars.png" width="640" />

_A direct scalars render by Glance3D_

See the [gallery](https://glance3d.app/gallery) for more images, take a look at the [changelog](doc/CHANGELOG.md) or go to the [download page](https://glance3d.app/download) to download and install Glance3D!

You can even use Glance3D directly in your [browser](https://glance3d.app/viewer)!

If you need any help or want to discuss with other Glance3D users and developers, head over to our [discord](https://discord.glance3d.app).

# Quickstart

Open a file directly in Glance3D or from the command line by running:

```
f3d /path/to/file.ext
```

Optionally, append `--output=/path/to/img.png` to save the rendering into an image file.

See the [Quickstart Guide](doc/user/01-QUICKSTART.md) for more information about getting started with Glance3D.

# Documentation

- To get started, please take a look at the [user documentation](doc/user/01-QUICKSTART.md).
- If you need any help, are looking for a feature or found a bug, please open an [issue](https://github.com/glance3d-app/glance3d/issues).
- If you want to use the libf3d, please take a look at its [documentation](doc/libf3d/01-OVERVIEW.md).
- If you want to build Glance3D, please take a look at the [contribution guide](CONTRIBUTING.md).

# Support

Glance3D needs your help!

If you can, please consider sponsoring Glance3D. Even a small donation would help us offset the recurring maintenance costs.
With enough sponsors we would be able to make Glance3D grow faster and stronger! Read more about it [here](https://glance3d.app/thanks).

If you are an industry user of Glance3D and want to make sure it can keep growing and being maintained, [please reach out](https://glance3d.app/thanks)!

In any case, please star it on github and share the word about it!

## Sponsors

Many thanks to our sponsors for supporting Glance3D

<a href="https://nlnet.nl/project/F3D/" target="_blank"><img src="https://nlnet.nl/image/logos/NGI0Core_tag.svg" height="45"/></a>
<a href="https://www.opendronemap.org/" target="_blank"><img src="https://glance3d.app/assets/images/opendronemap-95d4ad6e24c091a06ec00e1828e1eb38.png" height="45" /></a>

# Vision

As a minimalist 3D viewer Glance3D aims to:

- Support as many 3D file formats as possible
- Support many types of renderings (textures, edges, etc... ) and visualizations (meshes, volumic, point sprites)
- Support any and all use-cases dealing with 3D datasets
- Let any user easily and quickly view any model with good defaults
- Be as configurable as possible
- Be fully controllable from the command line and configuration file
- Be usable non-interactively
- Be as modular as possible to be built with a small number of dependencies

but there is no plan to:

- Provide a classic mouse-based UI, with menus and buttons
- Provide data processing tools
- Provide export feature

# Contributing

Glance3D is a community-driven, inclusive and beginner-friendly project. We love to see how the project is growing thanks to the contributions from the community. We would love to see your face in the list below! If you want to contribute to Glance3D, you are very welcome to! Take a look at our [contribution documentation](CONTRIBUTING.md), [governance documentation](doc/dev/11-GOVERNANCE.md) and [code of conduct](CODE_OF_CONDUCT.md).

<a href="https://github.com/glance3d-app/glance3d/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=glance3d-app/glance3d" />
</a>

# Acknowledgments

Glance3D is derived from F3D, which was initially created by [Kitware SAS](https://www.kitware.eu/), by Joachim Pouderoux, Michael Migliore and Mathieu Westphal, and later maintained by the F3D-APP Foundation. Glance3D relies on many awesome open source projects, including [VTK](https://vtk.org/), [OCCT](https://dev.opencascade.org/), [Assimp](https://www.assimp.org/), [Alembic](http://www.alembic.io/), [Draco](https://google.github.io/draco/), [web-ifc](https://github.com/ThatOpen/engine_web-ifc), [OpenUSD](https://openusd.org/release/index.html), [OpenVDB](https://www.openvdb.org/), [PDAL](https://pdal.org), [OSPRay](https://www.ospray.org/) and [ImGui](https://github.com/ocornut/imgui/).

# License

Glance3D can be used and distributed under the 3-Clause BSD License, see the [license](LICENSE.md).
Glance3D integrates the sources of other libraries and tools, all under permissive licenses, see the [third party licenses](THIRD_PARTY_LICENSES.md).
Glance3D packages rely on other libraries and tools, all under permissive licenses, all listed in the respective packages.
