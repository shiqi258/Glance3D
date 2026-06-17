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

## 国际化（i18n）

桌面端多语言方案＝**源串即 key（gettext 学派）＋ ICU MessageFormat 子集**。首批 en + zh-CN，默认按系统语言自动选，`--lang` / `G3D_LANG` / config 可覆盖。

**架构（保持不动）**：翻译核心 `G3DLocaleCore`（单例）在最底层模块 `vtkext/private/module/G3DLocaleCore.{h,cxx}`，三层都能直达。libf3d 导出公共门面 `g3d::locale`（`library/public/g3dLocale.h`）转发到核心：application 只用门面，library/vtkext internal 直接用 `G3DLocaleCore`。词表 `resources/locales/{en,zh-CN}.json` 经 `f3d_embed_file` 嵌入（`G3DLocaleEn`/`G3DLocaleZhCN`），外部 `share/f3d/locales/<lang>.json` 可热覆盖（`LoadFromDirectory`）。缺失回退链：active → en → key 本身。

**key 体系＝英文源串本身**（`translate("Number of points: {n, number}")`）。**en.json 仅含 `_meta.*`**——英文就是 key，无需维护完整 en master；只维护 `zh-CN.json`（镜像同 key → 中文）。**不要造 `ui.xxx`/`cli.xxx` 式符号 key**（那是将来 Option C 的事，见末尾）。CLI `--help` 在 `F3DOptionsTools` 把每个 `cli-options.json` 的 `helpText` 当 key 查译。

**消息格式＝ICU MessageFormat 子集**，由自研解析器 `G3DLocaleCore::FormatMessage(tmpl, args, lang)` 实现（无外部依赖，桌面 + WASM 通用）。支持：

- `{name}` —— 命名参数替换（与旧 `{name}` 完全兼容）。
- `{n, number}` —— 整数千分位分组（按 lang 取分隔符，en/zh 为 `,`）。
- `{n, plural, =0{…} one{# …} other{# …}}` —— 复数；`=N` 精确匹配优先，否则按 `PluralCategory(lang,n)` 选类目，缺失回退 `other`；子消息内 `#` = 格式化后的数字，可再嵌 `{name}`。
- `{x, select, key{…} other{…}}` —— 按参数字符串值选分支。
- 健壮性：括号不配对/未知类型/缺参 → 原样输出不崩。**不支持** ICU 单引号转义，消息体内别写裸 `{`/`}`。

参数仍是 `Args = vector<pair<string,string>>`（值为字符串，number/plural 解析时按需转数字），全部调用点签名不变。

**加一种语言**：① `SupportedLanguages()` 加码；② `NormalizeLocale()` 认系统 locale 拼写；③ 加 `resources/locales/<lang>.json` 并在 `vtkext/private/module/CMakeLists.txt` 加一组 `f3d_embed_file`；④ `PluralCategory()` 把该语言映射到复数族（germanic=只有 1 是 one；french=0 和 1 是 one；asian=恒 other）；⑤ 需要时在 `GroupingSeparator()` 加分隔符（de=`.`、fr=窄空格）；⑥ CJK 语言由 `NeedsCJK()` 识别并合并 CJK 字体（见下方"CJK 字体"约定）。

**CJK 字体**：`resources/fonts/NotoSansSC-Regular.ttf`（SIL OFL），仅 CJK 语言激活时 merge；字形范围 = ImGui 常用简中 + **当前词表实际字符**（否则生僻字显示成方框）。路径经 `g3d::locale::setCJKFontPath` 设入核心，actor（`vtkF3DImguiActor`）读 `G3DLocaleCore`。

**校验**：`node scripts/check-locales.mjs` —— ① 覆盖/孤儿（缺 zh 翻译会 fail）；② **ICU lint**：源 key 与 zh 值的括号配对、plural/select 必含 `other`、**key 与 value 的参数名一致**。

**单测**：`ctest -R TestG3DLocaleFormat -VV`（需 `BUILD_TESTING=ON`，如 `dev` 预设；纯字符串逻辑，`NO_DATA`）。覆盖 argument/number/plural(en·fr·zh)/select/`#`/`=N`/坏输入。

**何时升级到符号 key（Option C，暂不做）**：接外部翻译外包 / 上 XLIFF 工具链 / 语言数破 8–10、或需上下文消歧时，再把 230 调用点 re-key 成 `域.区域.名` 并维护完整 en master。本解析器届时 100% 复用。

实现见 `vtkext/private/module/G3DLocaleCore.cxx`。

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

