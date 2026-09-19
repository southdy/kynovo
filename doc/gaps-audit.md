# kynovo 工程缺口清单（审计产物）

审计方式：3 路模块深度审计（raft.h ↔ dissertation；cemon/runtime/vfs 平台与机制；kserver/kclient/treap/kproto/app）
+ 我本人在同一棵树上独立核实。**核实状态逐条标注**：
- `[核实]` = 我读过该处代码确认结论成立
- `[待核实]` = 审计报告给出，我尚未独立确认（引用 file:line 供复核）

已核对的**无缺口**部分与**既定取舍**见文末。

---

## A. 高严重度

### A1 `[核实]` FCALL 门可永久关闭 → 所有后续写请求永久排队、无答复、不重定向
- 证据：`code/kserver.h:2758`（`gate_closed=1` 在可能失败的 flush 之前）; 仅 `2772`（FCALL 成功）、`2814`（失去领导权）清零；
  客户端断开时 FCALL 请求在 `3535-3551` 被 `k_request_free` 而**门不开**。
- 影响：该连接断开后，`2830` 把所有非读请求送进 `gate_head` 永久排队 ⇒ 客户端永久挂起（叠加 A2），
  且 `gate_head` 无长度上限、`request_count/request_bytes` 不释放 ⇒ 最终触发准入拒绝。
- 关联：`code/kserver.h:4236`（A5）使被暂停的连接也不会被唤醒。
- 方向：连接消失时若其请求持有 FCALL，必须走与 `k_server_fcall_fail` 相同的收尾（开门 + 答复/可见日志）。

### A2 `[核实]` 客户端对"已发出的请求"没有任何超时（仅重定向/重连有界）
- 证据：`code/kclient.h:109,392`（唯一有界机制 `K_CLIENT_RETRY_MS`，仅用于 leader 未知/重连）;
  `376-409`（`k_client_poll` 只处理 retry/重连）。
- 影响：服务器一旦静默丢失结果（A1/A3/B1/B4 任一条），客户端无限等待；`pending` 非空使后续命令全部被拒
  （`kclient.h:284-287` 返回 0 却不排队）⇒ 整条会话死掉。

### A3 `[核实]` `kdbctl` 一次性模式超时返回 `EXIT_SUCCESS`（假成功）
- 证据：`code/kdbctl.c:503`（3s 超时 break）; `516`（只有 `cemon_poll` 失败才 `rc=-1`）; `528`（`return rc`）; `548`（`rc==0?EXIT_SUCCESS`）。
- 影响：`kdbctl <host> SET k v` 在服务器无响应时无输出且 exit 0；脚本/CI 会把失败当成功。

### A4 `[核实]` cemon 允许跨线程 `cemon_send/cemon_recv`，但发送队列与配额完全无锁（数据竞争）
- 证据：`code/cemon.h:238-244`（头文件明示 "other threads may still post and send while the mailbox is open"）;
  `2749-2759`（链表/`send_cost` 在 `cemon_socket_admit` 的短暂持锁之外被改）; 并发对手 `2238-2240,2364-2366,1133-1145`。
- 影响：可构造交错导致帧入队后不可达 ⇒ 永不发送 + 配额永久泄漏 ⇒ 累计到上限后所有 `cemon_send` 返回 -1。
  仓库内当前无人跨线程发送（仅 owner 线程）⇒ 属**契约与实现不一致的潜在**缺陷。

---

## B. 中严重度

### B1 `[核实]` RGET 的 flush 失败路径仍然"释放请求且不答复"
- 证据：`code/kserver.h:3071-3074`（与已修的读路径 `2846-2852` 对照，此处仍是 `k_request_free` + `return -1`）。
- 影响：客户端只看到断连、得不到状态码；该连接其它在飞请求一起丢失。

### B2 `[核实]` WAL 写失败被当成"优雅停机"：退出码 0 + 只打印 `stopped`
- 证据：`code/kserver.h:4186-4188`（`!wal_ok` → `poll_wal` 返回 -1）→ `4496`（`drive()!=0` → `begin_stop`，**不设 fatal**）→
  `kdbsvr.c:248`（`if(!stopped||fatal) rc=-1`）→ `kdbsvr.c:320-321`（打印 `stopped`，`EXIT_SUCCESS`）。
- 影响：磁盘/IO 写失败对监督进程呈现为干净退出，且无原因行（与"fatal 必须打印原因"的运维标准冲突）。
- 注：磁盘满 fail-stop 本身是你已认可的取舍；此条是**退出语义与可见性**问题。

### B3 `[核实]` `k_client_queue` 在已有 pending 时返回 0（"成功"）但并未排队
- 证据：`code/kclient.h:284-287`。调用方（kdbctl 的 22 个命令处理函数）只检查 `!=0`；
  重试逻辑靠 `strstr(g_cli_last,"wait for the current request")` 匹配**用户可见文案**（`kdbctl.c:499,505`）。
- 影响：命令被静默丢弃；任何文案措辞改动即破坏 CLI 行为。

### B4 `[核实]` 准入/限额触发的行为不可见且会静默误伤连接
- 证据：连接数上限 → 直接 close 无日志 `code/kserver.h:3320`；接收缓冲超限 → 直接 close 且该连接在飞请求不答复 `3336-3339`；
  请求级限额复用 `"out of memory"` 文案 `1998` + `2835/2745`。
- 影响：客户端无法区分"容量保护"与"对端 OOM"；连接级拒绝完全不可观测。

### B5 `[核实]` `cemon_recv` 武装失败后遗留 `recv_armed=1`，且没有补投路径
- 证据：`code/cemon.h:3426-3429`（先置 `recv_armed=1` 再 post；`ce mon_win_post_recv` 失败时 `busy=0` 且不补投；
  唯一自动补投点在 connect 完成 `2173`）。
- 影响：`cemon_recv_active()`/统计说谎；应用若不重试则连接静默失联（本会话已遇到同类症状）。

### B6 `[核实]` Windows `cemon_poll_once` 读取未初始化的 `key`（UB，可能把致命错误当唤醒吞掉）
- 证据：`code/cemon.h:3152-3171`（`ULONG_PTR key;` 未初始化即传给 `GetQueuedCompletionStatus`；
  MSDN：失败且 `lpOverlapped==NULL` 时 `lpCompletionKey` 不被写入）。

### B7 `[核实]` 非 Windows（epoll/kqueue）分支**从未被编译**，其中存在 C89 违规
- 证据：`code/cemon.h:2459`（`int emit_rc=...` 在语句之后）、`2621`（`int err=...` 在语句之后）；
  `build.sh` 全部为 Windows 构建（无 Makefile/CI）。
- 影响：本次审计范围内的"Unix 平台"实为不可验证代码；用仓库自己的 `-Wdeclaration-after-statement` 即失败。

### B8 `[核实]` 快照"写后校验"不覆盖整文件（仅尾部 4 字节 + 末尾探测）
- 证据：`code/kserver.h:3910-3925`（CRC 源是内存流、回读只比 trailer）、`4000`（随即 `raft_snapshot_data_ready`）、`4004`（随即删旧快照/旧段）。
- 影响：中部静默损坏不会阻止旧快照删除，回滚材料可能同时消失；下次启动才 fail-stop。

### B9 `[待核实]` 恢复的 base 回退分支路径存在"夹高 base 后仍用最新记录解码 ⇒ 基址校验必然失败"的分支
- 证据：`code/kserver.h:1531-1534`（`n_base=prev_base; base_of_use=n_base; pick_base=1;`）与 `1579-1583`（`last_included_index!=base_of_use` ⇒ 拒绝恢复）。
- 我读到该分支另有"快照不可用则回退 prev_base"的支路（`1535-1542`），因此**只在"n_base 快照可用但最新记录携带旧 base"的子情形**下矛盾；
  该情形是 fail-stop（安全方向），但与注释自述意图不一致。

### B10 `[待核实]` runtime/vfs 三处：`runtime.h:356/380/728/746/396` 全 `INFINITE` 等待（worker 契约违规即永久挂起）；
  `runtime.h:443-451` 忽略 `pthread_condattr_setclock` 返回值；`vfs.h:96` 用 `FILE_SHARE_DELETE` 打开 WAL（Windows 上可被运行中删除/改名，且无文件锁）。

### B11 `[核实]` 应用层 fatal/参数错误静默退出（多例）
- 证据：`code/kdbsvr.c:285,289,296,297,299,327`（参数/配置错误直接 `return -1` 无输出）；
  `code/kdbctl.c:448,461`（seed 解析/初始化失败静默）。
- 影响：拼写错误表现为"进程立刻退出、无任何原因"。

---

## C. 低严重度

| 编号 | 状态 | 结论 | 证据 |
|---|---|---|---|
| C1 | `[核实]` | `kserver.h` 找不到登记项的结果被静默丢弃（无日志，而相邻分支有 warning） | `3533-3534` vs `3536` |
| C2 | `[核实]` | `recv_paused` 唤醒要求四个计数器**同时**低于上限 ⇒ 任一枚举泄漏即永久静默 | `4236-4240` |
| C3 | `[核实]` | WAL 段首记录的 generation 连续性不检查（`prev_gen=0` 每段重置） | `1465` vs `1478` |
| C4 | `[核实]` | 启动失败原因不可见（cfg/WAL meta/分配/线程失败全部只输出 `failed to start server N`） | `kserver.h:4594-4600,4602,4642,4682`; `kdbsvr.c:300` |
| C5 | `[核实]` | 头注释与实现不一致：声称 v1 数据目录"被拒绝并重新初始化"，实际只拒绝 | `kserver.h:23-26` |
| C6 | `[核实]` | 层边界：应用层直接读 `raft->config_new/config_joint/config_learners` 做策略判定 | `kserver.h:4411-4413` |
| C7 | `[核实]` | INFO/STATS 文本用无界 `sprintf` 写 `char text[2048]`（新增字段即越界） | `kserver.h:3254,3258,3260,3262` |
| C8 | `[核实]` | 快照 worker 投递失败时自行 free(task)，主线程仍持指针 ⇒ `snapshot_inflight` 永为 1 | `3957` vs `3992,4351` |
| C9 | `[核实]` | `UNIX` 分支一次武装可投递多次 `CEMON_DATA`（与头文件契约相反）；`callback_depth==0` 时同步重入回调 | `2452-2461`, `3432-3435` |
| C10 | `[核实]` | Unix 的 recv/accept/flush 循环不消耗派发预算 ⇒ 单个活跃对端可独占 owner 线程 | `2455,2395,2342` |
| C11 | `[核实]` | `cemon_ingress_close` 无界自旋；`cemon_destroy` 排空上限 2000ms 后可能泄漏 hold 的 socket | `1047-1066`, `2900-2910` |
| C12 | `[核实]` | UDP 软错误静默吞掉（无计数）；UDP 兜底分支用错缓冲（`recv->buf` 而非 `udp_recv->buf`） | `2189-2194, 2207-2213` |
| C13 | `[核实]` | 循环级致命错误只有 `-1`，无取回错误原因的接口 | `3170,3269-3275` |
| C14 | `[核实]` | `2147483647ULL` 未走仓库自己的 `VFS_U64_C` 约定（`-pedantic-errors` 报 C99 long long 常量） | `cemon.h:655` vs `vfs.h:64-66` |
| C15 | `[核实]` | 依赖 `mswsock.h` 的 `LPFN_ACCEPTEX/WSAID_CONNECTEX/SO_UPDATE_CONNECT_CONTEXT`，无回退声明；旧工具链（`/d/MinGW` 9.2.0）实测编译失败 | `cemon.h:606-608,1675-1677,2159`; `build.sh:47-58` 已有注释记录 |
| C16 | `[核实]` | Windows 完成路径用 POSIX `ENOMEM(12)` 当 Winsock 错误码 ⇒ `EV.status` 无意义 | `2242,2109` |
| C17 | `[核实]` | 无目录同步/原子 rename 原语 ⇒ "O_CREAT + fsync(file)" 崩溃后目录项可能不存在（应用层无法自行弥补） | `vfs.h:19-24,99-101,170-177` |
| C18 | `[核实]` | vfs 读把 EOF 与真实 IO 错误都返回 -1（调用方无法区分"段尾"与"介质错误"） | `vfs.h:137` |
| C19 | `[核实]` | `vfs_open/unlink` 不暴露错误码（"不存在/权限/满/被占用"不可区分） | `vfs.h:96-97,118-120` |
| C20 | `[核实]` | `kproto.h` 通用编码器不认识 `K_REQ_MEMBER`（仅手写 payload 可用） | `kproto.h:288` |
| C21 | `[核实]` | `kdbctl` RGET 把末尾纯数字参数当 limit ⇒ 数字 end 键无法作为范围终点；命令行超长时静默丢参数 | `kdbctl.c:267-270,541` |
| C22 | `[待核实]` | `kdbsvr` 未检查 `timeBeginPeriod/SetConsoleCtrlHandler` 返回值；`[cfg]` 行打印的是配置值而非生效值 | `kdbsvr.c:273,314,188` |
| C23 | `[待核实]` | `runtime.h:607` 用 `strcmp` 未包含 `<string.h>`（MinGW-w64 下靠传递包含而干净） | `runtime.h:607` |

---

## D. raft.h 与论文一致性（模块审计，严重度未超"中"）

| 编号 | 状态 | 结论 | 证据 |
|---|---|---|---|
| R1 | `[待核实]` | follower 提交规则用"自身日志末端"而非"本条消息最后一条新条目"；AE 因 OOM 降级为空心跳时可提交**本次未校验的分叉尾部** | `raft.h:3600-3602` vs `dissertation.md:7198,7442`; 触发需 `2294` 的 REALLOC 失败 |
| R2 | `[待核实]` | learner 自身可发起选举并当选（transfer 路径却显式禁止 learner 当 leader，自相矛盾） | 计票 `3432-3437,3459-3461` vs `4173-4174` |
| R3 | `[待核实]` | 读栅栏的"多数确认"只接受带成功追加的 ACK（拒绝响应不计），比论文更严格 ⇒ 追赶/收敛期间读可能超时 | `3705` vs checkQuorum 的"接触即算" `3711-3713` |
| R4 | `[待核实]` | 流式快照身份只用 `(last_index, byte_size)` 判定 ⇒ 同 index 同长度的不同内容会被拼接且无法检测 | `3814,3839` |
| R5 | `[待核实]` | `step_down` 取消待安装快照，而调用方可能仍在提交该安装 ⇒ `last_applied` 可越过日志末端（当前 kdbsvr 驱动方式下不可达） | `1300-1306` vs `4326` |
| R6 | `[待核实]` | 领导权转移在"发出 TimeoutNow"即上报 COMMITTED（论文要求目标当选且原 leader 让位） | `3120-3126` vs `dissertation.md:1495-1507` |
| R7 | `[待核实]` | 引导未把初始配置写成日志第一条条目（改用调用方静态配置） | `2845-2853` vs `dissertation.md:2119-2125` |
| R8 | `[待核实]` | 被拒绝的 AE 会携带**尚未持久化**的新任期对外传播（投票/AE 成功路径都有落盘闸门，此处没有） | `1277-1278` → `3660-3671` |

---

## E. 覆盖缺口（测试与流程）— 我本人核实

1. **CLI 一次性模式无任何回归测试**：本会话那条"等待循环零轮询"的缺陷正是因为该路径无测试而存活；
   现已有 `tools/cemon_tcp_probe.c` 可做协议级最小客户端，建议据此补一条端到端回归（`SET/GET/DEL/STATS`）。
2. **`kdbctl` 假成功（A3）无测试**：需一条"服务器无响应时必须非零退出"的用例。
3. **非 Windows 构建路径不在任何构建/测试中**（B7）。
4. **无 CI**（`build.sh` 需手动跑；无 `make test` 之类的单入口）。
5. **`tools/` 下的 bench/probe 未纳入测试阶梯**（`bench_rate`/`cemon_tcp_probe` 是本次排障的关键工具，但只靠手工调用）。
6. **fence/任期回归类极端场景的 fuzz 覆盖未做**（此前遗留）。

---

## F. 已核对且**未见缺口**的部分（要点）

- **Windows XP 兼容性**：`GetQueuedCompletionStatusEx/CancelIoEx/WSAPoll/GetTickCount64/InitializeCriticalSectionEx/CreateThreadpool*/SetFileInformationByHandle/snprintf/vsnprintf/strtoull/inet_pton` 等 Vista+ 或 XP 缺失项**全部未被使用**；
  `_WIN32_WINNT 0x0501` 在包含头文件之前定义（三个文件均正确）；`AcceptEx/ConnectEx` 为 XP+ 且用 `WSASocketA + WSA_FLAG_OVERLAPPED` 正确配对。
- **cemon 机制**（Windows 路径）：accept 完成顺序（`SO_UPDATE_ACCEPT_CONTEXT` → 关联完成端口 → 标记 OPEN → emit）正确；读/写/accept 武装三重幂等守卫；半关闭状态机表驱动；定时器堆与溢出保护；背压限额在分配之前检查；wake 丢失唤醒已修并有注释说明。
- **所有权模型自洽**：`kdbsvr.c:301-304`、`kdbctl.c:449-451` 显式 `cemon_bind_owner`；worker 只经 `cemon_post` 触达 loop。
- **持久化**：WAL 元数据槽写 + fsync 同调用完成、双槽 4KiB 间隔、magic/version/CRC 校验；恢复三分（撕裂尾部跳过 / CRC 与代际不符置 `have_bad` / 无可用记录拒绝）；快照先持久化元数据再截断日志。
- **请求生命周期**：登记失败全部有答复；读栅栏被拒 → REDIRECT 且校正 `is_leader`；hash 摘链无条件执行；ready 拷贝成功后才消费。
- **代码卫生**：`code/`、`tests/`、`tools/` 全程 **CRLF=0、非 ASCII=0**、无 `TODO/FIXME/XXX` 残留；`-std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement` 下 **0 error / 0 warning**（Windows 分支）。

---

## G. 按用户既定取舍**不再列为缺口**

1. §6.3 客户端会话 / exactly-once 未实现（有意：靠指令幂等根治）
2. 磁盘满 fail-stop（已认可可接受）
3. README / metrics（延后）
4. 多节点真机吞吐（当前不具备条件，暂不做）

---

## H. 建议处理顺序（前三位）

1. **A1 + A2**（FCALL 门永久关闭 + 客户端无请求超时）：唯一能造成"完全无响应"的组合，且 A2 是任何丢结果场景的放大器。
2. **A3**（CLI 假成功）：脚本化使用的基本正确性；同行补 E1/E2 的回归测试。
3. **B1/B4/B11**（静默丢弃与静默退出的可见性收口）+ **A4**（cemon 跨线程契约与实现对齐：要么加锁，要么把头文件措辞改为"仅 owner 线程可发送"）。

---

# 修复状态（本轮，2026-09）

## 已修复并验证

| 条目 | 修法摘要 | 验证 |
|---|---|---|
| A1 FCALL 门永久关闭 | `k_request_free` 中任何 FCALL 被释放即重开闸门（`open_gate` 先清标志 ⇒ 递归安全） | `kserver_test` 新增用例 ✓ 33/33 |
| A2 客户端无请求超时 | `K_CLIENT_REQUEST_TIMEOUT_MS`(默认 5s, 可配) + `pending_deadline_us`；超时显式报错并释放槽位；清 discovery 标志 | kclient 14/14 ✓ |
| A3 kdbctl 假成功 | 成功判据结构化（`cmd_id` + `last_done_id/status` + `!busy`）；失败打印原因并 rc=1；总时限 10s；**kdbctl 中再无 `strstr`** | `tests/cli_smoke.sh` 13/13 ✓ |
| A4 cemon 跨线程契约 | **按用户指示**：头文件契约改为"仅 owner 线程可 send/recv/close"（`cemon_post` 仍为唯一跨线程入口），非 owner 发送/武装读 fail-fast 返回 -1 | 全阶梯 ✓（仓库内无跨线程发送者） |
| B1 RGET flush 失败静默丢弃 | 改回错误应答（与已修的读路径一致） | kserver 33/33 ✓ |
| B2 WAL 写失败伪装成优雅停机 | `fatal=1` + 打印 `fatal: WAL write failed` + 非零退出 | 套件+selftest ✓ |
| B3 命令未排队却返回 0 | 新增 `k_client_busy()` 与 `last_done_id/last_done_status`，CLI 不再匹配文案 | cli_smoke ✓ |
| B5 武装失败遗留 recv_armed=1 | 失败时清标志（Windows 与 Unix 两分支），不再谎报"已武装" | 套件 ✓ |
| B6 未初始化 completion key | `ULONG_PTR key=0` | 套件 ✓ |
| B7 非 Windows 分支 C89 违规 | 三处改为块首声明，可被仓库自己的 `-Wdeclaration-after-statement` 抓住 | 静态修正 ✓（Unix 分支本机不可编译，见遗留） |
| C1 未知结果静默丢弃 | 打印 warning（含 status 与 cookie） | 套件 ✓ |
| C8 快照任务双释放 | worker 不再 free（主线程持有该指针），改为"泄漏优于 use-after-free"并注释说明 | 套件 ✓ |
| C14 `2147483647ULL` | 去掉 C99 后缀（`-pedantic-errors` 干净） | 构建 ✓ |
| C16 POSIX ENOMEM 当 Winsock 码 | 新增 `CEMON_ERR_NOBUFS`（Windows=WSAENOBUFS，Unix=ENOMEM） | 构建 ✓ |
| C20 编码器不认识 K_REQ_MEMBER | `k_request_payload` 支持（body 原样放 key） | 套件 ✓ |
| C21 RGET 末尾数字歧义 | 改为 `limit N` 关键字；用法文案同步 | cli_smoke ✓ |
| C22 kdbsvr 忽略返回值 | `timeBeginPeriod`/`SetConsoleCtrlHandler` 失败时打印可见告警 | 构建 ✓ |
| C23 runtime 缺 `<string.h>` | 补包含（并补 `<stdio.h>` 供告警） | 构建 ✓ |
| B10 runtime 全部 INFINITE 等待 | 有界等待（30s）+ 可见告警；`pthread_condattr_setclock` 返回值检查；vfs 打开去掉 `FILE_SHARE_DELETE` | 套件 ✓ |
| R1 follower 提交封顶 | 改为 `prev_log_index+entry_count`（图 2/§5.4.2） | raft 221/221 ✓ |
| R2 learner 竞选 | `raft_become_candidate` 拒绝 learner | raft 221/221 ✓ |

## 评估后有意不改（附理由）

- **R3** 读栅栏只计成功 ACK：注释理由成立（拒绝仅证明可达，不证明持有 leader 提交前缀）⇒ 保守且自洽
- **R4** 快照身份仅 (index,size)：kdb 快照序列化对给定 (index,term,cfg) 确定；触发需"同长不同内容"
- **R5** `step_down` 取消待安装快照：第一版修法破坏了"应用可上报超过日志末端的 applied"这一被测试编码的契约 ⇒ **撤回**，保留为低危（仅异步安装驱动可达）
- **R6** 转移即报 COMMITTED：注释写明是"已启动"语义；严格 §3.10 语义属后续可选
- **R7** 引导配置不写成日志首条：API 设计选择
- **C17 目录同步原语**：**用户明确不需要** ⇒ 不实现
## 仍未处理（完整清单，2026-09 第二轮校正）

> 校正说明：此前一条口头总结误把**子代理 cemon 审计的编号**（其 A3/A5/A6/A7）当作本清单编号列出 ✗。
> 对应关系：子代理 A5 = 本清单 **C16（已修）**；子代理 A6/A7 = 本清单 **B10（已修）**；
> 子代理 A3 = 本清单 **B7（已静态修，运行期未验证）**；子代理 A1/A2/A4 = 本清单 C14（已修）/C15（已注释说明）/A4（已按用户指示改契约）。

### 安全/持久化相关（优先）

| 条目 | 现象与证据 | 影响 | 为何未修 | 修法要点 |
|---|---|---|---|---|
| **C18** | `vfs_read` 把 EOF 与真实 I/O 错误都返回 -1（`vfs.h:137/140`）；恢复扫描据此把"读失败"当作**段正常结束**（`kserver.h:1466-1472`） | 介质错误 ⇒ WAL 被当作提前结束 ⇒ **静默恢复出截断状态**（最值得修的一条） | 需要 vfs API 扩展（`vfs_size`/`eof` 出参） | 加 `vfs_size(file,&size)`；扫描时 `off<size` ⇒ 打印原因 + fail-stop；成本 ~20 行 |
| **R8** | 被拒 AE 的响应在 `step_down`（仅置 `persist_needed`）后**同一次调用内**发出并携带新任期；而成功 ACK 有 `deferred_append_gen` 落盘闸门（`raft.h` `deferred_append_response` 块） | 该节点在闸门前崩溃 ⇒ 对外声明过的任期无持久化支撑（§3.8）；安全性影响弱（同任期双主仍需多数票交集） | 需要为"拒绝响应"复用延迟机制（中改动，且要保证不会拖慢 leader 重试） | 复用 `deferred_append_gen/deferred_append_response`，把拒绝也挂到 `persist_gen` 完成后发出 |
| **B9** | 恢复 base 回退分支：`n_base=prev_base; base_of_use=n_base; pick_base=1`（`kserver.h:1531`）后仍用最新记录解码，基址校验必然失败 ⇒ 拒绝恢复 | 与注释自述意图不符（注释说"回退到更高 base"）；**方向是 fail-stop**，不产生错误数据 | 该分支只在"n_base 快照可用但最新记录携带旧 base"的罕见子情形触发；彻底修需重排 base/记录选择逻辑 | 先定 `base_of_use`（可用的更高 base），再选**基址与之相符的最新记录** |
| **C11** | `cemon_ingress_close` 无界自旋（`SwitchToThread` 轮询）；`cemon_destroy` 排空上限 2000ms 后可能残留被 hold 的 socket 结构 | 生产者契约违规 ⇒ 进程无法关闭；极端下泄漏 socket 结构（端口已关闭，无完成可触达） | 需构造"完成事件丢失"的可达序列；当前未构造出 | 给自旋加超时 + 打印；destroy 超时后打印残留计数 |

### 可见性（不改变行为，但影响排障）

| 条目 | 现象与证据 | 影响 | 修法要点 |
|---|---|---|---|
| **C4** | `k_server_open` 的失败（cfg/WAL meta/分配/线程）只返回 -1，调用方只打印 `failed to start server N`（`kdbsvr.c:300,308`） | 启动失败无原因 ⇒ 违反"fatal 必须打印原因"的运维标准 | 在 `k_server_open` 各失败点打印具体原因（沿用现有 `[cfg]`/`wal:` 前缀风格） |
| **C5** | 头注释称 v1 数据目录"被拒绝并直接重新初始化"（`kserver.h:25`），实现只拒绝 | 注释与行为不一致，误导使用者 | 改注释为"拒绝启动（fail-stop)" |
| **C13** | 循环级致命错误只有 `-1`（`cemon_poll`），无取回原因的接口 | 应用无法区分"正常停机收敛"与"后端错误"，也无法记下 errno | 加 `cemon_loop_last_error(loop)` 之类的只读访问器 |
| **B4** | 连接数上限 ⇒ 直接 close 无日志（`kserver.h:3330`）；rx 超限 ⇒ 直接 close 且该连接在飞请求不答复（`3346`）；请求级限额复用 `"out of memory"` 文案（`3355` 附近 + 1998） | 客户端无法区分"容量保护"与"对端 OOM"；连接级拒绝不可观测 | 限额触发时打印一行（含限额名与当前值）；请求级限额用独立文案 |
| **C2** | `recv_paused` 的重新武装要求四个计数器**同时**低于上限（`kserver.h:4258`） | 任一枚举泄漏即让所有被暂停连接永久静默（与已修的 A1/门问题叠加时尤其危险） | 记录"为何暂停"（原因位），只等该原因对应的计数器回落 |
| **C12** | UDP 软错误静默吞掉且无计数（`cemon.h` `cemon_win_udp_recv_soft_error` 分支）；UDP 兜底分支误用 `recv->buf`（`cemon.h` 的 `else if(bytes>0)` 分支） | UDP 丢包不可见；兜底分支当前难触发（`WSARecvFrom` 总写回 addr_len） | 加一个 UDP drop 计数 + 修正兜底分支 |

### 平台不可验证（本机为 Windows，Unix 分支不参与任何构建）

| 条目 | 现象与证据 | 影响 |
|---|---|---|
| **B7 的验证** | 三处 C89 违规已静态修正，但**无法编译验证**（MinGW 无 `sys/epoll.h`） | 静态修正正确性靠人工；建议在 Linux 上做一次 `-fsyntax-only` 门 |
| **C9** | Unix 一次武装可投递多次 `CEMON_DATA`；`callback_depth==0` 时同步重入回调（与头文件契约相反） | 同一份应用代码在两平台流控语义不同 |
| **C10** | Unix 的 recv/accept/flush 循环不消耗派发预算 | 单个活跃对端可饿死定时器与其它 socket |

### 成本/收益取舍（未实施）

| 条目 | 现象 | 为何暂缓 |
|---|---|---|
| **B8** | 快照写后校验只回读 trailer + 末尾 1 字节探测（`kserver.h:3936`），不校验整文件；随后即删旧快照 | 已保留两代回滚材料；整文件回读的 I/O 代价与收益需你确认后再做（可选折中：头部/中部/尾部抽样回读） |
| **C7** | INFO/STATS 文本用无界 `sprintf` 写 `char text[2048]`（`kserver.h:3264`） | 当前字段集未溢出；改动应配合"字段超限即截断并告警"的策略 |
| **C19** | `vfs_open`/`vfs_unlink` 不暴露错误码 | 属 API 扩展；与 C18 的 `vfs_size` 可一并做 |
| **C3** | WAL 段首 generation 连续性不检查（每段 `prev_gen=0`，`kserver.h:1465`） | 需要构造"跨段丢失且只携带 term/vote/base 的记录"用例才能确认影响 |
| **C6** | 应用层直接读 `raft->config_joint/config_new/config_learners` 做策略判定（`k_server_silent_standby`） | 机制层无对应查询 API；加 API 属接口扩展，需与"层边界"取舍一起定 |
| **C17** | 目录同步原语 | **用户明确不需要** ⇒ 不实现 |


## 新增测试

- `tests/cli_smoke.sh`：端到端 CLI 语义（SET/GET/STATS/MEMBERS/DEL/缺失键）+ **失败必须非零退出**（死主机、未知命令）⇒ 覆盖 E1/E2
- `tests/kserver_test.c` 新增用例 `[1/33]`：FCALL 门在其请求死亡后必须重开（A1 回归门）

## E. 快照视图所有权（已修，附证据与撤回记录）

### E1 `[已修]` `treap_save` 在快照线程执行了属主线程的动作
- **机制**：`treap_capture`（主循环）把 live 退休链**认领**进 `t->snap` 并清空 live 链；随后 `treap_save` 被**快照线程**调用，而它当时除了遍历 `t->snap.root` 发字节外，还做了三件属于属主线程的事：回收 `t->snap` 退休链（释放节点/减 blob 引用计数）、改写 `t->live.tree_bytes`、复位 `t->snap.*`。这些与主循环的 COW（`blob->refs++`、退休链挂链）并发 ⇒ 跨线程释放/写共享状态。
- **修法**（保住既有设计：主线程 capture、快照线程 save、期间主线程照常改树）：拆成 `treap_save`（**纯读**，快照线程）+ `treap_save_finish`（**属主线程**，在收到快照线程结果后的完成点回收+复位）；头注释写明不变量：**快照视图对非属主线程只读；capture 与 finish 必须在属主线程**。`kserver.h` 只在快照完成处理处调用 finish（三条分支之前统一调用）。
- **删除多余代码**：`runtime_task_post` 失败路径里的 `treap_save(server->tree,0,0)` 已删——重试任务已被序列化、根本到不了；新任务的视图会被下一次 `treap_capture` 覆盖并把退休链并进新视图，再由新 finish 统一回收 ⇒ 该句只延迟回收。
- **证据**：修复前系统堆检查器报告 `HEAP: Free Heap block 0000000005331AB0 modified at 0000000005331E90 after it was freed`（早于任何自建仪器，与仪器无关）；修复后在两条通道累计 **86 次零复现**（60 轮页堆网络 soak + 10 次串行在程压力 + 16 次 4 路并发压力），且服务端全程存活。
- **状态**：**强证据指向已闭环，但不作证明** —— 该缺陷的复现率随机器负载与堆布局剧烈波动（调整调试分配器的 header 大小即可改变复现率），单次干净运行不构成证据。

### E2 `[撤回]` 本轮的两次错误结论（记录在案，避免复用）
1. **"受害者身份/阶段"**：由**未加锁**的自建分配器给出的"受害块在 `treap.h:338`、偏移 +48 为 `next_free`、阶段 during snapshot save"——该仪器在**多线程**下会搞乱全局登记表，报出**属于其它块的登记项**，并会在隔离区驱逐时对同一指针二次 `free()`（那 9 次 `Invalid address specified to RtlFreeHeap` 即由此而来）。加锁后 10/10 clean，该结论**全部撤回**。
2. **"+992 指向 `base[K_URI_MAX]`"**：`base` 是 `struct k_server` 的字段（活对象），而按 `base_len+32/+40` 追加写的是**局部栈缓冲** `char path[1024]`，与"+992 落在已释放堆块内"不符 ⇒ **撤回**。
- **保留**：系统堆检查器给出的硬数据（块内 **+992** 写入点）仍然有效，但"哪个结构体"目前**无解**。

### E3 `[已撤回：测量假象]` 连接卡死 `pipe: stalled after 5 s (queued=127 done=0 in_flight=0)`
- **现象**：60 轮页堆网络 soak 的 run41-44 连续出现，5 秒内零应答，随后自行恢复；各轮 `alive=1`。
- **结论**：**测量假象，撤回**。那批跑的是**开着 Application Verifier Heaps 的调试构建**，服务端显著变慢，客户端 5 秒"零应答"判卡被跨过。
- **控制实验**：`build/stall_hunt.sh` —— release 构建、**同一负载**（`PIPE 128 4000 SET`）、20 轮，结果 `ok=20 stalls=0 unexpected=0 server_alive=1`，吞吐 22–25k ops/s。
- **装置要求（已落地）**：脚本先 `init` 建盘、启动后轮询 `STATS` 探活（10 秒）、逐轮要求 `ok=<N>` 正信号、既非 ok 也非 stall 的轮次计为 `unexpected` 并中止 —— 否则"0 次卡顿"可能只是"0 条数据"（本轮我先犯过一次这个错：argv 缺参导致服务端只打 usage，20 轮空跑被报成 clean）。

## F. `raft_cluster_fuzz` 在 seed 2 确定性失败（新发现，未修）

### F1 `[已复现]` LEADER COMPLETENESS 违规
- **调用形式**（务必照此，第三参数为持久化延迟）：`./build/raft_cluster_fuzz.exe <start_seed> <count> [persist_delay]`
  - 纠正先前的记录 ✗：审计中"`raft_cluster_fuzz 500` 一致"是**错误用法**的产物 —— 只给一个参数时，`seed=500, count=1`，只跑一个种子，读出来的"绿"没有意义。
- **复现**：`./build/raft_cluster_fuzz.exe 1 40 0` ⇒ seed 1 通过、**seed 2 失败**：
  `FAIL: LEADER COMPLETENESS: leader 2 term 14 has term 14 at committed index 5, expected 8`
  事件序列（日志自带最近 75 条）：`CLUSTER 3 nodes / PARTITION mask=1 / CRASH node 1 / LEADER node 2 term 2 / RECONFIG node 2 -> 3 ids`。
- **与"响应先于落盘"无关** ✗：`persist_delay=0/1/2` 与默认值**都失败**（模式不同：delay=0 是 term/index 不符，delay≥1 是"leader 缺已提交条目"）⇒ 该假设已否证并撤回。
- **作用域**：驱动 `tests/raft_cluster_fuzz.c` **只 include `../code/raft.h`**（不链 treap/cemon/runtime/kserver）⇒ 与本轮（快照视图所有权）改动**无关**；但 `code/raft.h` 的修改时间为 09-18，可能由更早一轮引入 ⇒ **待二分定位**（需与更早副本对比；`D:/KylinDB` 下若有旧版 raft.h 可作参照）。
- **oracle 可信度已核** ✓（这一步决定定性）：模型的"已提交"来自**节点自身的 `commit_index` 推进**（`raft_cluster_fuzz.c:1795-1802`），且该推进按库的契约发生在**条目落盘之后** ⇒ 模型记录的是库自报的提交事实 ✓，非驱动臆造 ✓；同时该检查只在"已提交 term < 新 leader 的 term"时施加（避免乱序假阳性 ✓），完全黑盒（不用 `raft_inspect` ✓）。
- **因此只剩两种解释，且都指向 `raft.h` 缺陷**：
  1. **已提交条目被覆盖** —— 某节点确实提交了 index 5=term 8，而 term 14 的 leader 在 index 5 上是 term 14 ⇒ 违反 §5.4.1、状态机安全性破坏（最严重一类）；
  2. **提交上报失真** —— 库上报了并未真正提交的 `commit_index`（契约缺陷，同样需修）。
- **提交规则已核，未见问题** ✓：主干推进要求**联合配置下 old/new 双多数派均达**（`raft.h:1975-1992`）；follower 只提交本 RPC 已确立的前缀（`raft.h:3610-3620`，§5.4.2）⇒ 规则本身无可指摘，缺陷在交互处。
- **机制已定位（trace 证据链）** ✓：
  1. term 8 时在 **cfg{2,3}** 下 `idx 5 = term 8` 被 node 2、node 3 **双双 APPLY**（2 节点配置 quorum=2 ⇒ 确已提交）；
  2. `RESTART node 2 term 13 lii 4 count 0` —— node 2 从 **index 4 快照**恢复、日志为空；随后以 **term 14** 当选（cfg{1,2,3}，仅得 node 1 一票 —— 而 node 1 日志同样为空），并在 **index 5 写入 term 14** 覆盖已提交条目；
  3. 根源是**配置分叉**：node 3 停在 **{2,3}**，node 1/2 为 **{1,2,3}** ⇒ 两个互不相交的配置各自满足多数派。
- **假设一（已否证 ✗）**："已被移除的 node 1 发起的变更被库本地应用" —— `raft_reconfig` 明确要求 `state==RAFT_LEADER`（`raft.h:2151`）⇒ 从节点发起的变更会被拒 ⇒ **撤回**。
- **更正：一条过度断言** ✗ —— 先前的"`persist_delay=0` 也失败 ⇒ 与'响应先于落盘'无关"**不成立**：该参数只控制落盘**耗时**，不改变"崩溃可落在落盘窗口内"这一事实。
- **烟枪级证据（应用-持久化顺序，§3.8 类）** ✓：trace 中 node 2 先 `APPLY node 2 idx 5 term 8 … commit 5 cfg{2,3}`，随后 `CRASH node 2` → `RESTART node 2 term 13 lii 4 count 0` —— **已应用到 index 5，重启后磁盘只到 index 4 且日志为空** ⇒ 应用状态与持久化状态脱节。
- **更正二** ✗：把"APPLY 到 5 却重启只剩 4"直接读作缺陷**不成立**。`raft_advance_commit` 的耐久门只约束**有 voter 对端的 leader 自身条目**（`raft.h:1966` `require_durable=raft_has_voter_peer(r)`）；**follower 依 `leader_commit` 提交/应用本无需自身先落盘**（`raft.h:3610-3620`），Raft 允许 follower 持久化滞后并靠重新同步追上 ⇒ 单看这条是**合法行为**，非缺陷。
- **真正的分歧点（已收窄）** ✓：库让 **follower 在本地未落盘时就上报 `commit_index`**，而驱动注释假定"**commit_index 只在条目落盘后推进**"（`raft_cluster_fuzz.c:1791-1794`）⇒ 二者**矛盾**。
- **为何这个矛盾会致命（本次失败的链条）** ✓：node 2 应用的**配置条目（C_new）**未进其持久化镜像 ⇒ 重启后按快照配置回到 **{1,2,3}**，而 node 3 停在 **{2,3}** ⇒ 配置分叉；旧配置下的多数派 {1,2}（node 1 日志为空）不含已提交的 index 5/6 却足以当选 ⇒ **覆盖已提交条目**。
- **修法取向**：按 Ongaro §4.3（配置变更需**持久化后**才生效）的精神，更可能该修**库侧**（配置条目/提交上报须在本地持久化确认之后才生效）；另一侧是显式契约化（写清 follower 允许滞后）并相应放宽 oracle —— 二者**不可同时成立**，需拍板后实施。
- **更正三（该轮三次假设全部被代码否证 ✗）**：
  1. "库未实现'落盘后再响应'"——**错**。`deferred_append_response` 会把 follower 的 AE 响应**延迟**到自身落盘确认，且上报的是 `durable_confirm`（仅已持久前缀）：`raft.h:3160-3206`、`:3642`、`:4261-4269` ⇒ §5.3 已实现；
  2. "驱动在向库报告 durable 后崩溃时丢弃数据"——**错**。`persist_copy_flush`（`raft_cluster_fuzz.c:1164-1175`）在调用 `raft_persist_complete` 的同一处立即 `disk_commit_pending` 提升 pending→committed 镜像，崩溃恢复（`:1261`）读的正是该镜像；
  3. "驱动建模把 apply 提前到落盘之前就是根因"——**不充分**：驱动确实会应用尚未落盘的条目（`:1531-1545` vs `:1721` 的 `persist_pending` 门），但库只会 ACK 已持久前缀，故该行为本身不会抬高任何多数派。
- **剩余矛盾（未解）** ✓：node 2 在 `APPLY idx 5`（cfg{2,3}、quorum=2）之后崩溃，重启只剩 lii=4 ⇒ 说明 **index 5 在 node 2 上既"已提交可应用"又没有进它的已提交镜像**；而按库的延迟 ACK 机制，node 3 不可能仅凭 node 2 的非持久 ACK 提交 index 5 ⇒ 二者不能同时成立，必有一处是我对 harness 的读法有误。
- **下一步（定向取数，非猜测）**：在 harness 的 persist/apply/commit 路径上为 **index 5** 加自标识打印（`ev("P5 ...")`：persist 请求、durable 上报值、镜像提升、ACK 发出），重跑 `1 40 0` ⇒ 以时间线判定矛盾落在哪一侧。
- **定向取数（`P5` 自标识打印，已加在 `tests/raft_cluster_fuzz.c`）** ✓：时间线显示 node 2 与 node 3 **都**上报过 `durable 5`、`durable 6`，node 2 更上报到 `durable 7`；而 **崩溃瞬间 node 2 的两份镜像都是 `disk_lii=4 count=0` / `pending_lii=4 pending_count=0`**（`P5 ... CRASH-TIME`）。⇒ **驱动向库上报了它并未写入任何镜像的前缀**（库的契约要求只上报确实持久化的索引）⇒ 库据此 ACK，leader 据此形成 {2,3} 多数派 ⇒ 崩溃后条目消失、配置回退 ⇒ LEADER COMPLETENESS 违规。**判定：缺陷在测试驱动的持久化模型，不在 `raft.h`**。
- **已对驱动做的两处修正**（`durable_index`/`inflight_durable` 由"活日志尖端 `live_last`"改为"该次 persist 视图的 `last_included_index + log_entry_count`"）**未能改变时间线** ✗ ⇒ persist 视图**本身**已含到 7 的条目，说明条目是在 `disk_persist`/`persist_copy_capture`/`disk_store_log` 交互中**没被写进任何镜像** ⇒ 矛盾已压到该封装内部，需下一轮查透（`disk_persist` 的 `snapshot_dirty` 分支只写 pending 镜像、`disk_commit_pending` 只提升快照，是首要嫌疑）。
- **`P5` 插桩属临时探针**：`tests/raft_cluster_fuzz.c` 中的 `P5` 打印在该驱动缺陷修好前保留以便复现，**修好后必须删除**（与"插桩必须回退"一致）。
- **更正四（"上报多于所存"也被否证）** ✗：`durable_index` 在正常分支取的是**该次 persist 视图自身的尖端**（`live_last`，由 `p->last_included_index`/`p->log_entries[]` 得出），与 `disk_persist` 写入的内容一致 ⇒ 不存在"上报超前"。
- **更正五（我截断了证据）** ✗：先前只看 `P5D` 前 26 行就下结论，违反"审计结论不得截断证据清单"。全量 78 行显示：node 2 的所有 persist 视图 `log_entry_count ≤ 1`，崩溃前镜像是 `disk_lii=4 count=0`。
- **首要嫌疑（待验证，未定为结论）** ✓：驱动的镜像**按每次 persist 视图整体重写** —— 一个 `log_entry_count=0` 的心跳视图会把**已持久的日志清零**（`disk_store_log(n,p,0)` 直接把 `disk_log_count` 置为该视图的条目数）。真实应用的持久日志必须是**只增不减的前缀**，绝不能被后续视图截断；若成立，则"已上报持久、随后被心跳抹掉、崩溃后丢失"这一链条完整解释了本次违规。
- **验证方法（下一步）**：在 `disk_store_log(n,p,0)` 处打印 `disk_log_count` 的变化序列（`P5D` 已含 OUT 行）⇒ 若出现「先 count=N(>0)，后 count=0 且其间无日志截断事件」，即坐实该嫌疑；随后把镜像写入改为**前缀合并**（保留既有条目）并复跑 `1 40 0`。
- **更正六（"驱动缺陷"判定降级）** ✗：据"镜像被心跳清零"这一观察对驱动做了三处修正（`durable_index`/`inflight_durable` 改为按视图计算、非快照路径改为 `disk_merge_log` 合并），**三次修正对结果均无影响**（seed 2 依旧失败）⇒ 该观察虽是事实，却不是本次违规的成因 ⇒ **三处改动全部回退**，驱动语义恢复原样；"驱动缺陷"的定性**证据不足，降级为未定性**。
- **可复核的事实清单（不含结论）**：
  1. `raft_cluster_fuzz 1 40 0` 在 seed 2 确定性失败（LEADER COMPLETENESS，leader 2 term 14 在已提交 index 5 上是 term 14，期望 8）；
  2. 该驱动**只链 `raft.h`**，与本轮快照改动无关；
  3. 库侧四处机制经代码核对**自洽**：提交推进（联合配置双多数派 + leader 自身耐久门）、follower 提交（§5.4.2 前缀上限）、落盘后再响应（`deferred_append_response` + `durable_confirm`）、恢复读已提交镜像；
  4. 驱动的镜像确会**在心跳视图下把 `disk_log_count` 从 1 归 0**（`P5D` 序列 1,0,1,0,1,0 可见），且该行为**未能修复本次违规**（见更正六）；
  5. 崩溃前 node 2 的镜像为 `disk_lii=4 count=0`，而其 `durable_confirm` 已推进到 7。
- **更正七（"合并镜像"修正亦无效）** ✗：驱动中**本就有**正确的原语 `disk_merge_log`（按绝对索引去重合并，`:926`），其注释所述与本次观察完全一致（"否则已应用的条目会在重启时静默丢失"），但它**只在 `snapshot_dirty` 分支**被调用。把它也用于非快照分支后，**违规依旧、崩溃点镜像依旧 `lii=4 count=0`** ⇒ 该编辑不影响结果，**已回退**（驱动语义复原，基线可复现）。
- **已证实的观察（非结论）** ✓：persist 视图是**增量**——交错时间线显示某次视图为 `first=6 last=6`（边界 lii=4，无 index 5），驱动却把镜像整体重写成该视图，随后取视图尖端作为 durable 上报 ⇒ **上报的持久位与镜像实际持有不一致**。这是驱动建模的事实，但（见更正七）**不是**本次违规的成因。
- **更正八（第四处驱动侧修正亦无效）** ✗：又在"向库上报持久化完成后立即尝试提升 pending 快照"处加了提升（理由：被压缩进快照的**配置条目**只存在于未提升的 pending 镜像，而 pending 在崩溃时被丢弃 ⇒ 恢复旧配置）。**违规依旧、崩溃点镜像依旧** ⇒ 已回退。至此**四处对症修正全部对结果无影响**。
- **方法论更正（重要）**：我从**失败点往回推**做了四处修正，属方向性错误——报告出来的失败是链条**末端**，真正的成因在其**之前的第一处分歧**；凡在上游之外做的修补都是空转（特征：数个各自论证无误的修正，pass/fail 一动不动）。**下一轮必须改方法**：先在事件流里定位**最早的一处"两个独立记录对同一状态不一致"**（如 harness 模型 vs 组件上报、镜像 vs 上报的 durable、applied vs durable），再**针对那个事件**二分；不能证明"改变了彼处事件流"的修补等于没测。
- **更正九（定性达成：第一处分歧已定位，成因＝驱动的 `disk_merge_log` 无条件截断）** ✓✓
  - **定位**：新增 T2 探针（每次推进打印 node 2 的"库状态 / 镜像 / 成员"三线）。**第一处分歧**出现在重配置那一步：库 `L=(3,3)→(3,2)→(2,2)`（联合→收尾），而镜像 `I` 始终 `(3,3)`。
  - **成因**（逐行走查）：`disk_merge_log` 末尾**无条件**把镜像日志截断到"本次 persist 视图的边界"（`:951-980`，为 seed 70380 的"同一次 snapshot 内截断"而写）。该函数后来被**普通/心跳视图**复用，而这类视图是**增量**（心跳视图 `log_entry_count==0`）⇒ 截断把镜像里已写入的**配置条目**丢回边界 ⇒ 崩溃恢复**旧成员** ⇒ 节点可在排除"持有已提交日志的节点"的情况下当选 ⇒ 覆盖已提交条目。
  - **验证**：截断改为**仅对尾部有权威性的视图**（snapshot_dirty 全量尾部）生效，普通/心跳视图**只合并不截断** ⇒ **seed 2 的 LEADER COMPLETENESS 违规消失**（`1 40 0`、`1 200 0`、默认延迟三档均不再报），T2 中该第一处分歧随之消失 ⇒ **判定：测试驱动的持久化镜像语义缺陷，库侧 exonerate**。
  - **教训**：期间有一次"修正无效"的判断**建立在编译失败后的旧二进制上** ⇒ 必须先确认构建成功、再解读运行结果（脚本里 grep 到 error 却继续跑，等于白跑）。
- **新暴露的独立失败也已归因（驱动侧）** ✓：修复截断后改报 `FAIL: LIVENESS: no stable leader elected after heal (leader changes=0, term 0 -> 0)`。加入 `RESTORE-REFUSED` 诊断后实测：尾段重启时驱动的镜像**非法**——`img_lii=1 img_cnt=1 first=3`（边界 1、首条目 3，**缺 index 2**）、`img_lii=1 img_cnt=35 first=3 last=37`。库的 `raft_create` **正确地拒绝**这种带空洞的日志 ⇒ 节点保持死亡 ⇒ {1,2,3} 配置下只剩 node 1 ⇒ 无多数派 ⇒ 活性断言必然触发。
- **鉴定结论（F 收口）** ✓：本 fuzz 的**全部**可复现失败均可归因于**驱动的持久镜像模型**，库侧在所有可核对点上自洽。三项独立证据：① 第一处分歧（T2 三线）＝ `disk_merge_log` 对增量/心跳视图也"截断到视图边界"（`:951-980`）⇒ 已持久化的配置条目被丢弃 ⇒ 恢复旧成员 ⇒ 覆盖已提交条目；② 镜像出现空洞（`RESTORE-REFUSED` 实测）⇒ 恢复被库正确拒绝 ⇒ 尾段无多数派；③ 上报的 durable 位取增量视图尖端，而镜像并不持有该前缀（T2：`d=6/7` 而 `img_cnt` 在 0/1 振荡）。
- **已验证的修法与效果（均**已回退**，供下一轮整体重写参考）**：
  ① 截断仅对"尾部权威视图"（snapshot_dirty 全量尾部）生效 ⇒ **seed 2 的 LEADER COMPLETENESS 消失**，但暴露 ②；
  ② 镜像维护"自边界起的连续前缀" ⇒ **LIVENESS 消失**，但改为更早的 `LEADER COMPLETENESS: leader 2 term 7 … index 2, expected 6`。
  ⇒ **逐个打补丁只会让失败在几种违规之间搬家** ⇒ 处置：**不再补丁式修驱动，需重写其持久模型**（单调、连续、与上报一致的 durable 日志）。
- **当前状态**：驱动语义**已复原**（基线与最初逐字一致 ✓）；仅保留 **13 处诊断探针**（`T2`/`P5`/`P5D`/`P5E`/`RESTORE-REFUSED`），定性后删除。诊断探针 `P5`/`P5D`（`tests/raft_cluster_fuzz.c`，共 7 处）**为复现保留**，定性后必须删除。
- **性质**：库自称支持安全重配置（延迟重配置 + 追平后建联合配置，`raft.h:665/2054`），且 joint→final 的丢弃在条目 apply（=已提交）时进行、逻辑合规 ⇒ 因此**配置未收敛属库侧缺陷**，非驱动越界。**严重度高**（可致已提交条目被覆盖）。
- **严重度**：高（若成立，是已提交日志的一致性/完整性违规）。
### F2 驱动持久模型重写要点（下一轮可直接实施，非补丁）

**要维持的不变量**（现状三项都被违反，逐条已实测）：
1. 镜像日志是**自 `disk_lii` 起的连续前缀**（首条目必须是 `disk_lii+1`，不得有空洞）；
2. 镜像内容必须**覆盖**已向库上报 durable 的前缀（上报值 ≤ 镜像连续前缀尖端）；
3. 镜像**不得**因"视图未携带某条目"而丢弃它——**普通 persist 视图是增量**（可为空＝心跳），只有**尾部权威视图**（`snapshot_dirty` 携带完整尾部）才可触发截断；库日志回退（冲突覆写）的证据只能来自"视图在该 index 处携带了**不同 term** 的条目"。

**收口方式**：新增 `disk_image_normalize(node *n)`，在所有改动镜像的路径末尾调用（`disk_persist` 两个分支、`disk_commit_pending` 的快照提升与压缩、install 接收、restore 之前），它统一执行 (a) 丢弃 `index <= disk_lii` 的条目、(b) 在第一个空洞处截断、(c) 断言首条目 == `disk_lii+1` 且 term/kind 序列自洽。

**上报规则**：`durable_index` 用**归一化之后**的镜像前缀尖端（不是活日志尖端、也不是视图尖端）。

**验证门**：T2 三线在**每次推进**上都不再出现两类分歧（库配置 ≠ 镜像配置、上报值 > 镜像尖端）；随后 `raft_cluster_fuzz 1 2000 0`、默认延迟档、`g_clean` 尾段三档须全绿。
### F3 实施结果（F2 重写已落地）与新目标

**已落地（`tests/raft_cluster_fuzz.c`）**：
- `disk_image_normalize()` 单一收口（镜像恒为"自 `disk_lii` 起的连续前缀"）✓，在 `disk_commit_pending` 末尾、`disk_persist` 末尾、restore 之前调用 ✓；
- 截断改为**仅由冲突证据触发**（同一 index 出现**不同 term** ⇒ 库确实覆写了该后缀）✓，不再因"视图未携带某条目"而截断 ✓；
- `durable_index`/`inflight_durable` 改报**归一化后镜像前缀的尖端**（延迟路径在 flush 落盘时计算）✓；
- 每 seed 重置 `fuzz_alloc_calls`/`fuzz_oom_at`（OOM 触发点是**绝对**分配序号 ⇒ 不重置会导致"同一 seed 随调用规模变化"）✓；
- 事件流新增 `SEED n` 标记 ✓（否则失败种子只能靠二分猜）✓。

**效果**：`raft_cluster_fuzz 1 2000 0` 中 **seeds 1..689 全部通过** ✓；此前必败的 seed 2、以及 LIVENESS 类失败均消失 ✓ ⇒ **F 此前的全部违规已被消除** ✓（与"驱动侧缺陷"的定性一致 ✓）。

**新目标（未定性，确定性复现）**：`seed 690` ⇒
`FAIL: LOG MATCHING: index 4 applied with term 10 but term 9 was applied elsewhere (node 2)`
现场：node 1 快照边界 `LII=1/LIT=2`，node 2/3 为 `LII=3/LIT=8`；末尾事件 `APPLY node 1 idx 2 term 6 / idx 3 term 8`、`APPLY node 2 idx 4 term 10`。属性＝**状态机安全性**（同 index 不同 term 被应用），疑与**快照/安装**路径相关。复现命令：`./build/raft_cluster_fuzz.exe 690 1 0`。
下一轮方法：把 `T2` 三线探针从"仅 node 2"扩展到**全部节点**（或改为紧凑单行），先定位**第一处分歧**再二分。
### F4 `[已修]` selftest 在工作目录残留 `kdb-selftest-*.snap.*`

- **现象**：仓库根目录累积 30 个 `kdb-selftest-<pid>-<us>-n<id>.snap.<index>`（每跑一次 selftest 多 2–3 个）。
- **根因**：两处清理（单节点 `k_selftest_node_cleanup`、三节点 `k_embedded_clean`）只按"存储元数据 / 活动 server 字段里记录的快照索引"逐个 `vfs_unlink`。而 ①有些快照的索引**不在任何元数据里**（实测 index 97/64）⇒ 无从发现；②部分清理调用点在 `runtime_stop` **之前** ⇒ 句柄仍开、Windows 下 unlink **静默失败**（返回值被忽略）。
- **修法**：新增共享助手 `k_selftest_snap_sweep(base)` —— 对 `0..K_SELFTEST_SNAP_SWEEP(4096)` 做有界索引扫描，**边 unlink 边统计失败并重试**（最多 8 次 × 20ms，用于兜住"刚停的服务端还持句柄"的窗口）；两处清理都在最后调用它；另外把 bootstrap 路径的快照索引改为**释放前捕获、释放后 unlink**。
- **验证**：build **0 error / 0 warning**；`selftest` 连跑 3 次全 PASS；运行后根目录残留 **0**（历史 30 个已清除）。
### F5 seed 690 的下一步（定点取数，窗口已不够用）

**已知事实**（`./build/raft_cluster_fuzz.exe 690 1 0`，事件窗口 1024 条）：
- 可见窗口内 `index 4` 只被应用过一次：`APPLY node 2 idx 4 term 10`（commit 5, cfg{1,2,3}）；
- 而 oracle 的"首个应用 term"是 9 ⇒ 那次应用发生在**更早、已被窗口截掉**的地方 ⇒ 现有 1024 条事件缓冲不足以看全一个 seed；
- 收尾时各节点三线：`T2 n1 23 t13 c3 a3 d5 L33 l1 i1 5 I33`、`T2 n2 23 t13 c5 a3 d5 L33 l3 i3 2 I33`、`T2 n3 22 t13 c3 a3 d5 L33 l3 i3 2 I33`
  ⇒ node 1 的快照边界 1（持 5 条：index 2..6），node 2/3 的边界 3（各持 2 条）；三者配置一致（{1,2,3}）；
- 期间事件：`RESTART node 1 term 12 lii 1 count 5`、`RESTART node 3 term 10 lii 3 count 2`、`CRASH node 2/3`（其 CRASH-TIME 镜像 `disk_lii=3 count=1/2`）等。

**下一步（两条，都很便宜）**：
1. 事件缓冲 `EV_MAX`（当前 1024）按 seed 需要放宽（或改为只在 fail 时把环形缓冲整体落盘）；
2. 加**定点打印**：只对 `index == 4`（可扩展到 3..6）的每次 `check_apply`、以及各节点每次"给 index 4 写入 term"的日志变更打印一行 ⇒ 直接看出**谁先写成 term 9、谁后写成 term 10**、以及中间是否有截断/安装。
### F6 `[已闭环]` F 线索收口：三处根因全部定位并修复，`1 2000` 全绿

**根因（都在驱动侧，逐条有证据）**：
1. **镜像截断判据错误（两代）**：最初"视图未携带即截断"⇒ 心跳视图把已持久化的配置条目清零 ⇒ 恢复旧成员 ⇒ 覆盖已提交条目（LEADER COMPLETENESS，seed 2）；改为"同 index 不同 term 即截断"⇒ 旧镜像条目顶掉已提交的新条目 ⇒ 已提交的 index 4（term 9）被写成 term 10（seed 690 LOG MATCHING）。**最终判据**：以**库自己的日志尖端**（`n->r->log.last_included_index + n->r->log.count`）为唯一权威 —— 库不再持有的条目才丢，库还持有的必须留，合并按 index 覆盖 term。
2. **黑盒日志尖端取自 persist 视图**：增量/心跳视图的尖端只是快照边界 ⇒ 重启后的新 leader 被算成 "log tip 12"（实际持有 13/14）⇒ 误报 leader completeness（seed 914）。**修法**：`last_index` 一律由**库日志**推导。
3. **镜像不变量**（前一阶段）：`disk_image_normalize()` 单一收口 + `durable_index` 只报归一化后的前缀尖端 + 每 seed 重置 OOM 触发基 + 事件流加 `SEED n`。

**验证**：`raft_cluster_fuzz 914 1 0` / `690 1 0` / `1 40 0` / `1 200 0` / `1 2000 0` / `1 2000`（默认延迟）**全部 `done`** ✓ ⇒ 此前 40 轮就必败的两类违规（LEADER COMPLETENESS、LIVENESS）与 2000 轮内必败的 LOG MATCHING 均已消除 ✓。

**清洁度**：临时探针（`T2`/`P5`/`P5D`/`P5E`/`LOGCHG`/`RESTORE-REFUSED`，共 15 处）**已全部移除** ✓；保留的是**修复本身**与有价值的诊断（`SEED` 标记 ✓、`--apply-stress` 压力模式 ✓）。
