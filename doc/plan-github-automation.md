# 规划：接入 GitHub 与 Hermes Agent 自动化开发维护

**状态**：已拍板并执行中 —— 决策见 §6（已定），P0 完成，P1 进行中。**依据**：本文件 §1 的实测核查 + Hermes 官方技能（`hermes-agent`）关于
项目上下文文件与后台系统的权威说明。**执行前提**：§6 的五个决策点需先拍板。

---

## 1. 现状分析（实测，非印象）

### 1.1 有利条件
| 项 | 事实 |
|---|---|
| 依赖 | **零外部依赖**：单头文件库 + MinGW/ws2_32/winmm。仅 `tools/cov_report.py`、`tests/lincheck.py` 需要 Python |
| 换行符 | 抽查 `build.sh`/`code/kbase.h`/`code/raft.h`/`tests/selftest.c`/`doc/testing.md` 全为 **LF**（符合既定要求） |
| 凭据 | 全树未见口令/密钥/令牌（`cli.h`、`dissertation.md` 的命中是普通词） |
| 产物 | 二进制与命令产物全在 `build/`（现已为纯产物目录，`clean` 后消失） |
| 文档 | `doc/` 六项：`dissertation`（语义基准）、`gaps-audit`（缺口/backlog）、`testing`（门禁与方法）、`investigations`（历史调查）、`code-review-2026-09`（审查）、`measurements`（178 份原始记录） |
| 工具 | `git 2.28` 与 `python 3.11.16` 可用 |
| 门禁 | 已分层且有判定行约定（`doc/testing.md`）；关键门当前全绿 |

### 1.2 阻碍项（按对 GitHub/自动化的影响排序）
| # | 阻碍 | 证据 | 影响 |
|---|---|---|---|
| B1 | **`doc/dissertation.md` 是第三方论文**（Ongaro 博士论文全文） | 8k 行、非本项目版权 | **不得进入公开仓库**；私有仓库也建议移出 |
| B2 | **无 LICENSE / 无 README** | 根目录仅 `build/ build.sh code/ doc/ tests/ tools/` | GitHub 首屏与合规缺失 |
| B3 | **28 个文件硬编码 `D:/kynovo`**（数据路径，如 `disk://D:/kynovo/build/...`） | `grep -rl` 计数 28 | 任何非本机/CI 环境跑不了带数据的脚本 |
| B4 | **`python3` 在本机是 WindowsApps 存根**（Permission denied） | 实测 | 脚本/CI 必须用 `python` 或做探测 |
| B5 | **仅 Windows 可运行**（5 个头文件含 `windows.h`/`winsock2.h`；非 Windows 分支从未编译） | `grep -lc` | CI 必须用 `windows-latest`；不可宣称跨平台 |
| B6 | **无版本控制**（无 `.git`） | `ls -a` | 无回滚、无 diff、无法协作——**最高性价比的修复项** |
| B7 | 无"一键门"入口：完整回归要手工敲 8 条命令 | `doc/code-review-2026-09.md` T-1 | 自动化与 cron 缺少稳定入口 |
| B8 | 无机器可读结果：判定靠人读输出 | 同上 | agent 无法可靠断言"通过/失败" |

---

## 2. GitHub 接入准备

### 2.1 仓库形态
- 私有仓库优先（B1 的存在使公开仓需先做版权处理）；`main` 为唯一长期分支，改动一律走短命 feature 分支 + PR。
- 提交信息：首行 ≤72 字符、祈使句；正文写**为什么**与**验证方式**（哪几个门、判定行结果）。避免把调试过程写进历史。
- 标签策略：`v0.x.y`（当前无生产使用，先 0.x）。

### 2.2 必备文件（本规划交付物，执行阶段生成）
| 文件 | 内容要点 |
|---|---|
| `.gitattributes` | `* text=auto eol=lf`、`*.md text eol=lf`、`*.c/*.h text eol=lf`、`*.exe binary` —— 强制 LF（既定要求） |
| `.gitignore` | `build/`、`*.exe`、`*.log`、`*.o`、`*.gcov`、临时数据盘目录；**不忽略** `doc/measurements/`、`tools/` |
| `README.md` | 20–40 行：单头文件 Raft KV 是什么、Windows XP+ 与 MSVC6/C89 约束、依赖、`./build.sh` 目标表、最小运行示例（`init` → `server` → `kdbctl SET/GET`）、指向 `doc/testing.md` |
| `LICENSE` | **Apache-2.0**（官方全文；与 README 的版权行一致） |
| `AGENTS.md` | 见 §3.1（同时是 Hermes 的项目上下文入口） |
| `.github/workflows/ci.yml` | 见 §2.3 |
| `.github/pull_request_template.md` | 要求填写：改了什么、**跑了哪些门**、判定行结果、风险与回滚方式 |

### 2.3 CI 设计（Windows runner，按"分钟预算"分层）
| 阶段 | 内容 | 预算 | 触发 |
|---|---|---|---|
| build | `./build.sh` + **0 error/0 warning 断言** | ~2 min | 每次 push/PR |
| unit | `raft_test` / `kserver_test` / `kclient_test` / `cemon_test` / `selftest` | ~1 min | 同上 |
| smoke | `tests/cli_smoke.sh`（真进程 + 真 socket） | ~10 s | 同上 |
| fuzz-fast | `raft_fuzz 1 2000`、`raft_cluster_fuzz 1 200 0`、`kserver_cluster_fuzz 1 1` | ~1 min | 同上 |
| nightly | `raft_cluster_fuzz 1 20000 0`（可切分并行）、`soak_release.sh RUNS=24`、`./build.sh coverage`、`./build.sh run-sanitize` | ~20 min | 每日 cron |
- 工具链：MSYS2 + `mingw-w64-x86_64-gcc`（**不钉 15.2.0**，在 CI 里记录实际版本；本地仍钉 15.2.0）。若某版本缺 `LPFN_ACCEPTEX`（9.2.0 的已知问题）CI 会立刻暴露。
- 产物：nightly 上传 `build/*.exe` 与 `build/perf/` 日志为 artifact；`doc/measurements/` 由本地人工归档，CI 不写仓库。
- 安全：`GITHUB_TOKEN` 只读；Actions 建议 pin 到 SHA；无任何 secret 需求（零依赖、零外部服务）。

---

## 3. Hermes Agent 自动化准备

### 3.1 项目上下文文件：`AGENTS.md`（唯一，cwd-only）
权威约定：`.hermes.md`（可向 git 根以上继承）与 `AGENTS.md`（**仅 cwd**、可移植到 Codex/Claude Code/OpenCode）
**先匹配者生效**。本项目扁平、且希望其他 agent 也能用 ⇒ 选 **`AGENTS.md`**，内容控制在 20,000 字符内，细节引用 `doc/testing.md`。

建议骨架（执行阶段落文件）：
```
# kynovo — agent 工作须知
## 硬约束（违反即拒绝）
- C89 / MSVC 6.0 兼容 / Windows XP+；禁止 C99 设施（inline、VLA、stdint.h、long long 字面量后缀 ULL）
- 换行一律 LF；不得手写任何文件到 build/（纯产物目录）
- 不得改动 raft.h/treap.h 的契约而不先对齐 doc/dissertation.md 的论文语义并说明
## 工作流
1) 改前先跑基线门；2) 改后跑 `./build.sh regress`（全门 + 判定行 + 机器可读摘要）；
3) 报告必须带证据（判定行 / 日志路径 / 退出码），禁止"应该没问题"式结论
## 纪律（本项目用代价换来的）
- 先确认构建成功，再解读运行结果（曾三次跑旧二进制得出错误结论）
- 同构建 A/B；低概率缺陷用数十次采样或长程门；单次干净运行不算证据
- 多不变量 harness 失败：先找"第一处分歧"，不要从失败点往回推
- 仪器（临时打印/埋点）必须回退；保留的只能是修复本身与长期诊断
- 不得逐行批量改控制流；释放点必须在契约终点；关闭一律"先存后清再关"
## 关键入口
- 门禁与方法：doc/testing.md   backlog：doc/gaps-audit.md   harness：tools/harness/
- 历史调查与结论：doc/investigations.md   原始测量：doc/measurements/
```

### 3.2 一键门：`./build.sh regress`（自动化与 cron 的稳定入口）
- `./build.sh regress [--quick|--full|--fuzz N]`：按 `doc/testing.md` §6 的顺序执行；
- **末行输出机器可读摘要**：`REGRESS|quick|pass=7 fail=0 duration=142s`（agent 只需读这一行）；
- 失败即停并打印失败门的**判定行**与日志路径；`--full` 追加 L4/L6；
- 与 `doc/testing.md` 的表格一一对应，避免两处漂移。

### 3.3 技能（skills）固化
现状已有：`kynovo-storage`、`performance-benchmarking`、`debug-instrumentation`、`consensus-fuzz-testing`、`code-audit-remediation`。
建议新增/修订（执行阶段）：
| 技能 | 内容 | 与现有分工 |
|---|---|---|
| `kynovo-build-verify` | 一键门用法、判定行、常见假失败（端口占用、appverif 未 disable、旧二进制） | 新 |
| `kynovo-release` | 打 tag、构建产物、更新 `doc/measurements/` 基线 | 新 |
| `kynovo-raft-contract` | 论文语义 ↔ 代码落点映射（`raft.h` 契约条目与行号） | 与 `raft-consensus-review` 互补 |
> 原则：技能写**过程与判定**，`AGENTS.md` 写**约束与纪律**，`doc/` 写**事实与证据**，三者不重复。

### 3.4 后台与自动化（按权威能力设计）
| 手段 | 用法 | 注意 |
|---|---|---|
| `cron` | 夜间：`workdir: D:\kynovo`（自动加载 `AGENTS.md`）+ `script` 预跑 `./build.sh regress --quick` 收集数据 + agent 任务解读并只在**失败**时报告 | **每次运行 3 分钟硬中断** ⇒ 长任务（20k 轮 fuzz、24 轮 soak）必须由脚本以**后台终端进程**启动并落日志，agent 只读日志 |
| `cron` 链 | `context_from` 把"夜间门禁摘要"喂给"次日评审/修复任务" | 保持主会话角色交替，不镜像 |
| `kanban` | 把 `doc/gaps-audit.md` 的条目转成任务卡（ID/验收标准/证据要求）；orchestrator 分派，worker 只做一卡 | 持久、可重试；与 GitHub Issues 二选一或双写（决策点 D3） |
| `delegate_task` | 读多写少的分析（审计、交叉验证、撰写报告）并行 | **不持久**：进程退出即丢 ⇒ 只用于分钟级子任务 |
| 工作树 | 并行写代码的 agent 用 `hermes -w`（worktree） | 避免同分支冲突；合并交给 merge-reconciler |

### 3.5 "ai 可信"的验收约定（写进 `AGENTS.md`）
1. 任何"完成/修复"声明必须附**可复核证据**：判定行 + 日志路径 + 退出码；
2. 高风险改动（raft 语义、持久化、并发）要求**两处独立证据**（例如单测 + 集群 fuzz 同绿）；
3. 修复必须**重跑曾经复现的那一道门**，并把结果写进 `doc/gaps-audit.md`；
4. 被否证的假设要显式撤回（本仓库已有此惯例，见 `doc/gaps-audit.md` E/F 节）。

---

## 4. 分阶段执行计划（每阶段可独立交付/回滚）

| 阶段 | 产出 | 验收 | 预估 |
|---|---|---|---|
| **P0 版本控制基线** ✅ 已完成 | `git init`；`.gitattributes`、`.gitignore`；移出 `doc/dissertation.md`（改由文档说明获取方式）；首次提交 | `git status` 干净；`grep -c dissertation .gitignore` 命中；`./build.sh clean && ./build.sh` 后 `git status` 仍干净（证明 `build/` 被忽略） | 0.5 天 |
| **P1 可自动化**（`AGENTS.md` ✅、`regress` ✅；余：数据路径参数化、`python` 探测） | `AGENTS.md`；`./build.sh regress`（含 `REGRESS|` 摘要）；统一 `python` 探测；数据路径参数化（`KDB_DATA`，默认 `build/data`） | `./build.sh regress --quick` 绿且末行可解析；在非 `D:\kynovo` 目录复制一份也能跑（验证无绝对路径依赖） | 1 天 |
| **P2 GitHub + CI**（`ci.yml`/`nightly.yml`/PR 模板 ✅ 已就绪；待远端与认证） | 私有仓库；`ci.yml`（build/unit/smoke/fuzz-fast）；PR 模板；nightly 工作流 | PR 触发 CI 全绿；故意引入一个告警 ⇒ CI 红（证明断言有效） | 1 天 |
| **P3 自动化闭环** | 技能三件（§3.3）；cron 夜间门禁 + 失败才报告；kanban/Issues 与 backlog 对齐 | 连续 3 个夜间任务按预期只在失败时报告；一条 backlog 卡片走完"分派 → 改 → regress → PR" | 1 天 |
| **P4 常态维护** | 按 backlog 驱动：每任务 = 分支 → 改 → `regress` → PR → 报告（附证据） | 每周回顾：门的绿/红趋势、`doc/measurements/` 是否更新、技能是否需修订 | 持续 |

**风险与缓解**
- 引入 CI 后本地与 CI 的 gcc 版本差异 ⇒ CI 记录实际版本；本地保持钉版，差异导致的失败先按"工具链差异"排查。
- 自动 agent 改动持久化/并发代码有回归风险 ⇒ P4 起强制"分支 + PR + 两处证据 + 夜间门"。
- 数据路径参数化会触及 28 个脚本 ⇒ 一次性完成并逐个 `bash -n` + 至少各跑一次（scripts 已能裸跑）。

---

## 5. 与现有文档的关系（避免重复）
- `doc/testing.md`：门禁与方法（**唯一权威**）——`regress` 的实现必须与它一致；
- `doc/gaps-audit.md`：backlog 与证据（每项修复在此更新状态与撤回记录）；
- `doc/investigations.md` + `doc/measurements/`：历史与原始数据（不重测就有据可查）；
- `doc/code-review-2026-09.md`：结构与卫生审查（本规划的上游）；
- `AGENTS.md`：**给 agent 的约束与纪律**（最短、最硬、可移植）。

## 6. 决策点（需拍板后执行）
| # | 决策 | 选项与影响 |
|---|---|---|
| D1 | 仓库可见性 | **公开**（已定）⇒ `doc/dissertation.md` 保持原地并 `.gitignore`（引用不失效、永不提交） |
| D2 | CI 范围 | **完整 + nightly**（已定） |
| D3 | backlog 载体 | **双写**（已定）：GitHub Issues 面向人 + Hermes kanban 面向 agent |
| D4 | agent 权限 | **允许直推 `main`**（已定）⇒ 补偿措施：每次推送前本地跑 `./build.sh regress quick`；nightly 是安全网（已写入 `AGENTS.md`） |
| D5 | 许可证 | **Apache-2.0**（已落 `LICENSE`，官方全文逐字取自 apache.org）：与 MIT 同为宽松许可，额外**显式授予专利许可**并含专利报复条款；要求保留版权/声明并标注修改 |

---

## 7. 环境约束：开发机不常开机（本方案据此调整）

**约束（维护者说明）**：这台 Windows 机器不是长期在线——上班期间不在此处，通常不开机。
这否定了本方案早期的一个隐含前提：**凡是依赖本机常驻的东西都不能进关键路径**。

### 7.1 不能作为基础设施的东西（本机侧）
| 组件 | 为什么不成立 |
|---|---|
| Hermes **cron** 定时任务 | 调度器由 gateway 托管，gateway 只在本机登录后运行；机器关机时到点的任务**直接错过**（不会补跑） |
| Hermes **kanban 派发器** | 同样由 gateway 每分钟 tick；没有 gateway，卡片**永远停在 `ready`** |
| 本机 **`regress full`** 长门（20 分钟） | 需要人真的开着机器；适合"在场时跑"，不适合当每夜的门 |
| 本机 **钉版工具链的 0 告警断言** | 只有本机能跑（CI 是外来编译器，见 §G2）——这是**只能在本机做的**检查，因此保留为"在场时"的门 |

### 7.2 方案：**把周期性/验证性工作搬到云端，本机只做交互式开发**

| 工作 | 归属 | 触发方式 | 机器关机时 |
|---|---|---|---|
| 每夜全量门（20k fuzz / 2000 cluster / 24 轮 soak / coverage / UBSan） | **GitHub Actions** | cron `17 18 * * *`（02:17 CST） | **照常运行** ✓ |
| 每次 push/PR 的快门（build+unit+smoke+fast fuzz） | **GitHub Actions** | `push` / `pull_request` | 照常运行 ✓ |
| 需要时立刻在云端跑全量门 | **GitHub Actions** | **推一个 tag（`v*`）**——走 SSH，**不需要任何 token** | 照常运行 ✓ |
| 失败证据 | **GitHub 分支** | 失败时推 `ci-logs` / `ci-logs-nightly`（公开可 `git fetch`） | 照常可得 ✓ |
| 失败→**待办条目** | **GitHub Issues** | 失败的工作流用自带的 `GITHUB_TOKEN` 自动开/更 issue（无需个人 token） | 照常可得 ✓ |
| 交互式开发、定性的调查、需要钉版工具链的严格门 | **本机** | 人在场时由 Hermes 驱动 | 不运行（**不在关键路径**） |

这样"机器不常开"只影响**开发速度**，不影响**验证与记录**：云端每天照跑，红灯自动变成 issue，证据自动落在公开分支。

### 7.3 本机侧的降级设置
- 本机的每日门禁值守任务（cron）保留，但**降级为 best-effort**：改为 `monitor` 变更检测（只在云端运行状态**真的变化**时才唤起 agent，平时不消耗），并把频率提到每 2 小时——开机后不久就会自查一次，关机期间也只是错过，不积压。
- **不要**把 gateway 当作关键路径：装了它（登录自启）能让"在场时"的自动化更顺，但所有关键结论都已经有云端来源。

### 7.4 已知的云端限制（诚实记录）
- **公开仓库的定时工作流在 60 天无仓库活动后会被自动停用**；任何 push 会重置该计时，也可在 Actions 页面手动重新启用。
- 云端用的是外来工具链（当前 gcc 16.2.0）⇒ **0 告警契约只在钉版工具链上强制**（见 `doc/gaps-audit.md` G2）；云端用**外来工具链的新发现**（如 G1）作为信号，不作为判决。
- Actions 日志与制品**匿名不可读**（403）⇒ 失败证据一律走公开分支 + issue 正文，这也是上面两条设计的原因。
