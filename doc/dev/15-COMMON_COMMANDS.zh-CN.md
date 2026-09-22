# 开发常用命令

本文记录开发中高频使用的命令。下面的命令以 Windows PowerShell 为准，默认已经位于项目根目录，复制后可直接执行。

## 依赖路径配置（一次性，免环境变量）

依赖（VTK、WASM 版 VTK、Emscripten、Ninja 等）的安装路径每台机器都不同，因此**不写死在仓库里**，而是放在两个本机配置文件中（已在 `.gitignore` 忽略，不会提交）。换机器时只需各复制一份模板并改路径即可，之后所有构建命令都不再需要设置任何环境变量。

```powershell
# 原生桌面版：复制模板后，填入本机 VTK 安装路径（含 vtk-config.cmake 的目录）
Copy-Item CMakeUserPresets.json.example CMakeUserPresets.json
# WebAssembly：复制模板后，填入本机 WASM 版 VTK 安装前缀、emsdk、ninja 路径
Copy-Item webassembly\deps.local.example.json webassembly\deps.local.json
```

> 仍然支持用环境变量覆盖（`VTK_DIR`、`F3D_WASM_DEPS_DIR` 等），环境变量优先级更高，方便 CI 使用。

## 构建并运行原生桌面版

使用 CMake 预设（采用 Visual Studio 生成器，无需手动 `vcvars64.bat`）：

```powershell
cmake --preset native-local            # 配置：首次、或改了 CMake 选项后
cmake --build --preset native-local    # 构建：日常改完代码跑这一条（Release）
build\bin_Release\glance3d.exe testing\data\f3d.glb   # 运行
```

> **必须在项目根目录执行。** `cd build` 之后再跑 `--preset` 会静默失败（没有任何报错），实际验证的是旧 `f3d.dll`；确认构建生效看 `build\bin_Release\f3d.dll` 的时间戳（exe 壳不重链是正常的）。

> 目录里若残留改名前的 `f3d.exe`，是陈旧产物，加载新 `f3d.dll` 会直接崩溃，勿用。

不开窗看渲染效果——`--output` 无头出图（开关详见 `CLAUDE.md`）：

```powershell
build\bin_Release\glance3d.exe testing\data\f3d.glb --output shot.png --resolution 800,600 -x -g
```

## 构建 WebAssembly

```powershell
npm run build
# 产物：dist\f3d.js + dist\f3d.wasm
```

`npm run build` 会自动根据 `webassembly\deps.local.json` 激活 Emscripten 并把 Ninja 加入 `PATH`，无需手动执行 `emsdk_env`。

## 网页查看器


### 构建 WebAssembly 包

```powershell
npm run build
```


### 启动网页查看器开发服务器

```powershell
npm --prefix "examples\libf3d\web" run dev -- --host 127.0.0.1 --port 5175
```

## 网页查看器日志

网页查看器的浏览器控制台日志默认写入：

```text
logs\browser-console.jsonl
```

### 清空网页查看器日志

```powershell
npm --prefix "examples\libf3d\web" run clear:browser-logs
```


### 用 Edge 启动远程调试并打开网页查看器

```powershell
Start-Process "msedge" -ArgumentList "--remote-debugging-address=127.0.0.1", "--remote-debugging-port=9222", "--user-data-dir=$env:TEMP\glance3d-edge-cdp", "http://127.0.0.1:5175"
```

### 采集浏览器 DevTools 日志

```powershell
npm --prefix "examples\libf3d\web" run collect:browser-logs -- --host=127.0.0.1 --port=9222 --url=127.0.0.1:5175
```

### 采集浏览器 DevTools 日志到指定文件

```powershell
npm --prefix "examples\libf3d\web" run collect:browser-logs -- --host=127.0.0.1 --port=9222 --url=127.0.0.1:5175 --log-file="../../../logs/browser-console.jsonl"
```
