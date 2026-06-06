[![CI](https://img.shields.io/github/actions/workflow/status/glance3d-app/glance3d/ci.yml?label=CI&logo=github)](https://github.com/glance3d-app/glance3d/actions/workflows/ci.yml)
[![Packaging](https://img.shields.io/github/actions/workflow/status/glance3d-app/glance3d-superbuild/nightly.yml?label=Packaging&logo=github)](https://github.com/glance3d-app/glance3d-superbuild)
[![codecov](https://codecov.io/gh/glance3d-app/glance3d/branch/master/graph/badge.svg?token=siwG82IXK7)](https://codecov.io/gh/glance3d-app/glance3d)
[![Downloads](https://img.shields.io/github/downloads/glance3d-app/glance3d/total.svg)](https://github.com/glance3d-app/glance3d/releases)
[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-2.1-4baaaa.svg)](CODE_OF_CONDUCT.md)

[English](README.md) | 简体中文

# Glance3D

Glance3D 是一款快速、轻量、面向生产场景的 3D 查看器。它可以打开数字内容创作、CAD/BIM、点云和科学可视化等多类 3D 数据文件，支持命令行控制、配置文件、缩略图生成、动画播放以及基于 `libf3d` 的二次开发。

项目由 Glance3D Foundation 维护，底层依赖 VTK，并通过插件体系扩展 Assimp、Open CASCADE、Alembic、OpenUSD、OpenVDB、PDAL、Draco、web-ifc、OSPRay 等生态能力。

![Glance3D rendering example](https://media.githubusercontent.com/media/glance3d-app/glance3d-website/refs/heads/main/static/images/typical.png)

## 目录

- [功能特性](#功能特性)
- [快速开始](#快速开始)
- [安装方式](#安装方式)
- [常用命令](#常用命令)
- [支持格式](#支持格式)
- [开发与构建](#开发与构建)
- [项目结构](#项目结构)
- [文档导航](#文档导航)
- [参与贡献](#参与贡献)
- [许可证](#许可证)

## 功能特性

- 快速打开和预览 3D 模型、点云、体数据、科学数据集和常见图像格式。
- 支持 glTF/GLB、USD、STL、STEP、PLY、OBJ、FBX、Alembic、3MF、VTK、IFC、VDB、LAS/LAZ、3D Gaussian Splatting 等格式，具体取决于构建时启用的插件。
- 提供 PBR、贴图、边线、点精灵、体渲染、色标、HDRI、SSAO、实时光照和可选光线追踪等渲染能力。
- 支持动画播放、快捷键交互、拖放打开文件，以及文件管理器缩略图集成。
- 可通过命令行、配置文件和命令脚本完成可重复、非交互式渲染流程。
- 内置 `libf3d`，提供 C++17 API，并包含 C、Python、Java 和 JavaScript/WebAssembly 绑定。
- 采用模块化插件设计，可按使用场景裁剪依赖和构建体积。

## 快速开始

安装 Glance3D 后，可以直接从文件管理器打开模型，也可以使用命令行：

```bash
f3d /path/to/file.ext
```

将当前视图渲染为图片：

```bash
f3d /path/to/file.ext --output=/path/to/image.png
```

查看命令行帮助：

```bash
f3d --help
```

在 Linux 上也可以查看 man 手册：

```bash
man f3d
```

打开文件后，按 `H` 可查看快捷键列表。常用交互包括：

- 鼠标左键拖动：旋转视角。
- 鼠标右键上下拖动或滚轮：缩放。
- 鼠标中键拖动：平移。
- `Enter`：重置相机。
- `Space`：播放或暂停动画。
- `G`：切换水平网格。

更多入门说明见 [快速开始文档](doc/user/01-QUICKSTART.md)。

## 安装方式

推荐优先使用项目发布页或官方网站提供的预构建包：

- 官方网站：[glance3d.app](https://glance3d.app)
- 下载页面：[glance3d.app/download](https://glance3d.app/download)
- GitHub Releases：[glance3d-app/glance3d/releases](https://github.com/glance3d-app/glance3d/releases)

如果你需要 Python 绑定，可根据 Python 包发布方式安装 `glance3d`，具体可参考项目的 Python 构建配置和发布说明。

## 常用命令

```bash
# 打开单个文件
f3d model.glb

# 输出截图
f3d model.step --output=preview.png

# 列出当前构建支持的读取器
f3d --list-readers

# 指定读取器参数，例如控制 VDB 体数据降采样
f3d volume.vdb -DVDB.downsampling_factor=0.5

# 播放所有动画
f3d animated.glb --animation-indices=-1
```

完整命令行参数见 [选项文档](doc/user/03-OPTIONS.md)，命令脚本能力见 [命令文档](doc/user/07-COMMANDS.md)。

## 支持格式

Glance3D 支持的格式会受到构建选项和插件启用情况影响。常见格式包括：

- 通用 3D：`.gltf`、`.glb`、`.obj`、`.fbx`、`.dae`、`.3ds`、`.3mf`、`.off`、`.x`
- CAD/BIM：`.step`、`.stp`、`.iges`、`.igs`、`.brep`、`.xbf`、`.ifc`
- 科学与体数据：`.vtk`、`.vtp`、`.vtu`、`.vtr`、`.vti`、`.vts`、`.vtm`、`.vtkhdf`、`.vdb`、`.nc`、`.nrrd`、`.mhd`
- 点云与扫描：`.ply`、`.pts`、`.las`、`.laz`、`.pcd`、`.ptx`
- 图像与环境贴图：`.png`、`.jpg`、`.jpeg`、`.bmp`、`.tga`、`.hdr`、`.webp`、`.exr`
- 3D Gaussian Splatting：`.splat`、`.spz`

请以 [支持格式文档](doc/user/02-SUPPORTED_FORMATS.md) 和本机 `f3d --list-readers` 输出为准。

## 开发与构建

Glance3D 使用 CMake 构建。基础依赖包括：

- CMake
- C++20 编译器
- CMake 兼容的构建系统，如 Ninja、Make、Visual Studio 或 Xcode
- VTK 9.4.0 及以上版本，推荐使用项目文档中的建议版本

典型本地构建：

```bash
cmake -S /path/to/source -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

贡献者可以使用仓库提供的开发预设启用调试构建、严格检查和测试：

```bash
cmake --preset=dev /path/to/source
```

使用 vcpkg 管理依赖时，可使用内置 `vcpkg` 预设：

```bash
cmake --preset=vcpkg /path/to/source
cmake --build build --config Release
```

安装构建产物：

```bash
cmake --install build
```

常用 CMake 选项：

- `F3D_BUILD_APPLICATION`：构建 Glance3D 可执行程序。
- `BUILD_TESTING`：启用测试。
- `F3D_MODULE_RAYTRACING`：启用光线追踪模块，需要 VTK 支持 OSPRay。
- `F3D_MODULE_EXR`：启用 OpenEXR 图像支持。
- `F3D_MODULE_WEBP`：启用 WebP 图像支持。
- `F3D_PLUGIN_BUILD_ASSIMP`：启用 Assimp 插件。
- `F3D_PLUGIN_BUILD_OCCT`：启用 Open CASCADE 插件。
- `F3D_PLUGIN_BUILD_USD`：启用 OpenUSD 插件。
- `F3D_PLUGIN_BUILD_VDB`：启用 OpenVDB 插件。
- `F3D_BINDINGS_PYTHON`：构建 Python 绑定。
- `F3D_BINDINGS_C`：构建 C 绑定。
- `F3D_BINDINGS_JAVA`：构建 Java 绑定。

完整构建说明见 [构建文档](doc/dev/05-BUILD.md)，测试说明见 [测试文档](doc/dev/06-TESTING.md)，WebAssembly 构建见 [WASM 构建文档](doc/dev/14-BUILD_WASM.md)。

## 项目结构

```text
application/     Glance3D 命令行和桌面应用入口
library/         libf3d 核心 C++ API
c/               C 语言绑定
python/          Python 绑定
java/            Java 绑定
webassembly/     WebAssembly 绑定和测试
plugins/         可选格式读取插件
vtkext/          基于 VTK 的扩展模块
doc/             用户、开发者和 libf3d 文档
examples/        libf3d 示例代码
testing/         测试数据、脚本和渲染基线
resources/       图标、配置、补全脚本和资源文件
cmake/           CMake 模块和安装配置
```

## 文档导航

- 用户入门：[doc/user/01-QUICKSTART.md](doc/user/01-QUICKSTART.md)
- 支持格式：[doc/user/02-SUPPORTED_FORMATS.md](doc/user/02-SUPPORTED_FORMATS.md)
- 命令行选项：[doc/user/03-OPTIONS.md](doc/user/03-OPTIONS.md)
- 交互方式：[doc/user/04-INTERACTIONS.md](doc/user/04-INTERACTIONS.md)
- 配置文件：[doc/user/06-CONFIGURATION_FILE.md](doc/user/06-CONFIGURATION_FILE.md)
- 插件说明：[doc/user/12-PLUGINS.md](doc/user/12-PLUGINS.md)
- libf3d 概览：[doc/libf3d/01-OVERVIEW.md](doc/libf3d/01-OVERVIEW.md)
- 开发上手：[doc/dev/04-GETTING_STARTED.md](doc/dev/04-GETTING_STARTED.md)
- 架构说明：[doc/dev/08-ARCHITECTURE.md](doc/dev/08-ARCHITECTURE.md)
- 更新日志：[doc/CHANGELOG.md](doc/CHANGELOG.md)

## 参与贡献

Glance3D 是社区驱动项目，欢迎提交问题、改进文档、修复缺陷或实现新功能。

建议流程：

1. 阅读 [贡献指南](CONTRIBUTING.md)、[行为准则](CODE_OF_CONDUCT.md) 和 [AI 使用政策](AI_POLICY.md)。
2. 在 GitHub Issues 中确认问题或需求，必要时先留言讨论。
3. Fork 仓库并创建功能分支。
4. 尽早创建 Draft Pull Request，方便维护者了解方向并给出反馈。
5. 为行为变更补充测试，并确保格式化、构建和 CI 检查通过。

贡献开发前，建议先阅读 [开发者入门](doc/dev/04-GETTING_STARTED.md)、[构建文档](doc/dev/05-BUILD.md)、[测试文档](doc/dev/06-TESTING.md) 和 [代码风格](doc/dev/09-CODING_STYLE.md)。

## 支持与交流

- 官网：[glance3d.app](https://glance3d.app)
- 在线查看器：[glance3d.app/viewer](https://glance3d.app/viewer)
- GitHub Issues：[glance3d-app/glance3d/issues](https://github.com/glance3d-app/glance3d/issues)
- Discord：[discord.glance3d.app](https://discord.glance3d.app)
- 赞助支持：[glance3d.app/thanks](https://glance3d.app/thanks)

## 致谢

Glance3D 源自 F3D。F3D 最初由 Kitware SAS、Joachim Pouderoux、Michael Migliore 和 Mathieu Westphal 创建，之后由 F3D-APP Foundation 维护。Glance3D 依赖众多优秀开源项目，包括 VTK、OCCT、Assimp、Alembic、Draco、web-ifc、OpenUSD、OpenVDB、PDAL、OSPRay 和 ImGui。

## 许可证

Glance3D 使用 3-Clause BSD License 发布，详见 [LICENSE.md](LICENSE.md)。

项目集成或依赖的第三方库和工具遵循各自许可证，详见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
