# kynovo 工程全面审视（2026-09）

**范围**：`code/`、`tests/`、`build/*.sh`、`tools/`、`doc/`、`build.sh` 与工作区卫生。
**方法**：清点（规模/引用关系）+ 逐项阅读关键路径 + 在本轮会话中以**运行证据**验证（L0–L4 阶梯全绿，见 `doc/testing.md`）。
**结论摘要**：工程质量高（分层干净、约束明确、测试分级完整），主要风险集中在**体量**、**脚本/文档的分类与可发现性**、
以及**工作区卫生**；没有发现新的功能性缺陷。

---

## 1. 清点

| 区域 | 文件数 | 行数 | 说明 |
|---|---|---|---|
| `code/` | 12 | 18,666 | 10 个头文件 + 2 个应用（`kdbsvr.c`、`kdbctl.c`） |
| `tests/` | 15 | 19,287 | 4 个单元驱动 + 3 个 fuzz + selftest + 2 个基准 + 框架/线性一致性检查 |
| `tools/harness/` | 12 | ~700 | **可复用 harness**（soak/hunt/perf/正确性；索引见 `doc/testing.md`） |
| `tools/archive/` | 16 | ~800 | 早期一次性调查脚本（结论汇总：`doc/investigations.md`） |
| `tools/` | 11 | 1,834 | 5 个基准 + 2 个探针 + 覆盖率聚合 + treap 压力 |
| `doc/` | 2 → 6 | 11,500+ | `dissertation.md`（论文，语义基准）、`gaps-audit.md`（缺口清单），本轮新增 `testing.md`、本文件 |
| 构建 | 1 | ~370 | `build.sh`：10 驱动 + 8 基准 + `coverage` / `sanitize` / `clean` |

分层（依赖方向自上而下，无环）：

```
kdbsvr.c / kdbctl.c（应用）
  └ kserver.h（服务端核心 + 对端协议，4850 行）── kproto.h ── kbase.h
  └ kclient.h（客户端状态机，514 行）
  └ raft.h（共识 + 快照，4666 行）── kproto.h
  └ treap.h（有序索引 + COW，1070 行）—— 机制层，**不依赖 kbase**
  └ runtime.h（线程/事件循环，807 行）／ vfs.h（IO 抽象，460 行）／ cemon.h（Win32 事件循环，3626 行）
  └ cli.h（CLI 消息层，782 行，仅 kdbctl 使用）
```

四缝（可注入面）：`transport` vtable、`elapsed_ms`、`runtime` backend、`vfs` backend —— 见 `doc/testing.md` §2。

---

## 2. 代码（`code/`）

**做得好的**
- **分层与依赖方向清晰**：`treap.h` 为纯机制层（不依赖 `kbase.h`，自带 `TREAP_MALLOC` 宏），
  策略（seed、边界校验、并发门控）留在上层；`raft.h` 只依赖 `kproto.h`。
- **约束显式且可自动检查**：C89/MSVC 6.0/XP 三条约束都落在构建门里（`-Wdeclaration-after-statement`、
  无 C99 设施、`kbase.h` 封装 `__int64`），本轮新增代码全部通过。
- **错误可见性**：不变量告警、fail-stop 分支带原因打印；`Fail-stop` 有 `server_is_stopping` 之类前提，避免把正常停机读成崩溃。
- **持久化契约**：`raft_persist` 视图 + `raft_persist_complete(durable_index)` 的"只上报确实持久化的前缀"在库侧实现完备
  （follower 的 AE 响应会 `deferred_append_response` 到自身落盘确认之后，且只上报 `durable_confirm`）。
- **快照所有权**：`treap_save`（快照线程**纯读**）/ `treap_save_finish`（**属主线程**回收+复位）拆分后，
  非属主线程不再 free/写共享状态；头注释写明了不变量。

**风险与建议**

| 编号 | severity | 观察 | 证据 | 建议 |
|---|---|---|---|---|
| C-1 | 中 | `kserver.h` 4,850 行、`raft.h` 4,666 行，单文件定位成本高 | `wc -l` | 不急于拆分（分层已清晰），但建议在文件头维护"章节索引"，并把新功能优先落在已有小节内 |
| C-2 | 中 | 非 Windows（epoll/kqueue）分支**从未被编译**（18 处） | `grep -rn epoll\|kqueue code/*.h` | 保留为编译期分支即可，但应在文档中明确"未验证"，不要对外宣称跨平台 |
| C-3 | 低 | `cli.h` 仅 `kdbctl.c` 使用，却与库头并列 | `grep -rl cli.h` | 语义上属应用层；可考虑移到 `code/app/` 或注释标明层次 |
| C-4 | 低 | `R8`：被拒 AE 可携带未落盘的新任期 | `doc/gaps-audit.md` §R8 | 低危（同任期双主仍需多数票交集）；若要严格 §3.8，为拒绝响应加落盘闸门（可参照 `deferred_vote_response`） |
| C-5 | 低 | 磁盘满走 fail-stop；`README`/metrics 延后 | 既有决策 | 保持；但建议在 `gaps-audit.md` 明确"已知取舍"清单，避免后续被当作缺口重提 |

---

## 3. 测试与构建（`tests/`、`build.sh`）

**做得好的**
- **测试比生产更严**：驱动与发布二进制**同 `-O2`、同告警门**（含 MSVC 6.0 专项告警）；这条原则挡住了"测试掩盖生产告警"。
- **分级完整**：单元 → CLI 冒烟 → 单节点/集群模糊（含线性一致性检查）→ 网络 soak → 性能 → 覆盖率/UBSan。
- **基准也被纳入告警门**：`build_all` 会编译 8 个基准，避免它们"静默腐烂"。
- **`build.sh coverage` 的失败不会被掩盖**：每个驱动独立日志 + 独立退出码（历史上曾因一个子 shell 而把红构建报成绿）。
- **UBSan trap 模式**：MinGW 无 libubsan 运行时，用 `-fsanitize-trap=undefined` 让 UB → SIGILL，最后打印的 seed 即复现点。
- **故障注入只走四缝**，不侵入业务路径（原则明确）。

**风险与建议**

| 编号 | severity | 观察 | 证据 | 建议 |
|---|---|---|---|---|
| T-1 | 中 | 缺少"**默认门**"的单一入口：一次完整回归要手工敲 8 条命令 | 本会话的调用序列 | 在 `build.sh` 增加 `regress`（L0–L4 一键、逐门判决行、失败即停）与 `regress-fuzz N`；与 `doc/testing.md` §6 对齐 |
| T-2 | 中 | fuzz 的"失败种子—事件窗口"耦合：环缓冲 1,024 条不足以容纳一个 seed 的历史 | 本会话 F5 | 环缓冲放大或失败时整体落盘（测试驱动侧，非产品） |
| T-3 | 低 | `raft_cluster_fuzz` 的确定性依赖"跨 seed 全局重置完整" | 本会话（已修 `fuzz_alloc_calls`） | 新增全局必须加入每-seed 重置清单，并在该函数处注释此约束 |
| T-4 | 低 | `tests/lincheck.*`（Python 辅助）未纳入 `build.sh` 任何目标 | `grep -rl lincheck` | 在 `doc/testing.md` 说明其调用方式（当前经 `kserver_cluster_fuzz` 间接使用） |

---

## 4. 脚本（已归位）

**现状（本轮整理后）**：`build/` 是**纯产物目录**（`./build.sh clean` 整目录删除，等同全新 checkout），
手写内容全部迁出：
- `tools/harness/`（12）：`soak_release`、`pipe_verify`、`pipe_frontier`、`burst`、`perf_matrix`、`watch_counters`、
  `cpu_probe`、`conn_leak`、`stall_hunt`、`crash_hunt`、`pageheap_hunt`、`diag_deep` —— 全部改为**按自身位置推导仓库根**、
  自带工具链环境（`PATH`/`MSYS2_ARG_CONV_EXCL`）、带合理默认值（可裸跑），索引见 `doc/testing.md`。
- `tools/archive/`（16）：`cl3`–`cl29`（15 个）、`diag3`、`repro_deep` —— 结论汇总在 `doc/investigations.md`；
  其中 `cl11`/`cl13`/`cl14`/`cl15`/`cl29` 的记录是"问题尚未修复时"的快照（已如实标注）。
- `doc/measurements/`：177 份原始记录（`*.txt` 103 份含 `PHASE` 数据、`*.out/.err` 为调查结论）。

**遗留**：脚本仍靠 `taskkill /F /IM kdbsvr.exe` 收尾（粗暴但幂等）；`cl7` 等历史脚本的端口固定，
并行运行会冲突——已在索引里标注前提。

## 5. 工具（`tools/`，11 个）

`bench_fsync`（fsync 微基准）、`bench_e2e`、`bench_mt`、`bench_pipe`、`bench_rate`（端到端/并发/流水线/吞吐）、
`bench_client.h`/`bench_env.h`（基准公共头）、`cemon_tcp_probe.c`（连接身份探针）、`proc_time.c`（只读 CPU 时间）、
`treap_stress.c`（treap 层压力，200 万次零告警）、`cov_report.py`（gcov 聚合）。

**观察**：均与 `build.sh` 对齐（源文件引用无缺失），除 `cemon_tcp_probe`/`proc_time`/`treap_stress` 外都进告警门。
**建议**：把这三个也纳入 `build_all`（进入 `-Wall -Wextra -Wdeclaration-after-statement` 门），成本极低。

---

## 6. 文档（`doc/`）

| 文件 | 状态 |
|---|---|
| `dissertation.md`（7,988 行） | 论文原文，**语义基准**（有冲突时以论文语义 + 设计一致性为准） |
| `gaps-audit.md`（406 行） | 缺口清单 + 本轮会话的 E/F 节（含 8 条被否证假设、4 次无效修补、三处根因与修法） |
| `testing.md`（本轮新增） | 测试与回归方法（唯一权威说明） |
| `code-review-2026-09.md`（本文件） | 全面审视 |

**缺口**：没有 `README`（构建/运行/CLI 用法）与架构概览；`WSM.md` 已被判定不需要（事务 = FCALL + treap COW）。
**建议**：下一轮补一份 `README.md`（20–40 行：依赖、`build.sh` 目标、最小运行示例、CLI 命令表），
把"为什么产物落 `build/`"（杀软误报）写进去，其余细节引用 `doc/testing.md`。

---

## 7. 工作区卫生

| 编号 | severity | 观察 | 证据 | 建议 |
|---|---|---|---|---|
| H-1 | ~~中~~ **已修** | `build/` 曾达 **349 MB**（`build/perf/` 有 437 个调查日志）；且 `./build.sh clean` 原为 `rm -rf build/`，**会连同 28 个调查脚本一起删除**（隐患） | `du -sh build`；`do_clean()` 原文 | 已按「保留有价值过程产物」清理：**保留** 28 个 `*.sh` 与归档记录 `build/records/`（177 个文件 / 429 KB：每次基准的 `STATS_BEFORE/PHASE/LAT/STATS_AFTER` 与调查脚本 `.out/.err`），其余（33 个 `.exe`、`perf/`、`bench-*`/`cl*`/`stall`/`soak`/`cov` 数据目录、`chk.o`/`rt_dbg3.exe`）全部删除；`do_clean()` 改为**只删产物、保留 `*.sh` 与 `records/`**。结果：**349 MB → 3.9 MB**，重建后 L0 rc=0/0 告警、L1 全绿 |
| H-2 | 中 | **无版本控制**（无 `.git`，无 `.gitignore`） | `ls -a \| grep '^\.git'` 为空 | 最高性价比的流程改进：`git init` + `.gitignore`（`build/`），至少对 `code/`、`tests/`、`doc/`、`build/*.sh`、`tools/` 做提交基线 |
| H-3 | 低 | 根目录曾有 `kdb-selftest-*` 残留（30 个，已修） | 本轮 F4 | 已闭环；建议在 `doc/testing.md` §6 的交付判据里持续检查"工作目录无残留" |
| H-4 | 低 | `build/kserver_test_dbg.exe` 等调试产物与发布产物混放 | `ls build` | 已由 §testing 的"生产对调试设施 0 痕迹"保证；可加 `build/debug/` 子目录约定 |

---

## 8. 优先级建议（按性价比排序）

1. **H-2 版本控制基线**（一次性，收益最大）。
2. **T-1 `build.sh regress` 一键门**（把 `doc/testing.md` §6 变成可执行入口）。
3. **§4 脚本分类 + `build/README.md`**（把 28 个脚本从"只有作者知道"变成"可发现"）。
4. **H-1 日志与调试产物清理目标**。
5. **C-1/C-2 体量与平台说明**（文档标注，不急于动代码）。
6. **`README.md`**（面向新使用者/新会话）。

> 未列入建议的既有取舍（按既定判断保持）：磁盘满 fail-stop、无目录同步原语、`vfs` 不暴露错误码、
> `treap_save` 保留引用计数、exactly-once 靠指令幂等根治、多节点真机测试暂不具备条件。
