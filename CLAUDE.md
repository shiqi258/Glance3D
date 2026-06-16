# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概览

Glance3D 是一款专注于快速文件预览的 3D 查看器，**由 [F3D](https://f3d.app/) 派生而来**，基于 VTK 渲染。核心能力（格式支持、命令行自动化、配置文件、缩略图、可嵌入渲染 API）来自上游 `f3d`/`libf3d`。


## 构建

需要 C++20 编译器、CMake、VTK ≥ 9.4。本仓库用 CMake Preset 驱动，**机器相关路径写在 git-ignored 的本地文件里**（本机已配置好）：

- `CMakeUserPresets.json` —— 提供本机 `VTK_DIR`（继承自 `CMakePresets.json` 的 `native` 预设）。模板见 `CMakeUserPresets.json.example`。
- `webassembly/deps.local.json` —— 提供 wasm 依赖、emsdk、ninja 的本机路径。模板见 `webassembly/deps.local.example.json`。

### 桌面应用 / 库（Windows，VS2022）

`native` 预设用 Visual Studio 生成器，无需手动配置 vcvars/Ninja。本机用 `native-local`：

```bash
cmake --preset native-local            # 配置（读取本机 VTK_DIR）
cmake --build --preset native-local    # 构建（Release）
```


### 开发 / 测试构建

`dev` 预设是 Debug + 开启测试 + 严格编译（`F3D_STRICT_BUILD`）：

```bash
cmake --preset dev
cmake --build build
```

### WebAssembly / JavaScript

用 `webassembly/build-local.mjs` 包装，**免手动 export 环境变量**——它会自动激活 emsdk、把 ninja 加进 PATH（路径来自 `deps.local.json`）。npm 脚本：

```bash
npm run build     # emcmake 配置 + 构建 + 打包到 dist/
npm run clean      # 清理 _wasm_build/ 和 dist/
npm run test       # ctest 跑 wasm 测试
```

## 测试

测试用 CTest，主要是**渲染输出图与 `testing/baselines/` 基线图做差异比较**（超阈值即失败）。需要 `BUILD_TESTING=ON`（默认关），克隆时需 git LFS。

```bash
ctest                       # 全部
ctest -R PLY                # 按名字匹配单个测试
ctest -R TestName -VV       # 单测 + 详细输出（新基线失败后看 build/Testing/Temporary/TestName.png）
ctest -L bindings           # 按标签：application/libf3d/bindings/c/java/python/js/module/<插件>/<扩展名>
ctest -L assimp -L piped    # 标签可叠加
```

新增应用层测试：在 `application/testing/` 的 CMakeLists 里加 `f3d_test(NAME ... DATA ... ARGS ...)`，首次运行会失败并生成图，目检后把图放进 `testing/baselines/` 再跑过。所有 `f3d_test` 关键字见 `cmake/f3dTest.cmake`。

## 日志 / 排查

桌面应用**每次启动都会自动写一个日志文件**，无需任何开关。排查问题时直接去读最新的日志文件即可，不必向用户索要。

- **位置**：用户缓存目录下的 `f3d/logs/`
  - Windows：`%LOCALAPPDATA%\f3d\logs\`
  - Linux：`$XDG_CACHE_HOME/f3d/logs/` 或 `~/.cache/f3d/logs/`
  - macOS：`~/Library/Caches/f3d/logs/`
- **文件名**：`f3d_YYYYMMDD_HHMMSS_mmm.log`，每次运行一个独立文件，按时间戳排序。默认保留最近 10 个（`F3D_LOG_KEEP` 可改）。
- **内容**：记录**全部级别（含 DEBUG）**，**与控制台 `--verbose` 无关**——即使用户没开 verbose，文件里也有完整 debug 链路。含毫秒时间戳、级别标签、启动命令行；WARN/ERROR 立即落盘，能保留崩溃前最后的错误。

读最新一条日志（Windows PowerShell）：

```powershell
Get-Content (Get-ChildItem "$env:LOCALAPPDATA\f3d\logs\f3d_*.log" | Sort-Object Name | Select-Object -Last 1).FullName
```

环境变量开关：`F3D_LOG_FILE=0` 关闭文件日志；`F3D_LOG_DIR=<path>` 改目录；`F3D_LOG_KEEP=<n>` 改保留数。

实现见 `application/F3DLogFile.cxx`（通过 `f3d::log::forward()` 挂接）。**已知限制**：VTK 第三方内部告警（`vtkWarningMacro` 等）直接写 `vtkOutputWindow`、不经过 `F3DLog::Print`，故不进文件——这类只在 `--verbose=debug` 的控制台输出里能看到。

