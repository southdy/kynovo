# 早期调查档案（tools/archive/）

本文件汇总 `tools/archive/` 下 **16 个一次性调查脚本**：当时要回答的问题、**当时的结论**（引证其输出记录
`doc/measurements/<name>.out`）、以及它对当前工程的影响。这些脚本是**历史证据**，不是日常门禁——
日常回归请用 `doc/testing.md` 里的分层门与 `tools/harness/` 下的 harness。

> 记录诚实的部分：若干 `.out` 是**问题尚未修复时**的快照（输出为空或状态异常），已逐条标注为"记录不完整"。

## 集群与协议

| 脚本 | 目的 | 当时的结论 | 影响 |
|---|---|---|---|
| `cl3.sh` | 同机 3 进程集群（真实对端协议走环回） | 选主成功（leader 8101），多节点提交 **30,003 ops/s @K=32**，p50 871µs | 首个可信的多节点基线 |
| `cl4.sh` | 3 节点（8111–8113）提交吞吐基线 | leader 由日志判定 = 8111；建立"leader 必须从状态里解出、不能猜"的做法 | 后续所有脚本沿用该约定 |
| `cl12.sh` | 为何三个端口都报 `state=3`？区分"真并发的多主"与"三个独立单节点集群" | 三者都是 `id=1, term=1`、commit 各自前进（2/5/8 → 12/15/18）⇒ **是三个独立单节点**（peer 链接未建立） | 定位到**集群 spec/参数**问题；促成后来 `cl29`/`diag3` 对启动参数的排查 |
| `cl29.sh` | 一次性 `kdbctl` 能否对 **3 节点**集群工作（此前只测过单节点） | 记录不完整（当时端口/leader 解析尚未稳定，`.out` 输出为空） | 促成 `tests/cli_smoke.sh`（现为 L2 门）覆盖单节点语义 |
| `diag3.sh` | 3 节点启动参数诊断（SPEC 格式与端口占用） | 记录为当时的启动日志 | 与 `cl12` 一起把"参数误用"与"库缺陷"区分开 |

## 延迟归因（性能主线）

| 脚本 | 目的 | 当时的结论 | 影响 |
|---|---|---|---|
| `cl5.sh` | 3 节点、K=8、200 个 37 字节值 | **977 ops/s**，p50 6.3ms；回读验证通过 | 确立了"早期慢"的基线数字 |
| `cl6.sh` | 同一集群同一形状的 **cold vs warm** 四阶段 | cold 690 / warm 737–761 ops/s ⇒ **冷热无显著差异** | 排除"首次写入慢"的解释，把矛头指向**合批/fsync 策略** |
| `cl7.sh` | ~8ms/请求的延迟在哪里？单节点 K 扫描（完全无复制） | K=8 → 953；K=32 → 13,191；K=128 → 21,290；K=256 → 32,673 ops/s ⇒ **延迟在"每请求的 flush 窗口"，不在复制** | 直接促成自适应 flush 窗口（EWMA 由实测 sync 成本推导）与深流水优化 |
| `cl8.sh` | **是否在 leader fsync 之前就 ack 了客户端？** 逐次采样 leader 计数 | 每次都是 `ok(waited for a fsync)` ⇒ **ack 确实等 fsync** | 排除"提前 ack"这一最严重的可能误解 |
| `cl9.sh` | 写往 follower 的快路径：leader 是否真的 fsync+提交了该突发 | follower 侧 **91,776 ops/s @K=32**（p50 287µs），leader 计数同步前进 ⇒ **是合法的中继/流水提交** | 确认高吞吐不是"漏提交" |
| `cl10.sh` | 写 follower 的真实语义 + 从 follower 读 | 写被拒并附 leader 提示（`RESP|` 空体）；从 follower 读 50 次全 `empty` | 对应论文 §6.2：follower 不承接读写 |

## 客户端与 CLI

| 脚本 | 目的 | 当时的结论 | 影响 |
|---|---|---|---|
| `cl11.sh` | 把探针指向 follower，检查是否被告知 leader 地址并重连（§6.2 推荐做法） | 记录不完整（该次运行 `sync_call_failed`，状态解析未稳定） | 该行为后来由 `cl10` 的空 `RESP|` 体 + `kclient` 重定向逻辑覆盖 |
| `cl13.sh` | 验证"无 TTY 的一次性 `kdbctl`"与 `bench_rate` 的 ok/err 标注 | 记录显示当时 CLI 输出为空、`PROBE_VALID|yes` 标注可用 | 促成 `kdbctl` 无控制台降级与本轮 F 系列对 `PROBE_VALID` 的坚持 |
| `cl14.sh` | 服务器是否可启动 + 带插桩的一次性 CLI（对照管道 REPL） | `CL14_DONE` / `EXIT=0`（当时纯管道 REPL 无输出） | 与 `cl13` 一起定位 CLI 交互模式问题 |
| `cl15.sh` | 修复后：`kdbctl` 必须**在没有控制台**时可用（输出重定向到文件） | 记录为空（该次运行未捕获输出） | 后续由 `tests/cli_smoke.sh` 固化为门禁 |
| `repro_deep.sh` | 复现 K=512 深流水突发并抓取：CLI 报告、服务端日志、存活、退出状态 | 复现了"服务端在深流水下死亡/无应答" | 直接导向崩溃①②的定位与修复（见 `doc/gaps-audit.md`） |

## 与当前门禁的对应关系

- 这些脚本关心的问题，现在都有**常规门**覆盖：
  集群/协议 → `raft_cluster_fuzz`、`kserver_cluster_fuzz`；CLI → `tests/cli_smoke.sh`；
  深流水/存活 → `tools/harness/soak_release.sh`、`stall_hunt.sh`、`watch_counters.sh`；
  性能归因 → `tools/harness/perf_matrix.sh`、`burst.sh`、`pipe_frontier.sh`。
- 因此它们**不必**进入日常回归；需要复现历史现象时再单独运行（脚本已改为按自身位置推导仓库根，可在任意目录调用）。
