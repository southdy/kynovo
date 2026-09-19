# kynovo 工程原则（及其强制方式）

> **本文件的判据**：一条原则只有在**被违反时会响亮报错**才算真的存在；只写在文档或注释里、
> 依赖"记得别那么做"的，都记在 §7 的**危险清单**里。

机械检查：`python tools/check-principles.py` → `PRINCIPLES|OK|rules=13 fail=0`（非零退出即违规）。
它已接入 `./build.sh regress`（每层一行 `GATE|principles|…`），因此**本地与 CI 都会跑**。
设计约束（都是踩过的坑）：**注释必须被剥离**（否则"the worker inline"这类散文会误报，误报的检查最终被人忽略）；
`long long` **不在禁用之列**（本仓有意用 `unsigned long long` 定义 64 位类型，见 §7-1）；对**已知且已挂账**的
违规用**预算（ratchet）**：只减不增，且消息里点名对应卡片；`doc/measurements/` 下的 CRLF **明确豁免**——
那是抓取到的归档证据，重排它就是毁掉它。

## 1. 语言与平台（硬约束）

| 原则 | 为什么 | 代码落点 | 当前强制方式 |
|---|---|---|---|
| C89：无 `inline`、无 VLA、无 `stdint.h`、不在语句后声明 | 目标含 MSVC 6.0 与 Windows XP | `code/*` | **机械**：checker（禁 `inline`/`stdint.h`）+ 编译门 `-std=c89 -Wdeclaration-after-statement` |
| 不用 `ULL` 字面量 | MSVC 6 不认 | `code/*` | **机械**：checker（`tests/`、`tools/` 豁免——它们只由 gcc 编译，已在消息中注明） |
| 64 位整数经 `kbase.h` 的 `__int64` 包装 | MSVC 6 只有 `__int64` | `kbase.h` | ⚠ **未强制**——见 §7-1（本仓另有 `unsigned long long` typedef，与 MSVC 6 冲突） |
| 只用 XP 及更早的 Win32 API | 目标 XP+ | `code/*` | **机械**：checker 黑名单（`GetQueuedCompletionStatusEx`/`GetTickCount64`/`CreateFile2`/…），注释里的说明不算 |
| 换行一律 LF | 脚本要能在 bash 下跑；`sed`/`grep` 行为一致 | 全仓 | **机械**：checker 断言**仓库索引** `i/crlf = 0`（仓库实际存储 ✓）+ 脚本 `w/crlf = 0`；CI 另有 LF 步骤 |
| 归档证据不重排、不"规范化" | 复现判断依赖原始记录 | `doc/measurements/` | 纪律 + 上面那条**豁免**即是执行（CRLF 记录被有意保留） |

## 2. 目录与产物

| 原则 | 强制方式 |
|---|---|
| `build/` 是纯产物目录、初始为空、**不得手写文件**、`clean` 整体删除 | **机械**：checker（`git ls-files build/` 必须为空） |
| 手写材料各有其家：可复用 harness → `tools/harness/`；一次性调查 → `tools/archive/`（结论写入 `doc/investigations.md`）；原始记录 → `doc/measurements/` | 纪律（新脚本的落点由 code review 把关） |
| 脚本内的仓库根**必须自推导**，不得硬编码 | **机械**：checker（被跟踪 `*.sh` 中不得出现盘符路径；`disk://` 这类 URI 前缀不算） |

## 3. 契约与所有权

| 原则 | 为什么 | 强制方式 |
|---|---|---|
| **`ready` 必须完整上报**：应用层一切状态从 `raft_advance` 返回的 ready 获取，`raft_inspect` **仅供观测端点** | 否则观测接口会渗进决策路径 | **机械 ratchet**：应用层与测试中 `raft_inspect` 站点预算 = **1**（唯一合法站点 = STATS 端点 `kserver.h:3326`），增长即报错 |
| 快照视图对**非属主线程只读**；`capture` 与 `finish` 必须在属主线程 | 快照线程流式读取期间主线程仍在改 treap | 纪律（`treap.h` 头注释写明不变量） |
| 释放点必须在**契约终点**，不是"数据被复制的地方" | 请求交给 raft 后仍是它的 cookie 与载荷 | 纪律 + 回归（`k_request_free` 唯一释放点） |
| 每个 close 站点：**先存、后清、再关** | cemon 关闭后对象即失效，残留句柄 = 延迟 UAF | 纪律 + 崩溃类回归 |
| **不丢请求**：任何接受请求的路径必须以应答或可见日志结束 | 静默丢弃曾让客户端永久挂起 | 部分机械（`selftest`/`cli_smoke` 回归） |
| **fatal 必须打印原因** | 静默退出曾被当成"没发生" | ⚠ 纪律（缺口 C4 已挂卡片 `t_469a3143`） |
| 事务副本禁止 `capture`/`save`/`load` | treap 的复制语义 | 纪律（`treap.h` 注释） |
| 不得改动 `raft.h`/`treap.h` 契约而不先对齐论文语义 | 语义基准是 Ongaro 论文 | **提示性机械**：checker 检测到这两个文件被改动会打印 NOTE（要求消息里注明依据段落） |

## 4. 分层（机制层 vs 策略层）

| 原则 | 强制方式 |
|---|---|
| 机制层只对**合法输入与用法**负责，前置条件由调用方保证（`treap.h`：seed 由上层决定；`treap_load` 不校验有序/重复） | 纪律 |
| 应用层**不得直读** raft 内部字段（`config_new`/`config_joint`/`config_learners`）做策略判定 | **机械 ratchet**：预算 = **3**（缺口 C6，卡片 `t_972b67e8`），增长即报错 |
| 故障注入只走**架构已有的四缝**（transport vtable / `elapsed_ms` / runtime backend / vfs backend），不得新增侵入式钩子 | 纪律 |
| 测试代码统一放 `tests/`，不侵入业务代码 | 纪律 |

## 5. 并发与生命周期

| 原则 | 强制方式 |
|---|---|
| 引用计数**只在成功提交时归因** | 纪律 |
| 绝不在非属主线程 free/改写共享状态 | 纪律（崩溃类 ③ 的根因） |
| 关闭后立即清句柄（`void *dead=h; h=0; close(dead);`） | 纪律 + 回归 |
| 多线程调试分配器必须加锁 | 纪律（仪器自伤过一次；已加锁） |

## 6. 测量与证据

| 原则 | 强制方式 |
|---|---|
| **"0 failures" 不等于 "0 data"**：装置必须自证（断言正向信号 `ok=<N>`/`done:`/存活） | **机械**：`regress` 的判定行机制（无判定行 = FAIL）+ `tools/harness/regress_selftest.sh` |
| 单一来源信任：`PHASE` 行不算证据 ⇒ 需 `PROBE`/`RESP` 为 accepted + commit 前移 + 读回 | 纪律（`kynovo-storage` 技能） |
| **同构建 A/B**，不跨构建比较 | 纪律 |
| 单次干净运行不是证据（低概率缺陷用数十次采样或长程门） | 纪律 |
| 多不变量 harness 失败 ⇒ **先找第一处分歧**，不从失败点往回推 | 纪律（`AGENTS.md`） |
| 仪器必须回退；临时打印必须带**自标识标签** | **机械**：checker 禁残留标签（`TEMP-INSTR`/`TEMP-AB`/`INSTR-<X>`/`XXX-`/`HACK-`）——注释里的说明不算，**代码里的才算** |
| 被否证的假设必须**显式撤回**（写在 `doc/gaps-audit.md`） | 纪律（那是该文件的意义） |

## 7. 危险清单：只靠纪律（无机械强制）的原则

这些是**最容易被误打破**的一类 ✓。按风险排序，以及可选的机械化方向：

1. **§7-1 `long long` × MSVC 6.0（本次梳理最重要的发现 ✗）**
   `vfs.h:16` `typedef unsigned long long vfs_u64;`，`raft.h:104` `raft_u64`、`cemon.h:16` `cemon_u64`、
   `runtime.h` 同类。而 **MSVC 6 不支持 `long long`**（只有 `__int64`）。也就是说 **"MSVC 6.0 兼容"这条原则
   在工具链层面从未被验证过** ✗（本机也没有 MSVC 6 可试）。两种出路：① 用 `_MSC_VER` 分支把 64 位类型定义
   成 `__int64`（小改动，但触及多个头文件的类型定义）；② 明确**把原则降级**为"C89 风格 + gcc/MinGW 工具链在编"，
   并在 `AGENTS.md` 里改词。**在拍板前不要两者都做**。已挂卡片。
2. **快照视图所有权**（capture/finish 在属主线程）：可机械化的方向 = 在 `treap.h` 里加 `owner_thread` 断言。
3. **fatal 必须有原因**（C4）：可机械化的方向 = 一个"每个 `exit(1)` 附近必须有 printf"的检查（噪声大，暂缓）。
4. **不丢请求**：可机械化的方向 = 在每个"接受即丢弃"的分支插自标识标签，再由 harness 断言零命中。
5. **同构建 A/B / 单次干净不算证据 / 先找第一处分歧**：方法论，不可机械化——但它们已经写进 `AGENTS.md` 与技能，
   由每次评审把关。

## 8. 改动本文件的规矩

新增一条机械规则时：**先证明它能失败**（注入一处真违规 → 看到它报红 → 精确回退 → 残留计数为 0），
再接入 `regress`。做不到"能失败"的规则不要加——加进 §7 反而更诚实。
