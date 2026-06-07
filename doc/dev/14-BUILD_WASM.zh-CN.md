# 构建 WebAssembly

Glance3D 可以通过 Emscripten 构建为 WebAssembly，从而嵌入浏览器中运行。
这个构建产物用于 [网页查看器](https://glance3d.app/viewer)，并覆盖了大部分 [libf3d 公共 API](../libf3d/04-LANGUAGE_BINDINGS.md)。
仓库中也提供了一个简单示例：[examples/libf3d/web](../../examples/libf3d/web)。

本文说明如何在 Linux 或 Windows 上使用本地 Emscripten 环境构建 VTK 和 Glance3D。

## 构建

### 准备构建环境

请先在本机安装 `npm`、CMake、Ninja 和 Emscripten SDK。确保当前终端已经激活 Emscripten 环境，并且 `emcmake` 可以在 `PATH` 中找到。

Windows 上的一种典型安装方式如下：

```powershell
winget install OpenJS.NodeJS.LTS Kitware.CMake Ninja-build.Ninja Git.Git Python.Python.3.12
git clone https://github.com/emscripten-core/emsdk.git C:\emsdk
cd C:\emsdk
.\emsdk install latest
.\emsdk activate latest
.\emsdk_env.ps1
```

Glance3D 还需要 C++ 依赖库的 WebAssembly 版本。最少需要提供 WebAssembly 版 VTK。如果需要完整的网页查看器插件集，还需要提供 WebAssembly 版 Assimp、Draco、OpenCASCADE、web-ifc 和 WebP。

将 `F3D_WASM_DEPS_DIR` 设置为这些 WebAssembly 依赖库的安装前缀目录：

```powershell
$env:F3D_WASM_DEPS_DIR = "D:\wasm-deps\install"
```

这些依赖必须使用和 Glance3D 构建时相同的 Emscripten SDK 编译。不能复用 Windows、macOS 或 Linux 的原生库来构建 WebAssembly。

### 构建 Glance3D

在仓库根目录运行：

```sh
npm run build
```

构建完成后会生成 `dist` 目录，其中包含 `f3d.js` 和 `f3d.wasm`。

默认情况下，本地构建会关闭可选插件，以减少本地依赖要求。如果想构建与原 Docker 流程等价的完整插件集，请先把可选依赖的 WebAssembly 版本安装到 `F3D_WASM_DEPS_DIR`，然后运行：

```powershell
$env:F3D_WASM_FULL_PLUGINS = "ON"
npm run build
```

如果需要选择其他 CMake 生成器，可以设置 `F3D_WASM_CMAKE_GENERATOR`。

## 运行测试

完成构建后，在仓库根目录运行：

```sh
npm test
```

## 集成到其他项目

可以生成一个本地 npm 包，供其他 JavaScript 项目使用：

```sh
npm pack
```

该命令会构建并生成一个 `f3d-vX.X.X.tgz` 文件。
其他项目可以通过下面的方式安装这个本地包：

```sh
npm install f3d-vX.X.X.tgz
```
