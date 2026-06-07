[![License: BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-blue.svg)](LICENSE.md)
[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-2.1-4baaaa.svg)](CODE_OF_CONDUCT.md)

[English](README.md) | 简体中文

# Glance3D

Glance3D 是一款专注于快速文件预览和丰富预览设置交互的 3D 查看器。

项目基于 [F3D](https://f3d.app/) 演进，保留了 `f3d`/`libf3d` 原有的核心能力：广泛的格式支持、基于 VTK 的渲染、命令行自动化、配置文件、缩略图生成，以及可嵌入的渲染 API。在这个基础上，Glance3D 更强调面向实际工作流的预览体验，让用户更直接、更可控地调整模型预览效果。

![Glance3D rendering example](https://media.githubusercontent.com/media/glance3d-app/glance3d-website/refs/heads/main/static/images/typical.png)

## 项目定位

Glance3D 面向需要频繁检查 3D 资产的人：既要快速打开文件，也要能控制文件以什么方式被预览。

- 快速打开和预览 3D 模型、CAD/BIM 数据、点云、体数据、科学数据集、图像和环境贴图。
- 调整材质、贴图、边线、光照、HDRI、标量着色、体渲染、色调映射、SSAO 和可选光线追踪等预览相关设置。
- 通过交互控件、快捷键、拖放、命令脚本和配置文件，让预览流程可重复、可沉淀。
- 生成截图和文件管理器缩略图，并为缩略图使用单独的预览配置。
- 复用 `libf3d` 渲染能力，支持 C++、C、Python、Java 和 JavaScript/WebAssembly。

## 与 F3D 的关系

Glance3D 建立在原 F3D 代码库之上。当前迁移阶段，一些可执行文件名、CMake 选项、配置路径和库 API 仍然使用 `f3d` 或 `F3D_` 命名。这是为了兼容现有生态和构建系统。

实际使用时可以这样理解：

- 项目名称和产品定位是 Glance3D。
- 当前命令行可执行文件仍是 `f3d`。
- 核心渲染库仍以 `libf3d` 暴露。
- 许多构建选项仍保留 `F3D_*` 命名。

## 快速开始

安装 Glance3D 后，可以直接从文件管理器打开模型，也可以使用命令行：

```bash
f3d /path/to/file.ext
```

将当前预览保存为图片：

```bash
f3d /path/to/file.ext --output=/path/to/preview.png
```

查看命令行帮助：

```bash
f3d --help
```

打开文件后，按 `H` 可查看快捷键列表。

常用交互：

- 鼠标左键拖动：旋转视角。
- 鼠标滚轮或右键拖动：缩放。
- 鼠标中键拖动：平移。
- `Enter`：重置相机。
- `Space`：播放或暂停动画。
- `G`：切换网格。
- `K`：切换交互模式。

更多入门说明见 [快速开始文档](doc/user/01-QUICKSTART.md)。

## 获取 Glance3D

推荐优先使用项目下载页或发布页提供的预构建包：

- [glance3d.app/download](https://glance3d.app/download)
- [GitHub Releases](https://github.com/glance3d-app/glance3d/releases)

## 预览配置

Glance3D 可以通过命令行选项、配置文件和交互命令驱动，因此可以为不同资产类型或工作流保存不同的预览预设。

示例：

```bash
# 预览模型并输出截图
f3d model.glb --output=preview.png

# 列出当前构建启用的读取器
f3d --list-readers

# 使用指定交互模式
f3d drawing.png --interaction-style=2d

# 配置读取器参数，例如控制 VDB 体数据降采样
f3d volume.vdb -DVDB.downsampling_factor=0.5

# 播放所有动画
f3d animated.glb --animation-indices=-1
```

相关文档：

- [命令行选项](doc/user/03-OPTIONS.md)
- [交互方式](doc/user/04-INTERACTIONS.md)
- [配置文件](doc/user/06-CONFIGURATION_FILE.md)
- [命令和脚本](doc/user/07-COMMANDS.md)
- [桌面集成和缩略图](doc/user/11-DESKTOP_INTEGRATION.md)

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
- VTK 9.4.0 或更新版本

典型本地构建：

```bash
cmake -S /path/to/source -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

使用仓库预设：

```bash
cmake --preset=dev /path/to/source
cmake --preset=vcpkg /path/to/source
```

常用 CMake 选项：

- `F3D_BUILD_APPLICATION`：构建 Glance3D 应用程序。
- `BUILD_TESTING`：启用测试。
- `F3D_MODULE_UI`：启用 ImGui UI 支持。
- `F3D_MODULE_RAYTRACING`：在 VTK 支持 OSPRay 时启用光线追踪。
- `F3D_PLUGIN_BUILD_ASSIMP`：启用 Assimp 格式支持。
- `F3D_PLUGIN_BUILD_OCCT`：启用 Open CASCADE 格式支持。
- `F3D_PLUGIN_BUILD_USD`：启用 OpenUSD 格式支持。
- `F3D_PLUGIN_BUILD_VDB`：启用 OpenVDB 格式支持。
- `F3D_BINDINGS_PYTHON`：构建 Python 绑定。
- `F3D_BINDINGS_C`：构建 C 绑定。
- `F3D_BINDINGS_JAVA`：构建 Java 绑定。

完整构建说明见 [构建文档](doc/dev/05-BUILD.md)，测试说明见 [测试文档](doc/dev/06-TESTING.md)。

## 项目结构

```text
application/     命令行和桌面应用入口
library/         libf3d 核心 C++ API
c/               C 语言绑定
python/          Python 绑定
java/            Java 绑定
webassembly/     JavaScript/WebAssembly 绑定和测试
plugins/         可选格式读取插件
vtkext/          基于 VTK 的扩展模块
doc/             用户、开发者和 libf3d 文档
examples/        libf3d 示例代码
testing/         测试数据、交互脚本和渲染基线
resources/       图标、配置、补全脚本和资源文件
cmake/           CMake 模块和安装配置
```

## 文档导航

- [用户入门](doc/user/01-QUICKSTART.md)
- [支持格式](doc/user/02-SUPPORTED_FORMATS.md)
- [命令行选项](doc/user/03-OPTIONS.md)
- [交互方式](doc/user/04-INTERACTIONS.md)
- [配置文件](doc/user/06-CONFIGURATION_FILE.md)
- [插件说明](doc/user/12-PLUGINS.md)
- [libf3d 概览](doc/libf3d/01-OVERVIEW.md)
- [开发上手](doc/dev/04-GETTING_STARTED.md)
- [架构说明](doc/dev/08-ARCHITECTURE.md)
- [更新日志](doc/CHANGELOG.md)

## 参与贡献

Glance3D 是社区驱动项目，欢迎提交问题、改进文档、修复缺陷、实现新功能，或一起打磨预览工作流。

贡献前请阅读 [贡献指南](CONTRIBUTING.md)、[行为准则](CODE_OF_CONDUCT.md) 和 [AI 使用政策](AI_POLICY.md)。

## 致谢

Glance3D 源自 F3D。F3D 最初由 [Kitware SAS](https://www.kitware.eu/)、Joachim Pouderoux、Michael Migliore 和 Mathieu Westphal 创建，之后由 F3D-APP Foundation 维护。Glance3D 依赖众多优秀开源项目，包括 [VTK](https://vtk.org/)、[OCCT](https://dev.opencascade.org/)、[Assimp](https://www.assimp.org/)、[Alembic](http://www.alembic.io/)、[Draco](https://google.github.io/draco/)、[web-ifc](https://github.com/ThatOpen/engine_web-ifc)、[OpenUSD](https://openusd.org/release/index.html)、[OpenVDB](https://www.openvdb.org/)、[PDAL](https://pdal.org)、[OSPRay](https://www.ospray.org/) 和 [ImGui](https://github.com/ocornut/imgui/)。

## 许可证

Glance3D 使用 3-Clause BSD License 发布，详见 [LICENSE.md](LICENSE.md)。

第三方库和工具详见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
