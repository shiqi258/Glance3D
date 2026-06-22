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

**默认构建是线程版（Emscripten pthreads）**：解析跑 Web Worker，网页打开大文件时浏览器主线程不卡（DOM 可响应、显示进度）。代价是**线程版 wasm 靠 SharedArrayBuffer，站点必须发 COOP/COEP 跨源隔离头才能加载**（`examples/libf3d/web` 的 Vite dev/preview 已自动发；生产部署须自备，静态托管用 `coi-serviceworker` 垫片）。线程版依赖一个用 `VTK_WEBASSEMBLY_THREADS=ON` 编出的 VTK（路径 `deps.local.json` 的 `wasmThreadsDepsDir`），产物在 `_wasm_build_threads/`。

无法发 COOP/COEP 的环境可 **opt-out 回单线程**：`F3D_WASM_THREADS=OFF npm run build`（用 `wasmDepsDir` 的非线程 VTK，产物在 `_wasm_build/`）。架构与实测见自动记忆 `load-perf-breakdown` / `load-blocking-mainthread`。

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


## 自动视觉自检（无头渲染出图 + 读图分析）

桌面端**可全自动跑视觉验证，无需用户开窗交互、无需用户操作**：命令行 `--output` 把"加载文件→渲染→写 PNG→退出"一气呵成，Claude 随后直接 `Read` 这张 PNG 即可用视觉判断结果。改了渲染/加载逻辑后想确认效果时，直接执行下面的步骤即可，**不必请用户手动截图或描述**。

**前提**：已有现成可执行文件 `build/bin_Release/f3d.exe`（注意文件名仍是 `f3d.exe`；这是 Release 非测试构建，故没有 ctest 用例）。若不存在，先 `cmake --build --preset native-local`。测试素材在 `testing/data/`（glb/obj/ply/stl/step/vtu 等各格式齐全）。

**步骤**（命令从仓库根目录执行）：

```bash
# 1) 无头渲染出图
build/bin_Release/f3d.exe testing/data/f3d.glb --output shot.png --resolution 800,600 -x -g
# 2) 用 Read 工具读 shot.png 做视觉分析
# 3) 看完删除临时图：rm -f shot.png
```

常用开关（按需叠加，全部 `--help` 可查）：

- `--output <png>` 渲染到文件（核心，触发无头模式）
- `--resolution W,H` 固定分辨率
- `--no-background` 透明背景出图
- `-x` 坐标轴 gizmo、`-g` 地面网格、`--up <dir>` 上方向
- `-D libf3d.option=value` 任意调渲染参数（相机/材质/光照…），可多次
- `--no-render --verbose` 只解析、打印文件信息不渲染（验证"能否正确读入"而非"长什么样"时用）
- `--rendering-backend <auto|wgl|egl|osmesa>` 切换渲染后端（默认 `auto`，走真实 GPU/OpenGL）

**两种用法定位**：① 即时视觉抽查——上面的 `--output` + 读图，用于"对不对/像不像"的人眼级确认；② 严格像素回归——走 `## 测试` 的 ctest 基线比对（需 `dev` 预设开 `BUILD_TESTING` 重新构建）。批量冒烟时可对 `testing/data/` 多个文件循环出图逐张读。

### 交互回放 / 命令脚本驱动（模拟手动操作）+ 正误判断

上面的 `--output` 是静态出图，**不走 UI/鼠标/事件循环**。要验证"和用户手动操作一样"的交互，用下面两条（现成 Release 二进制即支持，无需重建）：

- `--interaction-test-play <log>`：回放录制的**真实输入事件**（鼠标移动/点击控件/滚轮缩放/拖拽旋转平移/键盘/拖放文件），走和真人**同一条** VTK interactor 路径。录制在 `testing/recordings/`，纯文本格式 `事件名 x y ctrl shift keycode repeat keysym`，键盘/控制台/相机拖拽序列可手写或裁剪；新建"精确点击某 UI 控件"需实时布局坐标，从零不可靠，优先复用已有录制或让用户录一次。
- `--command-script <file>`：纯文本高层命令，走和控制台（Esc）**同一套命令分发**，可从零写。命令清单见 `doc/user/07-COMMANDS.md`，例：`set_camera top` / `toggle render.axes_grid.enable` / `cycle_coloring array` / `take_screenshot out.png`。

**正误判断**：每个录制对应一张人工验证基线 `testing/baselines/<测试名>.png`。

- 客观比对：加 `--reference <baseline> [--reference-threshold <t>]`，二进制算误差并打印（如 `图像对比成功，误差差异为：0.0003`）。**比对通过时只打印误差、不写 `--output`；失败才写图供检查**。
- 语义判断：单独用 `--output`（不带 `--reference`）强制出图，再 `Read` 实际图 + 基线图，肉眼判断交互效果是否发生。

复刻 ctest 的固定调用范式（默认分辨率 `300,300`，改了会和基线尺寸不符）：

```bash
build/bin_Release/f3d.exe testing/data/<data> --no-config --resolution=300,300 \
  --interaction-test-play=testing/recordings/<Name>.log \
  --output=<actual>.png --reference=testing/baselines/<Name>.png --rendering-backend=auto [ARGS]
```

每个录制对应的数据文件、`ARGS`、是否需 UI、以及行尾 `#...`（该录制的人类可读手势）见 `application/testing/tests.interaction.cmake`。

**坑**：① 误差大 ≠ 一定是代码缺陷——VTK 版本 / GPU / 分辨率差异会产生非缺陷性误差，须结合读图 + `--verbose=debug` 判断，别只看数字。② UI 类录制（控制台 / 场景树 / 点击控件）需带 `F3D_MODULE_UI` 的构建（现成 Release 二进制已含）。


## 日志 / 排查

桌面应用**每次启动都会自动写一个日志文件**，无需任何开关。排查问题时直接去读最新的日志文件即可，不必向用户索要。

- **位置**：用户缓存目录下的 `Glance3D/logs/`
  - Windows：`%LOCALAPPDATA%\Glance3D\logs\`
  - Linux：`$XDG_CACHE_HOME/Glance3D/logs/` 或 `~/.cache/Glance3D/logs/`
  - macOS：`~/Library/Caches/Glance3D/logs/`
- **文件名**：`g3d_YYYYMMDD_HHMMSS_mmm.log`，每次运行一个独立文件，按时间戳排序。默认保留最近 10 个（`G3D_LOG_KEEP` 可改）。
- **内容**：记录**全部级别（含 DEBUG）**，**与控制台 `--verbose` 无关**——即使用户没开 verbose，文件里也有完整 debug 链路。含毫秒时间戳、级别标签、启动命令行；WARN/ERROR 立即落盘，能保留崩溃前最后的错误。

读最新一条日志（Windows PowerShell）：

```powershell
Get-Content (Get-ChildItem "$env:LOCALAPPDATA\Glance3D\logs\g3d_*.log" | Sort-Object Name | Select-Object -Last 1).FullName
```

环境变量开关：`G3D_LOG_FILE=0` 关闭文件日志；`G3D_LOG_DIR=<path>` 改目录；`G3D_LOG_KEEP=<n>` 改保留数。

实现见 `application/F3DLogFile.cxx`（通过 `f3d::log::forward()` 挂接）。**已知限制**：VTK 第三方内部告警（`vtkWarningMacro` 等）直接写 `vtkOutputWindow`、不经过 `F3DLog::Print`，故不进文件——这类只在 `--verbose=debug` 的控制台输出里能看到。

### 网页查看器（WebAssembly）日志

网页查看器在 `examples/libf3d/web`（Vite + 上游 wasm 绑定）。浏览器 JS 不能直接写本地磁盘，所以网页日志靠**本地采集进程**落盘。**排查网页问题时直接读最新的 `g3d_*_web.log` 即可，不必向用户索要。**

- **位置 / 文件名**：和桌面**同一目录**（`%LOCALAPPDATA%\Glance3D\logs\` 等），文件名 `g3d_YYYYMMDD_HHMMSS_mmm_web.log`，每次采集一个独立文件，沿用同一套 `G3D_LOG_FILE` / `G3D_LOG_DIR` / `G3D_LOG_KEEP` 开关（默认保留 10）。`_web.log` 后缀用于和桌面 `g3d_*.log` 区分。
- **行格式**：`[HH:MM:SS.mmm] [LEVEL] [来源] 消息`。`来源` 取值 `console`（JS/WASM 经 console）、`rendering`（**WebGL/GPU 底层告警**）、`network` / `security` / `deprecation`、`exception`（未捕获异常）等。

启动（一条命令同时起 dev server + 带调试端口的浏览器 + CDP 采集器）：

```bash
cd examples/libf3d/web
npm run dev:logged
```

读最新一条网页日志（Windows PowerShell）：

```powershell
Get-Content (Get-ChildItem "$env:LOCALAPPDATA\Glance3D\logs\g3d_*_web.log" | Sort-Object Name | Select-Object -Last 1).FullName
```

**采集机制（两层）**：

- **主通道 = CDP DevTools 镜像**（`scripts/collect-browser-devtools-logs.mjs`，`dev:logged` 自动拉起）。通过 Chrome DevTools 协议订阅 `Log.entryAdded` / `Runtime.consoleAPICalled` / `Runtime.exceptionThrown`，把**整个 DevTools 控制台**镜像到 `g3d_*_web.log`——**这是唯一能抓到浏览器原生 WebGL 底层日志的路径**。落点/格式逻辑在 `scripts/glance3d-weblog.mjs`（桌面 `F3DLogFile` 的 JS 移植）。
- **辅通道 = 页面内钩子**（`src/browser-console-log.js` + `vite.config.js` 中间件，`browser-log.config.json` 控制）。劫持 `console.*` / `onerror` / `unhandledrejection`，并经 `main.js` 的 `Module.Log.forward()` 接入 f3d 日志，批量 POST 到 Vite，写 `logs/browser-console.jsonl`。**已知限制**：猴补丁 `console.*` **抓不到浏览器原生日志**（WebGL/CSP/弃用等由浏览器引擎直接写控制台，不经过 JS）——这类只有主通道（CDP）能记录。普通 `npm run dev` 只有辅通道。

