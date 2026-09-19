# kynovo 测试与回归方法（完整版）

本文是工程当前的**唯一权威测试说明**：分层、逐条命令、判定行、耗时、故障注入的架构缝、调试设施、
以及本项目在多轮排查中沉淀下来的**回归纪律**与已知陷阱。凡与本文不一致的旧脚本注释，以本文为准。

- 适用范围：`code/`（6 个头文件 + 2 个应用）、`tests/`（15 个文件）、`build/*.sh`（28 个脚本）、`tools/`（11 个工具）。
- 目标平台：**Windows XP 起**（`_WIN32_WINNT=0x0501`）、**MSVC 6.0 兼容的 C89**、单头文件库。
- 一切命令在仓库根目录、git-bash 下执行；构建产物只落 `build/`（不入 `%TEMP%`，理由见 `build.sh` 头注释：无签名 MinGW 二进制在临时目录最易触发杀软误报）。

---

## 0. 工具链与构建门（L0）

```bash
export PATH="/d/MinW64-15.2.0/bin:$PATH"      # 必须：PATH 上的 /d/MinGW 9.2.0 会因缺 LPFN_ACCEPTEX 失败
export MSYS2_ARG_CONV_EXCL='*'               # 必须：否则 MSYS 会改写传给原生程序的参数
./build.sh                                   # 构建全部目标（10 个驱动 + 8 个基准）
```

- `build.sh` 会把 `$MINGW_BIN` 前置进 PATH 并校验版本（期望 **15.2.0**）。
- **测试驱动与生产二进制同优化级别（`-O2`）且同告警门**：`-Wall -Wextra -Wdeclaration-after-statement`
  （外加 `-Wno-unused-function`，因为单头库会暴露驱动未调用的公共 API）。原则：**测试不得比生产宽松**。
- `-Wdeclaration-after-statement` 专门捕 MSVC 6.0 会拒绝的"语句后声明"。
- **L0 判定**：`./build.sh` 的 rc=0 且 `build/build.log` 中 `grep -cE ' error|warning'` 为 **0**。
  > 纪律：**先确认构建成功，再解读任何运行结果**。本项目出现过三次"以为修正无效、其实跑的是编译失败后的旧二进制"。

目标速查（`./build.sh <target>`）：`test fuzz cfuzz kclient kserver kclusterfuzz cemon-test`
与其 `run-*` 版本、`selftest`、`kdbsvr`、`kdbctl`、`bench*`、`cemon-bench`、`cemon-stress`、
**`coverage`**（gcov 行/分支聚合）、**`sanitize` / `run-sanitize`**（UBSan trap 模式，UB → SIGILL）、`clean`。

---

## 1. 测试分层

| 层 | 内容 | 命令 | 判定行 | 量级 |
|---|---|---|---|---|
| L0 | 编译门 | `./build.sh` | rc=0 且 `build.log` 0 告警 | ~90s |
| L1a | raft 单元 | `./build/raft_test.exe` | `SUMMARY: 221/221 passed` | ~1s |
| L1b | 服务端单元（含压力模式） | `./build/kserver_test.exe` | `SUMMARY: 33/33 passed` | ~30s |
| L1c | 客户端单元 | `./build/kclient_test.exe` | `SUMMARY: 16/16 passed` | ~0.1s |
| L1d | cemon 事件循环单元 | `./build/cemon_test.exe` | `SUMMARY: 5/5 passed` | ~2s（含 2s 长等待） |
| L1e | 端到端自检（单进程，含成员变更/引导/选主） | `./build/selftest.exe` | `selftest: PASS` | ~1–3s |
| L2 | CLI 语义冒烟（真进程 + 真 socket） | `bash tests/cli_smoke.sh` | `cli_smoke: PASS` | ~10s |
| L3a | 单节点随机模糊（API/OOM 回滚） | `./build/raft_fuzz.exe <seed> <n>` | `done: <n> iterations` / `FAIL: …` | n=2000 ~10s |
| L3b | **多节点集群模糊**（真实拉模式报文：丢包/重排/复制/分区/崩溃重启/成员变更/OOM 注入） | `./build/raft_cluster_fuzz.exe <seed> <n> [persist_delay]` | `done: <n> iterations` / `FAIL: …` | n=2000 ~30s |
| L3c | 服务端集群模糊（含线性一致性检查 `tests/lincheck.*`） | `./build/kserver_cluster_fuzz.exe <seed> <n>` | `done: 1/1 clusters consistent` | ~10s |
| L4 | 网络 soak（发布版二进制、24 轮、轮内 PIPE 压测 + 存活检查） | `RUNS=24 bash tools/harness/soak_release.sh` | `rounds_without_full_success=0`、`final liveness: 1` | ~3min |
| L5 | 性能/延时 | `tools/harness/perf_matrix.sh`、`burst.sh`、`pipe_frontier.sh`、`pipe_verify.sh`、`watch_counters.sh` | 各自输出 ops/s、p50/p99、计数器 | 分钟级 |
| L6 | 仪器化 | `./build.sh coverage`、`./build.sh run-sanitize`、`K_ALLOC_DEBUG` 构建 + appverif/gdb（见 §3） | 覆盖率表 / SIGILL 无发生 / 零报告 | 分钟级 |

**判定行的读法（强制）**：一律用 verdict grep，**禁止 `tail -1`**。带进度输出的套件里 `tail -1`
会显示无关行，使失败读成通过；缺失判决行同样按**失败**处理。

```bash
grep -E "SUMMARY|FAIL|done:|consistent|PASS" <log> | tail -3
```

---

## 2. 故障注入：只走架构已有的"缝"

测试基础设施**不得侵入业务代码**。若某个 failpoint 需要在业务路径埋点，则**宁可不要**。允许的注入面只有四缝：

| 缝 | 位置 | 用途 |
|---|---|---|
| transport vtable | `k_server_transport` | 报文丢弃/重排/复制、分区、崩溃（`raft_destroy`） |
| `elapsed_ms` 时钟注入 | `raft_advance(r, elapsed_ms, …)` | 选举超时/心跳节律、确定性"小步"尾段 |
| runtime backend | `runtime_create("thread"\|"inline", …)` | 同负载下"同步后端 vs 真线程后端"对照（最快判别跨线程缺陷） |
| vfs backend | `vfs_backend` | 磁盘错误、CRUD 钩子 |

OOM 注入走库自身的分配器钩子（`RAFT_MALLOC` 等），并按"第 N 次分配失败一次"语义武装；
**必须按 seed 重置触发基**（`fuzz_alloc_calls`），否则同一 seed 会随**调用规模**改变行为（已修）。

---

## 3. 调试设施（生产零影响）

1. **自描述分配器**（`code/kbase.h`，`#ifdef K_ALLOC_DEBUG`）
   - 隔离区 + 头/尾金丝雀 + 活块登记表 + 每操作 `k_dbg_verify()`；报告"分配点 / **释放点** / 首个坏字节偏移"。
   - **必须加锁**：全局登记表在多线程下会被并发 free 搞乱，产出与真缺陷无法区分的假报告（曾自造 9 次
     `Invalid address specified to RtlFreeHeap`）；入口（alloc/free/verify）一律串行化。
   - 覆盖范围：`K_MALLOC` **以及**各层自带宏（`TREAP_*`/`CEMON_*`/`RUNTIME_*`）——否则仪器对"最可能被破坏的对象"（treap 节点/blob、socket、队列节点）**完全失明**。
   - 调试构建：`gcc -std=c89 -O0 -g -Wall -Wextra -Wno-unused-function -pthread -DK_ALLOC_DEBUG -o build/kserver_test_dbg.exe tests/kserver_test.c`
2. **`--apply-stress <ops> [keyspace] [thread]`**（`kserver_test` 的内置模式）：把原本要数十轮网络 soak 才复现的
   跨线程缺陷压到**秒级**，并可与同步后端对照（`… thread` vs 默认）。
3. **Windows 页堆**：`appverif.exe -enable Heaps -for build/<exe>` → `gdb --batch -ex run -ex quit --args …`
   → **务必** `appverif.exe -disable Heaps -for build/<exe>`（机器级开关）。注意：应用验证器的 Heaps 只给
   "检测"，不提供写入时故障；无 paged-heap 工具时不要停在原地，改用**定向 A/B**（编译期开关关掉可疑机制再跑同一门）。
4. **gdb 配方**：`-O0 -g` **同一份源码**构建；grep 证据行（`^#[0-9]+ `、`received signal`、`exited`）而非 `tail`；
   崩溃的可疑帧常在真凶**下层**（框架的有效性检查先解引用悬垂指针）。

---

## 4. 回归纪律（本项目用代价换来的条目）

1. **改前先跑 L0–L1e** 建立基线；改后跑**完整阶梯** L0→L4（重大改动再加 L5/L6）。
2. **同构建 A/B**：任何对照实验必须在**同一个二进制**里用开关切换；跨构建比较会把"打印位置变化"读成路径差异。
3. **低概率缺陷的证据强度**：单次干净运行**不构成证据**（同一二进制几分钟内可从 2/3 失败变 0/8 通过，取决于机器负载）。
   用**数十次采样**或**长程门**（例如 60 轮页堆网络 soak），并在修复后**重跑那个曾经复现的门**。
4. **先找"第一处分歧"**：多不变量 harness 报出的失败是链条**末端**。先在事件流里定位**最早一处"两个独立记录对同一状态不一致"**
   （模型 vs 组件上报、镜像 vs 上报 durable、applied vs durable），再针对**那个事件**二分。
   判据：**两个以上各自论证无误的修复却让 pass/fail 纹丝不动 ⇒ 停止修补、去找第一处分歧**。
5. **测量装置必须先自证**：跑之前建立"正信号"断言（服务端探活、每轮 `ok=<N>`），并把既非成功也非失败的轮次计为
   `unexpected` 并中止——否则"0 次失败"可能只是"0 条数据"（本项目犯过一次：argv 缺参导致 20 轮空跑被报成 clean）。
6. **仪器必须回退**：探针（临时打印/埋点）在定案后全部移除，用**精确标签 grep** 核对残留；
   保留的只应是**修复本身**与**有长期价值的诊断**（如 `--apply-stress`、`SEED` 标记）。
7. **不要把假设写成结论、不要截断证据**：`grep … | head -N` 会藏掉致命行（本项目因此误判过一次"tree 只被主循环访问"）；
   被否证的假设要在文档里**显式撤回**。
8. **不逐行批量改控制流**：本项目一次批量行编辑把"有保护的关闭"改成无条件关闭，制造了服务端主动踢客户端的回归。
9. **句柄/所有权**：关闭点一律"**先存后清再关**"；跨层传递指针时，释放点必须在**契约终点**（不是"数据已复制"处）。
10. **平台约束是回归的一部分**：新增代码必须过 `-Wdeclaration-after-statement`；不得引入 C99（`inline`/VLA/`stdint.h`）、
    不得用 `long long` 字面量后缀 `ULL`（MSVC 6.0 不接受），`__int64` 由 `kbase.h` 封装。

---

## 5. 已知陷阱（harness 侧，本项目实测）

1. **驱动模型必须与"库自报的持久状态"一致**：`raft_cluster_fuzz` 曾因三处模型缺陷连累库被误判为有 bug：
   - 镜像按**增量/心跳视图**整体重写 ⇒ 把已持久化的配置条目清零 ⇒ 崩溃后恢复**旧成员** ⇒ 覆盖已提交条目（LEADER COMPLETENESS）；
   - 截断判据改为"同 index 不同 term 即截断"后，**旧镜像条目**又顶掉**已提交的新条目**（LOG MATCHING）；
     ⇒ 唯一正确的权威是**库自己的日志尖端**：库不再持有的才丢、库还持有的必须留、合并按 index 覆盖 term。
   - 黑盒日志尖端（`last_index`）取自 persist 视图 ⇒ 增量视图的尖端只是快照边界 ⇒ 重启后的新 leader 被算成"缺条目"
     ⇒ 误报 leader completeness ⇒ 必须**由库日志推导**。
2. **镜像不变量需要单一收口**：所有改镜像的路径末尾调用同一个归一化函数（自 `disk_lii` 起的**连续前缀**），
   否则会出现"边界 1 + 首条目 3"这类**空洞镜像**，库会正确地拒绝恢复，节点死在无故障尾段里表现为**假 LIVENESS 失败**。
3. **事件缓冲容量**：环缓冲（`EV_MAX`）太小会把失败种子的前半段事件挤掉（症状：oracle 记的"首次应用"在 dump 里找不到）
   ⇒ 要么放大，要么失败时把整个环落盘。
4. **seed 必须可指认**：在事件流里打 `SEED n`，否则只能按调用规模二分去找失败种子。
5. **调用规模影响行为**：任何跨 seed 未重置的全局（尤其**绝对**分配序号形式的 OOM 武装）都会让"同一 seed 在不同 `count` 下结果不同"。

---

## 5b. 一键门：`./build.sh regress`

自动化的**唯一稳定入口**（agent、cron、CI 都用它，不要各自拼命令）：

```bash
./build.sh regress quick     # 默认：build(0 告警断言) + 4 个单元 + selftest + CLI smoke            ~2.5 min
./build.sh regress fuzz      # 再加 raft_fuzz 2000 / raft_cluster_fuzz 200 / kserver_cluster_fuzz 1  ~5 min（**CI 在每次 push/PR 跑的就是它**）
./build.sh regress full      # fuzz 用发布级参数(20k/2000/10) 并追加 release soak 24 轮                ~20 min（nightly）
```

- 每层输出一行 `GATE|<层>|pass|FAIL|<判定行>|<秒>`；**判定行必须出现**——空跑（exit 0 但无判定行）判 **FAIL**，
  这就是"0 failures 不等于 0 data"的落地；
- **末行机器可读**：`REGRESS|quick|pass=7 fail=0 duration=150s`（agent 只读这一行即可判定）；
- 失败时打印该层的日志路径与末尾 12 行；日志在 `build/regress/<层>.log`；
- 门自身的失败路径有自检：`bash tools/harness/regress_selftest.sh`（期望 `pass=1 fail=2`）。

## 6. 推荐门序与时间预算

| 场景 | 命令序列 | 预算 |
|---|---|---|
| 日常小改 | L0 + L1a/b/c/d/e + L2 | ~2.5min |
| 触及 raft/集群语义 | 上一行 + `raft_fuzz 1 2000` + `raft_cluster_fuzz 1 2000 0`（默认延迟再跑一次）+ `kserver_cluster_fuzz 1 5` | ~5min |
| 触及服务端/传输/快照 | 上一行 + L4（24 轮）+ `--apply-stress`（`K_ALLOC_DEBUG` 构建 + appverif/gdb） | ~10min |
| 发布前 | 上一行 + L6（`coverage`、`run-sanitize`）+ `perf_matrix.sh` | ~20min |

**过程产物与清理**：
- **`build/` 是纯产物目录，初始应为空**：`./build.sh` 生成的全部内容都在这里，`./build.sh clean` 会**整目录删除**（等同全新 checkout）。
  **不要把任何手写文件放进 `build/`** —— 脚本与数据已各自归位（见下）。
- 工具脚本分两处：`tools/harness/`（12 个可复用 harness，见下表）与 `tools/archive/`（16 个早期一次性调查脚本，
  其结论汇总在 `doc/investigations.md`）。
- 测量与调查记录归档在 `doc/measurements/`（`*.txt` = 每次基准的 `STATS/PHASE/LAT` 原始数据，
  `*.out/.err` = 调查脚本输出；**不可字节级复现**）。读法见该目录的 `README.md`。
- 需要历史数据时先查 `doc/measurements/README.md` 的头部数字，再决定是否重测。

## 可复用 harness 索引（tools/harness/）

全部脚本会自行切到仓库根目录并设置工具链环境，可在任意位置调用；`set -u` 下均带合理默认值，可裸跑。

| harness | 用途 | 命令 | 判定行 | 前提 |
|---|---|---|---|---|
| `soak_release.sh` | 发布版重复深流水 + 每轮存活检查 | `RUNS=24 bash tools/harness/soak_release.sh` | `rounds_without_full_success=0`、`final liveness: 1` | 端口 9481/9482 空闲 |
| `pipe_verify.sh` | 真实客户端流水线 + 三道正确性门 | `bash tools/harness/pipe_verify.sh [K]` | 三档均 `ok=<n> not_found=0` | 端口 9931/9932 |
| `pipe_frontier.sh` | 吞吐/延时前沿（逐响应计时） | `bash tools/harness/pipe_frontier.sh` | 各档 `ops_per_s` 与分位 | 同上 |
| `burst.sh` | 开环容量（K≥N，不受客户端自限） | `bash tools/harness/burst.sh` | 峰值 `ops_per_s` | 同上 |
| `perf_matrix.sh` | 归因矩阵：轮次/每写/WAL(mem vs disk)/载荷/合批 | `N=2000 bash tools/harness/perf_matrix.sh` | 各格 `ops_per_s` + `flush_by_*` | 需 `proc_time` 已构建 |
| `watch_counters.sh` | 反复压测 + 打印准入计数器（找单调增长＝泄漏） | `RUNS=16 bash tools/harness/watch_counters.sh` | 计数器是否增长 | 同上 |
| `cpu_probe.sh` | 服务端 CPU vs wall（CPU-bound 还是等待） | `bash tools/harness/cpu_probe.sh [tag]` | CPU% | `tools/proc_time.c` |
| `conn_leak.sh` | 多个短连接后是否仍能接受新连接 | `bash tools/harness/conn_leak.sh` | 每轮是否被接受 | 同上 |
| `stall_hunt.sh` | 卡死控制实验（release vs debug+Heaps） | `RUNS=20 bash tools/harness/stall_hunt.sh` | `stalls=0` 且 `server_alive=1` | 自带探活与"无 ok= 即中止" |
| `crash_hunt.sh` | gdb 下重复深流水直至崩溃，留 backtrace | `bash tools/harness/crash_hunt.sh` | gdb 栈帧 | 需 gdb |
| `pageheap_hunt.sh` | appverif Heaps + 重复深流水（写时定位） | `RUNS=60 bash tools/harness/pageheap_hunt.sh` | 无 `Free Heap block …` / 存活 | 需 `appverif`（用完必须 disable） |
| `diag_deep.sh` | 深 K 下连接是否被服务端关闭 | `bash tools/harness/diag_deep.sh` | 服务端日志与客户端报告 | 端口空闲 |

**交付判据**：L0 0 error/0 warning；L1 全绿；L2 PASS；L3 `done`；L4 `rounds_without_full_success=0` 且 `final liveness: 1`；
工作目录无残留（如 `kdb-selftest-*`）；无残留进程（`ps -W | grep -icE 'kdbsvr|kdbctl'` 为 0）。
