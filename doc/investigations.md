# Early investigation archive (tools/archive/)

This file collects the **16 one-off investigation scripts** under `tools/archive/`: the question each was meant to
answer, **the conclusion at the time** (citing its output record `doc/measurements/<name>.out`), and what influence it
has on the current project. These scripts are **historical evidence**, not day-to-day gates —
for routine regression use the tiered gates in `doc/testing.md` and the harnesses under `tools/harness/`.

> A note in the interest of honesty: several `.out` files are snapshots from **before the problem was fixed** (empty output or abnormal state) and are marked one by one as "incomplete record".

## Cluster and protocol

| Script | Purpose | Conclusion at the time | Impact |
|---|---|---|---|
| `cl3.sh` | 3-process cluster on one machine (the real peer protocol over loopback) | leader election succeeded (leader 8101), multi-node commit **30,003 ops/s @K=32**, p50 871µs | the first trustworthy multi-node baseline |
| `cl4.sh` | commit-throughput baseline for 3 nodes (8111–8113) | the leader was determined from the log = 8111; established the practice that "the leader must be solved from state, never guessed" | every later script follows that convention |
| `cl12.sh` | why did all three ports report `state=3`? distinguish "a genuinely concurrent multi-leader" from "three independent single-node clusters" | all three were `id=1, term=1` with their commits advancing independently (2/5/8 → 12/15/18) ⇒ **three independent single nodes** (the peer links were never established) | pinned the problem to the **cluster spec/parameters**; led to the later `cl29`/`diag3` investigation of the startup parameters |
| `cl29.sh` | can a one-shot `kdbctl` work against a **3-node** cluster (only single-node had been tested before) | incomplete record (port/leader parsing was not yet stable at the time, the `.out` output is empty) | led to `tests/cli_smoke.sh` (now an L2 gate) covering single-node semantics |
| `diag3.sh` | diagnostics for 3-node startup parameters (SPEC format and port occupancy) | recorded as the startup log of the time | together with `cl12`, separated "parameter misuse" from "library defect" |

## Latency attribution (the performance main line)

| Script | Purpose | Conclusion at the time | Impact |
|---|---|---|---|
| `cl5.sh` | 3 nodes, K=8, 200 values of 37 bytes | **977 ops/s**, p50 6.3ms; read-back verification passed | established the baseline number for "slow in the early days" |
| `cl6.sh` | four phases of **cold vs warm** on the same cluster, same shape | cold 690 / warm 737–761 ops/s ⇒ **no significant cold/warm difference** | ruled out "the first write is slow" and pointed the finger at the **batching/fsync policy** |
| `cl7.sh` | where does the ~8ms/request latency come from? a single-node K sweep (no replication at all) | K=8 → 953; K=32 → 13,191; K=128 → 21,290; K=256 → 32,673 ops/s ⇒ **the latency is in the per-request flush window, not in replication** | directly produced the adaptive flush window (EWMA derived from the measured sync cost) and the deep-pipelining optimization |
| `cl8.sh` | **does it ack the client before the leader fsyncs?** sampling the leader counters one by one | every sample was `ok(waited for a fsync)` ⇒ **the ack really does wait for the fsync** | ruled out premature ack, the most serious possible misreading |
| `cl9.sh` | the fast path for writes to a follower: did the leader really fsync+commit that burst | on the follower side **91,776 ops/s @K=32** (p50 287µs) with the leader counters advancing in step ⇒ **a legitimate relay/pipelined commit** | confirmed the high throughput was not a missed commit |
| `cl10.sh` | the true semantics of writing to a follower + reading from a follower | the write was rejected with a leader hint (`RESP|` empty body); 50 reads from the follower were all `empty` | matches the paper §6.2: a follower serves neither reads nor writes |

## Client and CLI

| Script | Purpose | Conclusion at the time | Impact |
|---|---|---|---|
| `cl11.sh` | point the probe at a follower and check whether it is told the leader's address and reconnects (the approach §6.2 recommends) | incomplete record (that run hit `sync_call_failed`, state parsing was not stable) | the behaviour is now covered by `cl10`'s empty `RESP|` body plus the `kclient` redirect logic |
| `cl13.sh` | validate "a one-shot `kdbctl` with no TTY" and the ok/err annotations of `bench_rate` | the record shows the CLI output was empty at the time while the `PROBE_VALID|yes` annotation worked | led to the no-console fallback in `kdbctl` and this round's F series insisting on `PROBE_VALID` |
| `cl14.sh` | can the server start + a one-shot CLI with instrumentation (against a piped REPL) | `CL14_DONE` / `EXIT=0` (a purely piped REPL produced no output at the time) | together with `cl13`, located the CLI interaction-mode problem |
| `cl15.sh` | after the fix: `kdbctl` must work **with no console** (output redirected to a file) | empty record (that run captured no output) | later frozen into a gate by `tests/cli_smoke.sh` |
| `repro_deep.sh` | reproduce the K=512 deep-pipelining burst and capture: the CLI report, the server log, liveness, exit status | reproduced "the server dies / stops answering under deep pipelining" | led directly to locating and fixing crashes ①② (see `doc/gaps-audit.md`) |

## Correspondence with the current gates

- Every question these scripts cared about now has a **routine gate** covering it:
  cluster/protocol → `raft_cluster_fuzz`, `kserver_cluster_fuzz`; CLI → `tests/cli_smoke.sh`;
  deep pipelining/liveness → `tools/harness/soak_release.sh`, `stall_hunt.sh`, `watch_counters.sh`;
  performance attribution → `tools/harness/perf_matrix.sh`, `burst.sh`, `pipe_frontier.sh`.
- They therefore **do not** belong in routine regression; run them individually when a historical phenomenon needs
  reproducing (the scripts now derive the repo root from their own location and can be invoked from any directory).
