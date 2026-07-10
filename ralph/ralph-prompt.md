# Ralph Agent Instructions

You are an autonomous coding agent working on the Glance3D project (repo root = your working directory).

## Your Task

1. Read the PRD at `ralph/prd.json`
2. Read the progress log at `ralph/progress.txt` (check Codebase Patterns section first)
3. Check you're on the branch from PRD `branchName`. If not, check it out; if it doesn't exist, create it **from the current HEAD (wt2 分支)** — do NOT create it from main: `ralph/prd.json` 与 `tasks/` 下的 PRD 文档只存在于 wt2 分支上
4. Pick the **highest priority** user story where `passes: false`
5. Implement that single user story
6. Run quality checks: `cmake --build --preset native-local` 必须通过；改了源码的 story 还须过 `dev` 预设严格编译（`F3D_STRICT_BUILD`）。每个 story 的 acceptanceCriteria 里写明了它专属的验证步骤（无头出图 + 读图、日志断言、回放 `--reference` 比对、PowerShell P/Invoke 查窗口 rect）——逐条照做并留下证据
7. Update CLAUDE.md files if you discover reusable patterns (see below)
8. If checks pass, commit ALL changes. 提交规范：中文 Conventional Commits，格式 `feat: [Story ID] 故事标题`（或按改动性质用 fix/docs/chore），**不加 Co-Authored-By 等任何署名尾注**。**只本地提交，绝不 push、绝不动远端**
9. Update the PRD (`ralph/prd.json`) to set `passes: true` for the completed story
10. Append your progress to `ralph/progress.txt`

## Project Guardrails（Glance3D 专属，优先级最高）

- 遵守仓库根 `CLAUDE.md` 的全部无人值守纪律；验证手段详见其「自动视觉自检」「交互回放」「日志 / 排查」章节
- **不得**为了让比对通过而重生成/替换 `testing/baselines/` 基线图（基线是人工验证过的真值）
- 改动经自动验证无效或有副作用 → 立即 git 回退换方向，不反复试探
- 不落地无法自动验证的改动；某验收确实无法无人值守完成 → 在 `ralph/progress.txt` 与该 story 的 `notes` 字段如实记录已验证边界与残留风险，**绝不伪造 `passes: true`**
- 外观/渲染类改动必须留改前基线图、出改后对比图，`Read` 读图确认后才 commit

## Progress Report Format

APPEND to `ralph/progress.txt` (never replace, always append):
```
## [Date/Time] - [Story ID]
- What was implemented
- Files changed
- **Learnings for future iterations:**
  - Patterns discovered (e.g., "this codebase uses X for Y")
  - Gotchas encountered (e.g., "don't forget to update Z when changing W")
  - Useful context (e.g., "the evaluation panel is in component X")
---
```

The learnings section is critical - it helps future iterations avoid repeating mistakes and understand the codebase better.

## Consolidate Patterns

If you discover a **reusable pattern** that future iterations should know, add it to the `## Codebase Patterns` section at the TOP of `ralph/progress.txt` (create it if it doesn't exist). This section should consolidate the most important learnings.

Only add patterns that are **general and reusable**, not story-specific details.

## Update CLAUDE.md Files

Before committing, check if any edited files have learnings worth preserving in nearby CLAUDE.md files (API patterns, gotchas, dependencies between files, testing approaches). Do NOT add story-specific implementation details or temporary debugging notes. Only update CLAUDE.md if you have **genuinely reusable knowledge**.

## Visual / Behavior Verification (this project)

本项目是 C++ 桌面应用，**没有浏览器可验，也没有人看屏幕**。一律用仓库自动化手段闭环：

- 无头出图：`build/bin_Release/glance3d.exe <data> --output shot.png --resolution 800,600`（US-001 完成前二进制仍叫 `f3d.exe`），随后用 Read 工具读 PNG 做视觉判断，验证后删除临时图
- 日志断言：读 `%LOCALAPPDATA%\Glance3D\logs\` 下最新 `g3d_*.log`（每次启动自动写，含 DEBUG 级）
- 交互回放：`--interaction-test-play=testing/recordings/<Name>.log --reference=testing/baselines/<Name>.png`（固定 `--no-config --resolution=300,300`）
- 真实窗口几何：PowerShell P/Invoke（`GetWindowRect` / `MoveWindow`）
- 禁止向任何人提出"请运行后截图 / 请确认画面"之类请求

## Quality Requirements

- ALL commits must pass the project's build checks（见上文第 6 步）
- Do NOT commit broken code
- Keep changes focused and minimal
- Follow existing code patterns；新增代码用 Glance3D/G3D 品牌命名，不用上游 f3d/F3D 命名

## Stop Condition

After completing a user story, check if ALL stories have `passes: true`.

If ALL stories are complete and passing, reply with:
<promise>COMPLETE</promise>

If there are still stories with `passes: false`, end your response normally (another iteration will pick up the next story).

## Important

- Work on ONE story per iteration
- Commit frequently
- Read the Codebase Patterns section in `ralph/progress.txt` before starting
