# PRD: 桌面应用可执行文件更名 + 启动窗口几何（默认值/记忆/防重置）

> 状态：待评审（无人值守模式下按推荐默认值编写，所有待拍板项见「9. Open Questions」）
> 范围：仅桌面应用（`application/` + 少量 `library/` 共享核心 API 增补）；网页端不涉及
> 验证手段说明：本项目为桌面 C++ 应用，无浏览器可验。所有 UI/行为类验收一律使用仓库既定自动化手段：无头渲染出图 + `Read` 读图、`%LOCALAPPDATA%\Glance3D\logs\` 日志断言、`--interaction-test-play` 回放 + `--reference` 基线比对、PowerShell P/Invoke（`GetWindowRect` 等）查询真实窗口几何。

## 1. Introduction / Overview

Glance3D 由 F3D 派生。项目身份（`project(Glance3D)`、窗口标题、macOS bundle id `app.glance3d.Glance3D`、Linux desktop entry `Name=Glance3D`）已完成品牌化，但**可执行文件仍叫 `f3d.exe`**（CMake 目标 `f3d` 的默认输出名，[application/CMakeLists.txt:231](application/CMakeLists.txt)）。用户机器上另装有官方 F3D，导致「按名字找程序」（人查任务管理器/开始菜单、脚本与自动化按文件名定位）经常命中错误的 `f3d.exe`。

同时，启动窗口体验有三处缺陷：

1. **默认位置差**：`--position` 无默认值（`F3DOptionsTools.h:51` 默认空串）→ 不设位置 → VTK/OS 默认落在屏幕左上角；
2. **无记忆**：窗口尺寸/位置不跨会话保存，每次启动都要手动拖放调整；
3. **加载完成重置**：`F3DStarter::Start()` 在 `LoadFileGroup()` **之后**第二次调用 `ApplyPositionAndResolution()`（[application/F3DStarter.cxx:1338](application/F3DStarter.cxx:1338)，首次在 :1319），把 CLI/默认分辨率（默认 `1000, 600`，`F3DOptionsTools.h:50`）重新写回窗口——大文件加载期间用户手动调好的尺寸/位置在加载结束瞬间被覆盖。

本 PRD 定义：① 产物更名为 `glance3d`；② 参照业内成熟做法（VS Code / Chrome / Blender 等：首启居中 + 合理默认尺寸、记忆几何 + 显示器有效性校验、显式 CLI 参数优先）重做启动窗口几何策略，并根治加载后重置。

## 2. Goals

- 本机上按名字定位程序**唯一命中**本项目产物：`glance3d.exe`（Windows 任务管理器/文件属性同时显示品牌化元数据）。
- 首次启动（无记忆、无显式 CLI 几何）：窗口在光标所在显示器**居中**，尺寸自适应工作区、观感合理。
- 后续启动：**恢复上次会话的尺寸/位置（及最大化状态，Windows 完整支持）**，并对显示器变化做有效性校验。
- 窗口几何一经建立（恢复/默认/用户手调），程序自身**不再自动改写**——加载完成不重置。
- **零自动化回归**：无头出图尺寸不变、全部 `testing/recordings/` 回放路径不受影响、ctest 基线不受影响。

## 3. User Stories

依赖关系：US-001→US-002/003/004 可并行；US-005 是 US-006/007 的前置；US-008 可与 US-006/007 同批实现（同一处代码）。

### US-001: 构建产物更名为 glance3d
**Description:** 作为用户，我希望本项目桌面程序的文件名是 `glance3d.exe`，从而在任务管理器、开始菜单、脚本与自动化中不再和其它已安装的 `f3d.exe` 混淆。

**Acceptance Criteria:**
- [ ] CMake 目标名保持 `f3d` / `f3d-console` 不变，仅设 `OUTPUT_NAME` 为 `glance3d` / `glance3d-console`（理由见 7.1；`$<TARGET_FILE:f3d>` 及 `f3dTargets` 导出零波及）
- [ ] `cmake --build --preset native-local` 构建通过，产物为 `build/bin_Release/glance3d.exe`
- [ ] 构建树中残留的旧 `build/bin_Release/f3d.exe` 已删除（防继续误用旧产物）
- [ ] `build/bin_Release/glance3d.exe testing/data/f3d.glb --output shot.png --resolution 800,600` 出图成功且 `Read` 读图内容正常（更名未破坏资源相对路径解析，见 7.1 风险）
- [ ] `dev` 预设严格编译（`F3D_STRICT_BUILD`）通过

### US-002: Windows 版本信息资源（VERSIONINFO）
**Description:** 作为用户，我希望在任务管理器/文件属性里看到「Glance3D」的产品名与版本号，而不是无元数据的裸 exe。

**Acceptance Criteria:**
- [ ] [resources/f3d.rc](resources/f3d.rc)（现仅一行图标）补充 `VERSIONINFO` 块：`ProductName=Glance3D`、`FileDescription=Glance3D - A fast and minimalist 3D viewer`、`OriginalFilename=glance3d.exe`、`FileVersion/ProductVersion` 取自 `F3D_VERSION`（经 configure_file 注入，不写死）
- [ ] PowerShell `(Get-Item build/bin_Release/glance3d.exe).VersionInfo` 显示上述字段
- [ ] 构建通过（native-local + dev 严格编译）

### US-003: 周边引用与帮助文本同步
**Description:** 作为用户/集成方，我希望缩略图扩展、Linux 桌面集成、shell 补全、帮助文本引用的程序名与新产物一致，不留指向失效的 `f3d` 引用。

**Acceptance Criteria:**
- [ ] [winshellext/F3DThumbnailProvider.cxx:103](winshellext/F3DThumbnailProvider.cxx) 定位 `L"glance3d.exe"`
- [ ] [resources/glance3d.desktop](resources/glance3d.desktop)：`TryExec=glance3d`、`Exec=glance3d %F`、`Icon=glance3d`；[application/CMakeLists.txt](application/CMakeLists.txt) 中图标安装 `RENAME` 由 `f3d.png/f3d.svg` 改为 `glance3d.png/glance3d.svg`（Linux 无法本机实测，属声明性变更，代码评审即验收）
- [ ] shell 补全安装名（`f3d`→`glance3d`、`_f3d`→`_glance3d`、`f3d.fish`→`glance3d.fish`）与 man 生成（`f3d.1`→`glance3d.1`）同步
- [ ] `glance3d.exe --help` 的 Usage 行显示 `glance3d`（若取自硬编码则改之；取 argv[0] 则自动正确，实测确认）
- [ ] `rg -i 'f3d\.exe'` 全仓复查：除历史性文档（changelog 类）外无残留（`doc/` 的两处使用说明在 US-004 处理）
- [ ] 构建通过

### US-004: 仓库文档与 CLAUDE.md 更新
**Description:** 作为后续维护者（人或 AI），我希望文档里的可执行路径全部指向新产物，无人值守流程照抄可用。

**Acceptance Criteria:**
- [ ] `CLAUDE.md` 中所有 `build/bin_Release/f3d.exe`（无头出图、回放范式等）更新为 `glance3d.exe`，并实测文档中的出图命令可运行
- [ ] `doc/user/13-LIMITATIONS_AND_TROUBLESHOOTING.md`、`doc/dev/04-GETTING_STARTED.md`、`doc/dev/15-COMMON_COMMANDS.zh-CN.md` 中的 `f3d.exe` 引用更新
- [ ] `rg 'bin_Release/f3d\.exe'` 命中 0

### US-005: 共享核心窗口几何读取 API（前置）
**Description:** 作为应用层开发者，我需要从 `f3d::window` 读到当前窗口位置（及最大化状态），才能实现几何持久化——现有 API 只有 `setSize/getWidth/getHeight/setPosition`（[library/public/window.h:83-98](library/public/window.h)），**无 getPosition**。

**Acceptance Criteria:**
- [ ] `f3d::window` 增补 additive API：`getPosition()`（返回屏幕坐标）；Windows 平台另提供最大化状态读取（命名与形式实现时定，建议 `isMaximized()`，经 `vtkRenderWindow::GetGenericWindowId` 的 HWND 走 `GetWindowPlacement`，平台宏守卫；非 Windows 返回 false）
- [ ] 无窗口/无头/WASM 实现给出安全语义（返回 last-set 或 0,0，不崩溃），并在头文件注释写明
- [ ] 桌面端 round-trip 日志验证：`setPosition(x,y)` 后 `getPosition()` 读回一致（debug 日志行为证据）
- [ ] 两端回归：`cmake --build --preset native-local` 与 `npm run build`（wasm）均通过；`dev` 严格编译通过
- [ ] 语言绑定（js/python）不强制暴露新 API（additive，非破坏），如顺手暴露需绑定测试通过

### US-006: 首启默认几何（居中 + 自适应尺寸）
**Description:** 作为用户，第一次启动（或无记忆可用）时，我希望窗口出现在我正在用的显示器中央、尺寸合理，而不是左上角的固定小窗。

**Acceptance Criteria:**
- [ ] 交互式启动（有窗口、无显式 `--resolution/--position`、无有效记忆）：目标显示器 = 光标所在显示器（取不到则主屏）；逻辑尺寸 = 该显示器工作区（work area，扣任务栏）的 70%，并 clamp 到 [1000×600×DPI缩放, 工作区 100%]；位置 = 工作区居中
- [ ] 无头路径（`--output` / `--no-render`）行为**完全不变**：默认仍 `1000, 600`，不居中、不记忆——回归证据：`glance3d.exe testing/data/f3d.glb --output a.png` 产物为 1000×600（DPI 缩放规则与现状一致），与改动前基线图对比一致
- [ ] macOS 专用 hack（[F3DStarter.cxx:916](application/F3DStarter.cxx) 的 `setPosition(100, 800)`）由居中逻辑取代（本机无法实测 macOS，代码评审 + 平台宏守卫为验收，报告注明）
- [ ] 启动日志（debug 级）输出决策链：目标显示器、工作区 rect、计算得到的窗口 rect、决策原因（`default-centered`）——日志断言为主要证据
- [ ] 加强证据（Windows）：PowerShell P/Invoke `GetWindowRect` 读实际窗口 rect，与日志一致且中心点落在目标显示器工作区中心 ±2px
- [ ] 构建通过（native-local + dev 严格）；抽查 2 个既有 recordings 回放（均带 `--no-config --resolution=300,300`）误差达标，证明显式分辨率路径不受影响

### US-007: 窗口几何持久化与恢复
**Description:** 作为用户，我希望程序记住我上次的窗口尺寸/位置（及最大化状态），下次启动直接还原，不必每次手动拖放。

**Acceptance Criteria:**
- [ ] 交互式运行中，窗口几何变更后防抖（≤1s）保存 + 正常退出时兜底保存，写入每用户状态文件（位置见 FR-11；原子写：临时文件 + rename）
- [ ] 状态文件为带 `version` 字段的 JSON：`{version, x, y, width, height, maximized}`（物理像素）
- [ ] 再次交互式启动：恢复保存的 rect；`maximized=true` 时先按普通 rect 定位再最大化（保证还原到正确显示器；Windows 完整支持，其它平台降级为仅普通几何）
- [ ] 有效性校验：保存 rect 与当前任一显示器工作区交集 ≥100×100 且标题栏可达，尺寸超出目标工作区则 clamp；不满足 → 回退 US-006 默认居中。伪造离屏坐标（如 x=-99999）写入状态文件后启动，日志显示 `restore-invalid → default-centered`
- [ ] 抑制规则：显式 `--resolution` 或 `--position`、`--output`、`--no-render`、`--no-config` 任一存在 → 该次运行**既不恢复也不保存**（状态文件 mtime 不变为证据）——保证全部既有 ctest/回放/无头自动化不接触状态
- [ ] 开关：`G3D_WINDOW_STATE=0` 禁用、`G3D_WINDOW_STATE=<path>` 重定向状态文件（命名风格沿用 `G3D_LOG_*`；测试用它隔离）
- [ ] 损坏 JSON → WARN 日志 + 回退默认，不崩溃
- [ ] 全自动端到端证据（Windows，PowerShell 驱动）：启动（重定向状态文件）→ P/Invoke `MoveWindow` 改几何 → 正常关闭 → 断言状态文件内容 → 再启动 → 日志 `restored` rect 与 `GetWindowRect` 实测一致
- [ ] 每个决策（restored / restore-invalid / default-centered / suppressed(原因) / saved rect）都有 debug 日志行（FR-16）
- [ ] 构建通过（native-local + dev 严格）；`npm run build` 不受影响

### US-008: 消除「加载完成后窗口几何被重置」
**Description:** 作为用户，打开大文件时我会趁加载中把窗口拖到合适的位置与尺寸，我希望加载完成后窗口保持我调整的样子，而不是被程序改回默认。

**Acceptance Criteria:**
- [ ] 初始几何建立（恢复/默认/CLI 显式，US-006/007 之后统一收敛为一次性的「初始几何应用」）后，程序不再自动改写窗口几何：[F3DStarter.cxx:1338](application/F3DStarter.cxx) 处 `LoadFileGroup()` 之后的第二次 `ApplyPositionAndResolution()` 不得再覆盖既有几何（删除或改为仅在几何尚未成功建立时生效——先 `git log`/对照上游 f3d 考证该二次调用的存在原因，确认无 `--output` 时序依赖后再动，见 7.3）
- [ ] 审计其余再加载路径（dmon `--watch` 重载、`load_next_file_group`/`load_previous_file_group`、`remove_file_groups`、拖放新文件）：均不触发几何改写（代码审计 + 命令脚本实测其一：加载文件 A → P/Invoke 改窗口尺寸 → `--command-script` 执行 `load_next_file_group` → `GetWindowRect` 断言尺寸未变）
- [ ] 无头回归：默认 `--output`（1000×600）与显式 `--resolution 800,600` 的产物尺寸与改动前一致，出图 `Read` 对比无异常
- [ ] 在原第二次调用位置留 debug 日志（如 `skip geometry re-apply: already established`），作为该路径被正确跳过的运行时证据
- [ ] 既有 interaction 回放抽查（≥3 个，含 UI 类）+ 若开 `BUILD_TESTING` 跑 `ctest -L application` 相关子集，全部达标
- [ ] 构建通过（native-local + dev 严格）
- [ ] 诚实性约束：真实「加载中途拖动」场景涉及被阻塞消息循环的时序（见 7.3），若无法在无人值守下可靠自动复现，允许以「代码路径证据（日志）+ 全量回归」为验收，报告如实注明验证边界，不得伪造

## 4. Functional Requirements

### 更名（FR-1 ~ FR-7）
- **FR-1**: 桌面应用构建产物文件名必须为 `glance3d`（Windows `glance3d.exe`）；`f3d-console` 目标（`F3D_WINDOWS_BUILD_CONSOLE_APPLICATION=ON` 时）产物为 `glance3d-console.exe`。CMake 目标名（`f3d`/`f3d-console`）、导出集（`f3dTargets`）、命名空间（`f3d::`）、库名（`libf3d`）**保持不变**。
- **FR-2**: Windows 产物必须内嵌 VERSIONINFO 资源（ProductName=Glance3D、FileDescription、OriginalFilename=glance3d.exe、版本号随 `F3D_VERSION` 自动注入）。
- **FR-3**: `winshellext` 缩略图 DLL 必须按 `glance3d.exe` 定位渲染进程。
- **FR-4**: Linux 桌面集成（.desktop 的 TryExec/Exec/Icon、hicolor 图标安装名、bash/zsh/fish 补全、man 页）必须统一为 `glance3d`。
- **FR-5**: `--help` Usage、错误提示等面向用户文本中的程序名必须显示 `glance3d`。
- **FR-6**: 不提供 `f3d.exe` 兼容别名/shim（更名目的即消歧）；构建目录残留旧产物须清除。
- **FR-7**: 仓库内文档（`doc/`、`CLAUDE.md`）中的可执行文件路径必须全部更新。

### 窗口几何（FR-8 ~ FR-18）
- **FR-8**: 交互式启动且无有效记忆、无显式 CLI 几何时：目标显示器 = 光标所在显示器（fallback 主屏）；逻辑尺寸 = 工作区 70%（宽高各自计算），clamp 到 [1000×600×DPI, 工作区 100%]；位置 = 工作区居中。
- **FR-9**: 无头路径（`--output`、`--no-render`）的默认分辨率保持 `1000, 600`、无定位、无记忆——所有无人值守出图自动化的产物尺寸零变化。
- **FR-10**: 交互式运行必须持久化窗口几何：变更防抖（≤1s）保存 + 退出兜底保存；写入必须原子（temp + rename）。
- **FR-11**: 状态文件为每用户**状态**（非漫游配置）：Windows `%LOCALAPPDATA%\Glance3D\window-state.json`（与日志同根目录）；Linux `$XDG_STATE_HOME/Glance3D/`（无则 `~/.local/state/Glance3D/`）；macOS `~/Library/Application Support/Glance3D/`。不写入 `%APPDATA%\f3d`（该目录另案处理，见 Non-Goals）。
- **FR-12**: 恢复前必须校验：保存 rect 与任一显示器工作区交集 ≥100×100 且窗口标题栏可达；尺寸超出目标显示器工作区则 clamp；校验失败回退 FR-8。
- **FR-13** *(SHOULD)*: 最大化状态必须记忆与还原；Windows 用 `GetWindowPlacement/SetWindowPlacement` 语义（记普通 rect + maximized 位，还原时先定位后最大化）；Linux/macOS best-effort，做不到则降级为普通几何，不得报错。
- **FR-14**: 显式 `--resolution`/`--position`、`--output`、`--no-render`、`--no-config` 任一出现 → 该次运行禁用恢复与保存（显式意图优先 + 自动化确定性）。
- **FR-15**: 初始几何建立后，程序自身不得再自动改写窗口几何；`F3DStarter::Start()` 中 `LoadFileGroup()` 之后的二次 `ApplyPositionAndResolution()`（F3DStarter.cxx:1338）与一切再加载路径（watch 重载、文件组切换、拖放）均不得覆盖用户几何。
- **FR-16**: 恢复/默认/保存/抑制/跳过重应用的每个决策必须写 debug 级日志（含最终 rect 与原因码），作为无人值守验证与后续排查的观测依据。
- **FR-17**: 跨会话显示器/DPI 变化不做像素换算（v1 存物理像素），一律经 FR-12 校验兜底。
- **FR-18**: 多实例并发：last-writer-wins；读到损坏 JSON → WARN 日志 + 回退默认，不崩溃。

## 5. Non-Goals (Out of Scope)

- **不改** CMake 目标名、`f3dTargets` 导出、`f3d::` 命名空间、`libf3d` 库名及上游内部标识符（`F3DStarter` 等类/文件名）——控制与上游 F3D 的合并摩擦（仓库既定策略，见 [CMakeLists.txt:27-28](CMakeLists.txt)）。
- **不迁移**用户配置目录 `%APPDATA%\f3d`（[F3DSystemTools.cxx:141](application/F3DSystemTools.cxx) `applicationName = "f3d"`）：涉及存量用户配置迁移与回退策略，另立 PRD（见 Open Questions #2）。
- **不提供** `f3d` 兼容别名/软链/shim。
- **不记忆**全屏（`--fullscreen`）状态；`--fullscreen` 行为不变。
- **不做**多实例各自独立记忆、按显示器/工作区多套记忆。
- **网页端**无 OS 窗口概念，不实现本功能；仅保证 US-005 新增共享 API 在 wasm 下安全空实现并回归构建。
- **不动** `testing/baselines/` 任何基线图（本 PRD 全部改动在既有基线下应零漂移；出现漂移即回归缺陷）。

## 6. Design Considerations

业内惯例对照（取各家交集作为本设计依据）：

| 行为 | VS Code / Chrome / Blender 惯例 | 本设计 |
|---|---|---|
| 首启尺寸 | 按工作区比例 + 上下限 | 工作区 70%，clamp [1000×600×DPI, 100%] |
| 首启位置 | 活动/光标显示器居中 | 同左（fallback 主屏） |
| 记忆内容 | rect + 最大化位（记普通 rect） | 同左（FR-13） |
| 恢复校验 | 与可见显示器求交，失败回中 | ≥100×100 交集 + 标题栏可达（FR-12） |
| 显式参数 | CLI 指定几何优先于记忆 | FR-14 且不写回 |
| 保存时机 | 变更即存（防抖）+ 退出兜底 | 同左（FR-10） |

状态文件示例（`window-state.json`）：

```json
{ "version": 1, "x": 312, "y": 184, "width": 1680, "height": 1004, "maximized": false }
```

UI 无新增可见控件；唯一用户可感知变化 = 窗口出现的位置/尺寸行为。日志决策原因码建议：`restored` / `restore-invalid` / `default-centered` / `suppressed(<flag>)` / `skip-reapply`。

## 7. Technical Considerations

### 7.1 更名方案：OUTPUT_NAME 而非改目标名
- 仅对 `f3d`/`f3d-console` 目标设 `OUTPUT_NAME`，`application/testing/` 的 `$<TARGET_FILE:f3d>`、SDK `install(EXPORT f3dTargets)`、`VS_STARTUP_PROJECT` 等全部零波及；与仓库「内部 F3D 标识稳定、对外身份 Glance3D」的既定策略一致。
- **风险——资源相对路径**：运行时资源按 `<exe>/../share/f3d/...` 解析（[application/CMakeLists.txt:233-243](application/CMakeLists.txt) 注释），该锚点是 exe **所在目录**而非文件名，更名理论无影响；US-001 以出图实测封口。同理排查 `F3DSystemTools` 中一切按 argv[0]/exe 名推导的逻辑。
- `winshellext` 按 DLL 同目录定位 exe（字面量 `L"f3d.exe"`），必须同步（FR-3）；本机不注册 shell 扩展，编译期验收。
- man 生成（help2man）描述串 "fast and minimalist 3D viewer" 顺带核对输出名。

### 7.2 窗口几何：分层与新代码命名
- **落点**：策略与持久化在 `application/`（新文件按品牌命名，如 `G3DWindowState.{h,cxx}`，遵循「新代码用 Glance3D/G3D 命名」约定）；显示器枚举/工作区查询 Windows 用 Win32（`MonitorFromPoint`/`GetMonitorInfo`），置于 app 层平台守卫内。
- **共享核心增补**（US-005）：`f3d::window::getPosition()`（及 Windows 最大化读取）走 `library/`，因 app 层无 VTK 直接访问、且平台差异应收敛在既有 `window_impl` 平台层；属 additive API，源内 fork 无 ABI 顾虑，但**每次公共 API 增补都会加大上游合并成本**——实现时在 tech-debt 记录。
- **保存触发**：优先挂 VTK 窗口事件（resize/configure/move）防抖；退出路径（正常 close、`Esc` 退出命令）兜底 flush。崩溃丢最后一次变更可接受（防抖已将窗口缩到 ≤1s）。
- **抑制判定**：需要区分「resolution 值来自内置默认」与「用户显式给出」——CLI/config 解析层已有 per-option 来源信息（`ParseOption` 合并处，F3DStarter.cxx:821-822），实现时以「用户是否显式提供」为准，而非值比较。

### 7.3 二次 ApplyPositionAndResolution 的考证义务
- 动 F3DStarter.cxx:1338 前，先 `git log -L` / 对照上游 f3d 考证其引入原因（合理假设：早期为保证首帧前窗口尺寸生效/某些平台窗口 realize 时序），确认 `--output` 无头路径不依赖该次调用后再删改；若确有时序依赖，改为「几何未成功建立时才生效」的守卫式实现。
- 「加载中拖动」的真实时序：加载阻塞主线程（见既有结论 load-blocking-mainthread），期间 OS 层移动/改尺寸由 DWM ghost window 代管，恢复响应后才回灌——自动化注入（`SetWindowPos`）与 :1338 的执行顺序存在竞态，故 US-008 的验收以「日志证据 + `load_next_file_group` 实测 + 全量回归」为主，报告写明边界。

### 7.4 测试与自动化兼容矩阵
- 全部既有 recordings 回放调用固定带 `--no-config --resolution=300,300` → 被 FR-14 双重抑制，逐条免疫；US-006/007/008 各抽查回放为证。
- 无头出图（`--output`）被 FR-9/FR-14 抑制 → CLAUDE.md 出图流程零变化（路径里的 exe 名除外）。
- 新增自动化：PowerShell P/Invoke 脚本（`GetWindowRect`/`MoveWindow`）+ `G3D_WINDOW_STATE=<临时路径>` 隔离态，端到端验证记忆链路；脚本入 `scripts/` 或测试目录以便复用。

## 8. Success Metrics

- **消歧**：`rg -i 'f3d\.exe'` 仓库命中 0（历史 changelog 除外）；本机按 `glance3d` 检索唯一命中本项目产物；任务管理器显示 ProductName=Glance3D。
- **启动体验**：首启窗口中心 = 目标显示器工作区中心（±2px，P/Invoke 实测）；二次启动几何恢复一致（状态文件 ↔ 日志 ↔ GetWindowRect 三方一致）；加载完成后窗口几何与用户最后一次调整一致（日志 `skip-reapply` + 实测）。
- **零回归**：默认与显式分辨率的无头出图尺寸/内容与改动前基线一致；抽查回放 `--reference` 误差达标；若开 `BUILD_TESTING`，相关 ctest 标签全绿；`npm run build`（wasm）通过。
- **可观测**：任一几何决策均可从 `g3d_*.log` 单文件还原完整因果链。

## 9. Open Questions

1. **产物名大小写**：推荐全小写 `glance3d`（跨平台 CLI 惯例一致，Linux 二进制小写为规范；Windows 展示层品牌感由 VERSIONINFO 承担）。若倾向资源管理器里显示 `Glance3D.exe`，仅改 OUTPUT_NAME 大小写即可，不影响其它验收。
2. **用户配置目录 `%APPDATA%\f3d` 是否更名迁移**：建议另立 PRD（需「新目录优先、旧目录 fallback 读 + 一次性迁移」策略），本轮不动，避免和窗口状态混在一个变更里。
3. **Wayland 位置限制**：Wayland 协议不允许客户端自定位窗口 → Linux/Wayland 下位置恢复天然降级（仅尺寸生效）。推荐接受并在日志/文档注明，不做专门 workaround。
4. **保存粒度**：推荐「防抖 ≤1s + 退出兜底」（崩溃丢失窗口小）；若嫌 IO 频繁可退化为仅退出保存，验收相应放宽——默认按前者执行。
5. **发行物层（安装器/注册表）**：仓库内未见 NSIS 等安装脚本；本轮范围 = 仓库内全部可见引用。若后续引入安装器，需把 `glance3d.exe`、文件关联、winshellext 注册一并纳入其 PRD。
