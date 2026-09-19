# Process measurement archive (doc/measurements)

This directory holds **raw process artifacts**: not build output, and **not reproducible byte for byte** (machine
state and other load vary). They once supported throughput/latency trade-offs and tuning decisions, so they were
kept; `build/` holds artifacts only, historical data lives here.


## Platform: these Windows numbers are not Linux numbers

Everything below was measured on the Windows host.  The Linux records added beside them
(`linux-latency-disk.txt`, `linux-latency-mem.txt`) come from a VMware guest on the SAME physical
machine, so they are a platform comparison with a virtual disk in the path, not a bare-metal Linux
baseline - an empty sync measures 1 us there against 78 us here.  Never mix the two sets in one table
without saying which platform each row came from.

## Contents
- `*.txt` (111 of them): the server counters and phase results of each benchmark run.
  The naming rule is **flattened source directory**, e.g. `bench-run3_k1.txt` = what used to be `build/bench-run3/k1.txt`.
- `*.out` / `*.err`: the **conclusion output** of the early investigation scripts (now under `tools/archive/`).

## How to read a record
```
STATS_BEFORE|id=… state=… commit=… wal_records=… flush_by_target=… rounds=…
PHASE|n=<request count> k=<in-flight depth> wall_us=<total wall time> ops_per_s=<throughput>
LAT|n=… min=… p50=… p90=… p99=… max=…         (percentiles are available only when this line is present)
STATS_AFTER|… flush_by_target/flush_by_drain/flush_by_window/flush_by_bytes=…
```
- The four `flush_by_*` counters say **which condition triggered the batching** (target record count / drain / time
  window / byte count); they are the key to telling "whether the bottleneck is the rounds or the fsync";
- `rounds` is the event-loop round count; `wal_records` is the WAL record count (≈ one per fsync).

## Headline numbers (103/111 of the records that contain a PHASE line)
**Best throughput at each in-flight depth K** (maximum taken across all records):
| K | best ops/s |
|---|---|
| 1 | 3,113 |
| 8 | 73,778 |
| 32 | 91,776 |
| 64 | 14,090 |
| 128 | 83,333 |
| 256 | 95,328 |
| 512 | 53,079 |
| 2000 | 62,295 |

**Effect of payload and batching** (records from the same period): 1B values at K=1 ≈ 3.1k ops/s; 4KiB values at
K=32 ≈ 1.1k ops/s; 512KiB values at K=1 drop to ≈ 80 ops/s ⇒ **large values + shallow pipelining is the worst
combination** (the double cost of the value copy and the fsync).
**Latency**: 69 records contain percentile data; typical p50 is 86µs at K=8 and 2.1ms at K=256 (deep pipelining trades
single-request latency for throughput).
**Confirmed boundaries**: the step-by-step sampling in `cl8` concludes that **the ack waits for the leader fsync**
("ok(waited for a fsync)", no premature ack); `cl9` shows a burst written to a follower is fsynced+committed by the
leader as a whole (91,776 ops/s @K=32, a legitimate relay path).

## How to use this
1. First check whether a **same-shape** record already exists here (same K, same payload, same mem/disk, same node
   count) before deciding to re-measure;
2. when re-measuring, use the corresponding harness under `tools/harness/` and write the new results back into this
   directory (keeping the same naming rule);
3. when comparing you **must** record whether the binary is the same version and whether the machine carried other
   load at the time — otherwise the numbers are not comparable.
