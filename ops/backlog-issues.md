# Backlog, posted as GitHub issues

Posted by `tools/ops/post-backlog-as-issues.py` (tag `ops/post-backlog-as-issues`).  Titles are the
dedupe key: re-posting skips issues that already exist, so this file can be edited and re-tagged.
The card ids in each footer point back at the Hermes kanban board `kynovo`.

## G1 定性：CI gcc 16.2 对 tests/raft_test.c 报 17 处 ready.* may be used uninitialized

证据：ci-logs 分支的 build.log（gcc 16.2.0 报，钉版 15.2.0 一处不报）。两种可能：(a) -O2 优化器误报；(b) raft_advance(...,&out) 未写满 ready 所有字段——按 ready 必须完整上报的既定契约则属 raft.h 缺口。验收：同一处在 -O0/-O2 × 15.2.0/16.2.0 四组合下编译对比，给出结论并写进 doc/gaps-audit.md G1；若是 (b) 则修 raft.h。

<sub>backlog card `t_18a8c63b` (Hermes kanban)</sub>

## C4 启动失败原因不可见

kserver.h:4594-4600,4602,4642,4682 与 kdbsvr.c:300：cfg/WAL meta/分配/线程失败都只输出 failed to start server N。用户明确要求 fatal 必须打印原因。验收：每条失败路径打印具体原因，并写一个能失败的用例证明（例如故意占用端口或损坏 meta）。

<sub>backlog card `t_469a3143` (Hermes kanban)</sub>

## C7 INFO/STATS 无界 sprintf 写 char text[2048]

kserver.h:3254,3258,3260,3262：新增字段即越界。验收：改为有界写入（记录剩余空间或截断），并证明新增字段不会越界。

<sub>backlog card `t_315a3c62` (Hermes kanban)</sub>

## C11 cemon_ingress_close 无界自旋；cemon_destroy 排空 2000ms 上限后可能泄漏 socket

code/cemon.h:1047-1066, 2900-2910。验收：自旋有界（有退出条件与可见告警），destroy 后不遗留 hold 的 socket；用可复现的慢对端场景证明。

<sub>backlog card `t_f9bf6d96` (Hermes kanban)</sub>

## C12 UDP 软错误静默吞掉；UDP 兜底分支用错缓冲

code/cemon.h:2189-2194, 2207-2213（应读 udp_recv->buf 而非 recv->buf）。验收：软错误有计数或日志，缓冲取对；给出可触发的证据，或明确该分支未被使用并记录理由。

<sub>backlog card `t_ed19113c` (Hermes kanban)</sub>

## C13 循环级致命错误只有 -1，无取回错误原因的接口

code/cemon.h:3170, 3269-3275。验收：提供取回最后错误（含 Winsock 码或 errno）的接口并在 fatal 路径打印；与 C4 的 fatal 必须可见一致。

<sub>backlog card `t_933e252c` (Hermes kanban)</sub>

## 非 Windows 分支从未编译（含 C9/C10 的 Unix 语义）

C9: code/cemon.h:2452-2461,3432-3435（一次武装可投递多次 CEMON_DATA；callback_depth==0 同步重入）；C10: 2455,2395,2342（Unix recv/accept/flush 不消耗派发预算）。这些分支在本机从未被编译过。验收：先用最小桩让 Unix 分支能被编译（或明确记录无法验证的理由），再逐条处理 C9/C10；不得在没有编译证据时说已修。

<sub>backlog card `t_082e24b2` (Hermes kanban)</sub>

## R8 被拒绝的 AE 携带尚未持久化的新任期对外传播

code/raft.h:1277-1278 → 3660-3671：投票/AE 成功路径都有落盘闸门，此处没有。验收：先对齐 doc/dissertation.md 的对应段落并说明依据，再决定改法；给出两处独立证据（单元 + 集群 fuzz）。

<sub>backlog card `t_07585855` (Hermes kanban)</sub>

## C6 层边界：应用层直接读 raft->config_new/config_joint/config_learners 做策略判定

code/kserver.h:4411-4413。属机制层/策略层边界问题（边界在哪层责任就在哪层）。验收：改成由 raft.h 上报所需信息（ready 或查询接口），应用层不再直接读内部字段。

<sub>backlog card `t_972b67e8` (Hermes kanban)</sub>

## C3 WAL 段首记录的 generation 连续性不检查

code/kserver.h:1465 vs 1478（prev_gen=0 每段重置）。验收：定义并检查跨段 generation 连续性（或在文档里论证为何不需要），给出损坏段的取证方式。

<sub>backlog card `t_39285829` (Hermes kanban)</sub>

## lincheck 未进入任何构建目标

tests/lincheck.py 与 tests/lincheck.h 存在但 build.sh 无目标，线性一致性检查实际上从未在回归中运行。验收：加构建或运行目标并纳入合适的门（或明确记录为何不纳入），跑出一次真实判定行。

<sub>backlog card `t_9f03e69c` (Hermes kanban)</sub>
