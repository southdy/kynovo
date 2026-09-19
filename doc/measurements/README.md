# 过程测量归档（doc/measurements）

本目录是**原始过程产物**，不是构建输出、也**不可字节级复现**（机器状态、其他负载会变）。
它们曾支撑过吞吐/延时取舍与调参决策，因此被保留；`build/` 只放产物，历史数据在这里。

## 内容
- `*.txt`（111 个）：每次基准运行的服务器计数器与阶段结果。
  命名规则为**源目录展平**，例如 `bench-run3_k1.txt` = 原 `build/bench-run3/k1.txt`。
- `*.out` / `*.err`：早期调查脚本（现位于 `tools/archive/`）的**结论输出**。

## 一份记录怎么读
```
STATS_BEFORE|id=… state=… commit=… wal_records=… flush_by_target=… rounds=…
PHASE|n=<请求数> k=<在飞深度> wall_us=<总耗时> ops_per_s=<吞吐>
LAT|n=… min=… p50=… p90=… p99=… max=…         (仅有该行时才有分位数据)
STATS_AFTER|… flush_by_target/flush_by_drain/flush_by_window/flush_by_bytes=…
```
- `flush_by_*` 的四类计数说明**合批是被哪一条件触发**的（达目标条数 / drain / 时间窗 / 字节数），是判断"瓶颈在轮次还是在 fsync"的关键；
- `rounds` 是事件循环轮次；`wal_records` 是 WAL 记录数（≈ 每次 fsync 一条）。

## 头部数字（103/111 份含 PHASE 的记录）
**各在飞深度 K 的最优吞吐**（跨全部记录取最大）：
| K | 最优 ops/s |
|---|---|
| 1 | 3,113 |
| 8 | 73,778 |
| 32 | 91,776 |
| 64 | 14,090 |
| 128 | 83,333 |
| 256 | 95,328 |
| 512 | 53,079 |
| 2000 | 62,295 |

**载荷与合批的影响**（同期记录）：1B 值、K=1 时约 3.1k ops/s；4KiB 值、K=32 时约 1.1k ops/s；
512KiB 值、K=1 时降到约 80 ops/s ⇒ **大值 + 浅流水是最差组合**（值拷贝与 fsync 双重代价）。
**延时**：69 份记录含分位数据；典型 K=8 时 p50 86µs、K=256 时 p50 2.1ms（深流水换吞吐、牺牲单请求延时）。
**已确认的边界**：`cl8` 的逐步采样结论是 **ack 等待 leader fsync**（"ok(waited for a fsync)"，无提前 ack）；
`cl9` 显示写往 follower 的突发由 leader 统一 fsync+提交（91,776 ops/s @K=32，合法中继路径）。

## 使用建议
1. 先查这里有没有**同形状**的记录（同 K、同载荷、同 mem/disk、同节点数），再决定是否重测；
2. 重测时用 `tools/harness/` 里对应的 harness，并把新结果写回本目录（保持同一命名规则）；
3. 比较时**必须**记录二进制是否同版本、以及当时机器是否有其他负载——否则数字不可比。
