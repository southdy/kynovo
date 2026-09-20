# kynovo testing and regression method (complete)

This document is the project's **single authoritative testing specification**: the layers, the exact
commands, the verdict lines, the durations, the architectural seams used for fault injection, the debugging
facilities, and the **regression discipline** and known pitfalls this project has accumulated over many
rounds of investigation. Wherever an older script comment disagrees with this document, this document wins.

- Scope: `code/` (10 headers + 2 applications), `tests/` (15 files), `tools/harness/` (15 scripts +
  `wake_probe.c`), `tools/archive/` (16 one-off investigation scripts), `tools/` top level (benchmarks,
  probes, `check-principles.py`, `cov_report.py`), `build.sh`.  Nothing hand-written lives in `build/`.
- Target platform: **Windows XP and later** (`_WIN32_WINNT=0x0501`), **MSVC 6.0 compatible C89**, single-header library.
- All commands run from the repository root under git-bash; build artifacts land only in `build/` (never in
  `%TEMP%`; reason in the `build.sh` header comment: unsigned MinGW binaries in a temporary directory are
  the most likely to trip antivirus false positives).

---

## 0. Toolchain and build gate (L0)

```bash
export PATH="/d/MinW64-15.2.0/bin:$PATH"      # required: /d/MinGW 9.2.0 on PATH fails for lack of LPFN_ACCEPTEX
export MSYS2_ARG_CONV_EXCL='*'               # required: otherwise MSYS rewrites the argv passed to native programs
./build.sh                                   # build every target (10 drivers + 8 benchmarks)
```

- `build.sh` prepends `$MINGW_BIN` to PATH and checks the version (expects **15.2.0**).
- **Test drivers use the same optimisation level as the production binaries (`-O2`) and the same warning
  gate**: `-Wall -Wextra -Wdeclaration-after-statement`
  (plus `-Wno-unused-function`, because a single-header library exposes public APIs the driver never
  calls). Principle: **tests must not be looser than production**.
- `-Wdeclaration-after-statement` specifically catches the "declaration after statement" MSVC 6.0 rejects.
- **L0 verdict**: `./build.sh` rc=0 and `grep -cE ' error|warning'` over `build/build.log` is **0**.
  > Discipline: **confirm the build succeeded before interpreting any run result**. This project has three
  > times believed a fix was ineffective when it was in fact running the stale binary a failed compile left behind.

Target quick reference (`./build.sh <target>`): `test fuzz cfuzz kclient kserver kclusterfuzz cemon-test`
and their `run-*` versions, `selftest`, `kdbsvr`, `kdbctl`, `bench*`, `cemon-bench`, `cemon-stress`,
**`coverage`** (gcov line/branch aggregation), **`sanitize` / `run-sanitize`** (UBSan trap mode, UB → SIGILL), `clean`.

---

## 1. Test layers

| Layer | Content | Command | Verdict line | Magnitude |
|---|---|---|---|---|
| L0 | compile gate | `./build.sh` | rc=0 and 0 warnings in `build.log` | ~90s |
| L1a | raft unit | `./build/raft_test.exe` | `SUMMARY: 221/221 passed` | ~1s |
| L1b | server unit (with stress mode) | `./build/kserver_test.exe` | `SUMMARY: 33/33 passed` | ~30s |
| L1c | client unit | `./build/kclient_test.exe` | `SUMMARY: 16/16 passed` | ~0.1s |
| L1d | cemon event-loop unit | `./build/cemon_test.exe` | `SUMMARY: 5/5 passed` | ~2s (includes a 2s long wait) |
| L1e | end-to-end self-test (single process, with membership change/bootstrap/election) | `./build/selftest.exe` | `selftest: PASS` | ~1–3s |
| L1f | harness cleanliness: the self-test's store artifacts are gone | (gate layer `selftest_cleanup`) | `selftest-cleanup: 0 leftover file(s)` | ~0s |
| L1g | vfs fault injection (the storage seam, exercised not assumed) | `./build/vfs_fault_test.exe` | `SUMMARY: 4/4 passed` | ~1s |
| L2 | CLI semantics smoke (real process + real socket) | `bash tests/cli_smoke.sh` | `cli_smoke: PASS` | ~10s |
| L3a | single-node randomised fuzzing (API/OOM rollback) | `./build/raft_fuzz.exe <seed> <n>` | `done: <n> iterations` / `FAIL: …` | n=2000 ~10s |
| L3b | **multi-node cluster fuzzing** (real pull-mode wire messages: drop/reorder/duplicate/partition/crash-restart/membership change/OOM injection) | `./build/raft_cluster_fuzz.exe <seed> <n> [persist_delay]` | `done: <n> iterations` / `FAIL: …` | n=2000 ~30s |
| L3c | server cluster fuzzing (with the in-harness linearizability checker `tests/lincheck.h`) | `./build/kserver_cluster_fuzz.exe <seed> <n>` | `done: 1/1 clusters consistent` **and** `linearizability: <N> histories / <M> ops decided by the checker` with N>0 | ~10s |
| L4 | network soak (release binaries, 24 rounds, in-round PIPE load + liveness check) | `RUNS=24 bash tools/harness/soak_release.sh` | `SOAK\|PASS\|rounds_run=24 fails=0 liveness=1` - one line encoding all three criteria (the gate greps this, so a 1-round run or a dead server cannot pass) | ~3min |
| L5 | performance/latency | `tools/harness/perf_matrix.sh`, `burst.sh`, `pipe_frontier.sh`, `pipe_verify.sh`, `watch_counters.sh` | each prints its own ops/s, p50/p99, counters | minutes |
| L6 | instrumentation | `./build.sh coverage`, `./build.sh run-sanitize`, `K_ALLOC_DEBUG` build + appverif/gdb (see §3) | coverage table / no SIGILL / zero reports | minutes |

Crash contract (what survives a crash on each platform, and the audited platform differences):
`doc/crash-contract.md`.

Not a layer, and therefore outside the table above: `tools/harness/git-cn-setup.sh` - GitHub reachability
from a China-based machine: ssh keepalives + a 443 alias, read-only fallback remotes (`github-https`
direct, `cn-mirror` domestic), and an opt-in repo-local proxy that refuses to write unless its port is
listening (`--status` reports, `--proxy off` reverts).

**How to read the verdict line (mandatory)**: always use a verdict grep; **`tail -1` is forbidden**. In a
suite with progress output, `tail -1` shows an unrelated line and makes a failure read as a pass; a missing
verdict line is likewise treated as a **failure**.

```bash
grep -E "SUMMARY|FAIL|done:|consistent|PASS" <log> | tail -3
```

---

## 2. Fault injection: only through the seams the architecture already has

Test infrastructure **must not intrude into production code**. If a failpoint would need an instrumented
point on a production path, **do without it**. The only surfaces that may be injected through are four seams:

| Seam | Location | Purpose |
|---|---|---|
| transport vtable | `k_server_transport` | message drop/reorder/duplicate, partition, crash (`raft_destroy`) |
| `elapsed_ms` clock injection | `raft_advance(r, elapsed_ms, …)` | election timeout/heartbeat cadence, deterministic "small-step" tail |
| runtime backend | `runtime_create("thread"\|"inline", …)` | "synchronous backend vs real thread backend" comparison under the same load (the fastest way to tell a cross-thread defect apart) |
| vfs backend | `vfs_backend` | disk errors, CRUD hooks |

OOM injection goes through the library's own allocator hooks (`RAFT_MALLOC` etc.), armed with "fail once on
the Nth allocation" semantics; the **trigger base must be reset per seed** (`fuzz_alloc_calls`), otherwise
the same seed changes behaviour with the **call volume** (fixed).

---

## 3. Debugging facilities (zero production impact)

1. **Self-describing allocator** (`code/kbase.h`, `#ifdef K_ALLOC_DEBUG`)
   - quarantine + head/tail canaries + a live-block registry + `k_dbg_verify()` on every operation; it
     reports "allocation site / **free site** / offset of the first bad byte".
   - **It must take a lock**: under multiple threads a global registry is scrambled by concurrent frees and
     produces bogus reports indistinguishable from real defects (it once manufactured 9
     `Invalid address specified to RtlFreeHeap` by itself); the entry points (alloc/free/verify) are all
     serialised.
   - Coverage: `K_MALLOC` **and** the per-layer macros (`TREAP_*`/`CEMON_*`/`RUNTIME_*`) — otherwise the
     instrumentation is **completely blind** to the "objects most likely to be corrupted" (treap
     nodes/blobs, sockets, queue nodes).
   - Debug build: `gcc -std=c89 -O0 -g -Wall -Wextra -Wno-unused-function -pthread -DK_ALLOC_DEBUG -o build/kserver_test_dbg.exe tests/kserver_test.c`
2. **`--apply-stress <ops> [keyspace] [thread]`** (a built-in mode of `kserver_test`): compresses a
   cross-thread defect that would otherwise take dozens of rounds of network soak to reproduce down to
   **seconds**, and can be compared against the synchronous backend (`… thread` vs the default).
3. **Windows page heap**: `appverif.exe -enable Heaps -for build/<exe>` → `gdb --batch -ex run -ex quit --args …`
   → **always** `appverif.exe -disable Heaps -for build/<exe>` (a machine-wide switch). Note: Application
   Verifier's Heaps only "detects", it gives no fault at the moment of the write; when no paged-heap tool
   is available, do not stand still — switch to a **targeted A/B** (turn the suspect mechanism off with a
   compile-time switch and re-run the same gate).
4. **gdb recipe**: build the **same source** with `-O0 -g`; grep the evidence lines (`^#[0-9]+ `,
   `received signal`, `exited`) instead of `tail`; the suspect frame of a crash is often **below** the true
   culprit (the framework's validity check dereferences the dangling pointer first).

---

## 4. Regression discipline (items this project bought with real cost)

1. **Run L0–L1e before the change** to establish a baseline; run the **full ladder** L0→L4 afterwards (add
   L5/L6 for major changes).
2. **Same-build A/B**: any controlled experiment must flip a switch inside **the same binary**; comparing
   across builds reads "a change of print position" as a difference of code path.
3. **Evidential strength for low-probability defects**: one clean run is **not evidence** (the same binary
   can go from 2/3 failing to 0/8 passing within minutes, depending on machine load). Use **dozens of
   samples** or a **long-run gate** (e.g. a 60-round page-heap network soak), and after a fix **re-run the
   gate that used to reproduce**.
4. **Look for the "first divergence" first**: the failure a multi-invariant harness reports is the **end** of
   the chain. In the event stream, first locate **the earliest place where "two independent records disagree
   about the same state"** (model vs component report, image vs reported durable, applied vs durable), then
   bisect on **that event**. The criterion: **two or more separately sound fixes leave pass/fail utterly
   unmoved ⇒ stop patching and go find the first divergence**.
5. **The measuring apparatus must prove itself first**: establish "positive signal" assertions before
   running (server liveness probe, per-round `ok=<N>`), and count a round that is neither a success nor a
   failure as `unexpected` and abort — otherwise "0 failures" may just be "0 data" (this project did it
   once: a missing argv argument got 20 empty rounds reported as clean).
6. **Instrumentation must be reverted**: probes (temporary prints/instrumented points) are all removed once
   the case is closed, and leftovers are checked with an **exact-tag grep**; all that should remain is **the
   fix itself** and **diagnostics of lasting value** (e.g. `--apply-stress`, the `SEED` marker).
7. **Do not write a hypothesis as a conclusion, and do not truncate evidence**: `grep … | head -N` hides the
   fatal line (this project misjudged "the tree is only accessed by the main loop" once because of it); a
   refuted hypothesis must be **explicitly retracted** in the documents.
8. **Do not edit control flow line-by-line in bulk**: one bulk line edit in this project turned a "guarded
   close" into an unconditional close and created a regression where the server actively kicked clients.
9. **Handles/ownership**: every close site is "**stash, then clear, then close**"; when a pointer crosses
   layers, the release point must be at the **contract end point** (not at the "data has been copied" site).
10. **Platform constraints are part of the regression**: new code must pass
    `-Wdeclaration-after-statement`; it must not introduce C99 (`inline`/VLA/`stdint.h`), and must not use the
    `long long` literal suffix `ULL` (MSVC 6.0 will not take it); `__int64` is wrapped by `kbase.h`.

---

## 5. Known pitfalls (harness side, measured in this project)

1. **The driver's model must agree with "the persistent state the library reports"**: `raft_cluster_fuzz`
   once had the library wrongly judged buggy because of three model defects:
   - rewriting the image wholesale from the **incremental/heartbeat view** ⇒ zeroing config entries that were
     already persisted ⇒ restoring **old members** after a crash ⇒ overwriting committed entries (LEADER
     COMPLETENESS);
   - after the truncation criterion became "same index, different term ⇒ truncate", the **old image
     entries** then displaced **committed newer entries** (LOG MATCHING);
     ⇒ the only correct authority is **the library's own log tip**: drop only what the library no longer
     holds, keep whatever the library still holds, and merge by overwriting the term at each index.
   - the black-box log tip (`last_index`) was taken from the persist view ⇒ the tip of the incremental view
     is just the snapshot boundary ⇒ a new leader after restart was counted as "missing entries" ⇒ a bogus
     leader-completeness report ⇒ it must be **derived from the library's log**.
2. **The image invariant needs a single choke point**: every path that mutates the image calls the same
   normalisation function at its end (the **contiguous prefix** starting at `disk_lii`), otherwise you get a
   **hole-ridden image** such as "boundary 1 + first entry 3", the library correctly refuses to restore it,
   and the node dies in a fault-free tail, showing up as a **bogus LIVENESS failure**.
3. **Event buffer capacity**: a ring buffer (`EV_MAX`) that is too small squeezes the first half of the
   events out for a failing seed (symptom: the "first apply" the oracle recorded cannot be found in the
   dump) ⇒ either enlarge it, or write the whole ring to disk on failure.
4. **The seed must be identifiable**: print `SEED n` into the event stream, otherwise you can only bisect by
   call volume to find the failing seed.
5. **Call volume affects behaviour**: any global that is not reset across seeds (especially OOM arming in
   the form of an **absolute** allocation ordinal) makes "the same seed give different results at different
   `count`".

---

## 5b. One-shot gate: `./build.sh regress`

The **single stable entry point** for automation (agents, cron and CI all use it; do not assemble your own
command lists):

```bash
./build.sh regress quick     # default: principles + build (0-warning assertion) + 4 unit suites + selftest + cleanliness + CLI smoke   ~2.5 min
./build.sh regress fuzz      # adds raft_fuzz 2000 / raft_cluster_fuzz 200 / kserver_cluster_fuzz 1                       ~5 min (**this is what CI runs on every push/PR**)
./build.sh regress full      # fuzz with release-sized parameters (20k/2000/10) plus a 24-round release soak              ~20 min (nightly)
```

- Every layer prints one line `GATE|<layer>|pass|FAIL|<verdict line>|<seconds>`; **the verdict line must
  appear** — a silent run (exit 0 but no verdict line) is judged **FAIL**; this is where "0 failures does
  not mean 0 data" is made concrete;
- **the last line is machine-readable**: `REGRESS|quick|pass=9 fail=0 duration=144s` (the `principles` layer
  added: `python tools/check-principles.py`) (an agent can decide by reading this line alone);
- on failure it prints that layer's log path and the last 12 lines; logs live in `build/regress/<layer>.log`;
- the gate's own failure path is self-tested: `bash tools/harness/regress_selftest.sh` (expects `pass=1 fail=2`).

## 6. Recommended gate order and time budgets

| Scenario | Command sequence | Budget |
|---|---|---|
| daily small change | L0 + L1a/b/c/d/e + L2 | ~2.5min |
| touches raft/cluster semantics | the row above + `raft_fuzz 1 2000` + `raft_cluster_fuzz 1 2000 0` (re-run once with the default delay) + `kserver_cluster_fuzz 1 5` | ~5min |
| touches server/transport/snapshot | the row above + L4 (24 rounds) + `--apply-stress` (`K_ALLOC_DEBUG` build + appverif/gdb) | ~10min |
| before release | the row above + L6 (`coverage`, `run-sanitize`) + `perf_matrix.sh` | ~20min |

**Working artifacts and cleanup**:
- **`build/` is a pure output directory and should start empty**: everything `./build.sh` generates lives
  here, and `./build.sh clean` **deletes the whole directory** (equivalent to a fresh checkout). **Do not put
  any hand-written file into `build/`** — scripts and data have their own homes (see below).
- Tool scripts live in two places: `tools/harness/` (15 reusable harnesses plus `wake_probe.c`, see the table
  below) and
  `tools/archive/` (16 early one-off investigation scripts, whose conclusions are summarised in
  `doc/investigations.md`).
- Measurement and investigation records are archived in `doc/measurements/` (`*.txt` = the raw
  `STATS/PHASE/LAT` data of each benchmark, `*.out/.err` = investigation-script output; **not
  byte-reproducible**). For how to read them see that directory's `README.md`.
- When you need historical data, check the headline numbers in `doc/measurements/README.md` first, then
  decide whether to re-measure.

## Reusable harness index (tools/harness/)

Every script changes to the repository root itself and sets up the toolchain environment, so it can be
invoked from anywhere; under `set -u` they all carry sensible defaults and can be run bare.

| harness | Purpose | Command | Verdict line | Precondition |
|---|---|---|---|---|
| `soak_release.sh` | repeated deep pipelining on the release build + a liveness check each round | `RUNS=24 bash tools/harness/soak_release.sh` | `rounds_without_full_success=0` and `final liveness: 1` | ports 9481/9482 free |
| `pipe_verify.sh` | real client pipelining + three correctness gates | `bash tools/harness/pipe_verify.sh [K]` | all three tiers `ok=<n> not_found=0` | ports 9931/9932 |
| `pipe_frontier.sh` | throughput/latency frontier (per-response timing) | `bash tools/harness/pipe_frontier.sh` | `ops_per_s` and percentiles per tier | as above |
| `burst.sh` | open-loop capacity (K≥N, not self-limited by the client) | `bash tools/harness/burst.sh` | peak `ops_per_s` | as above |
| `perf_matrix.sh` | attribution matrix: rounds/per-write/WAL(mem vs disk)/payload/batching | `N=2000 bash tools/harness/perf_matrix.sh` | `ops_per_s` + `flush_by_*` per cell | `proc_time` must be built |
| `watch_counters.sh` | repeated load + print the admission counters (look for monotonic growth = a leak) | `RUNS=16 bash tools/harness/watch_counters.sh` | whether the counters grow | as above |
| `cpu_probe.sh` | server CPU vs wall (CPU-bound or waiting) | `bash tools/harness/cpu_probe.sh [tag]` | CPU% | `tools/proc_time.c` |
| `conn_leak.sh` | whether new connections are still accepted after many short connections | `bash tools/harness/conn_leak.sh` | whether each round is accepted | as above |
| `stall_hunt.sh` | stall control experiment (release vs debug+Heaps) | `RUNS=20 bash tools/harness/stall_hunt.sh` | `stalls=0` and `server_alive=1` | ships its own liveness probe and "abort when there is no ok=" |
| `crash_hunt.sh` | repeat deep pipelining under gdb until it crashes, leaving a backtrace | `bash tools/harness/crash_hunt.sh` | gdb stack frames | gdb required |
| `pageheap_hunt.sh` | appverif Heaps + repeated deep pipelining (locates the write itself) | `RUNS=60 bash tools/harness/pageheap_hunt.sh` | no `Free Heap block …` / alive | `appverif` required (must be disabled afterwards) |
| `diag_deep.sh` | whether the server closes the connection at deep K | `bash tools/harness/diag_deep.sh` | server log and client report | ports free |
| `regress_selftest.sh` | self-test of the GATE's own failure path (silent-but-zero-exit / verdict line present / crash) | `bash tools/harness/regress_selftest.sh` | `pass=1 fail=2` | none |
| `rate_profile.sh` | per-round timing profile of the server's own counters | `bash tools/harness/rate_profile.sh` | its own `PHASE`/`STATS` lines | ports free |
| `wake_probe.c` | C helper used by the wake-latency investigations (`wake_us_*` in STATS) | build it by hand (`gcc … tools/harness/wake_probe.c`) | its own report | toolchain on PATH |

**Delivery criteria**: L0 0 error/0 warning; L1 all green; L2 PASS; L3 `done`; L4
`rounds_without_full_success=0` and `final liveness: 1`; no leftovers in the working directory (e.g.
`kdb-selftest-*` — enforced by the `selftest_cleanup` layer, not by eye); no leftover processes (`ps -W | grep -icE 'kdbsvr|kdbctl'` is 0).

## 7. Windows XP + MSVC 6.0 on real hardware

The constraint "C89 / MSVC 6.0 / Windows XP+" is verified on a real Windows XP SP3 guest with Visual
C++ 6.0 (cl 12.00.8804) and the Windows Platform SDK, not by inspection.

How it is driven (no sshd on XP; the host's SMB1 client is removed, so SMB is not an option):

- the host serves the 22 sources plus the build/run scripts over TFTP from a read-only root, and
  accepts uploads into an inbox directory;
- `go.bat` is a stable bootstrap that never changes (XP's `tftp.exe` refuses to overwrite an existing
  file, and a running batch is locked), so all mutable logic lives in `kynovo_step.bat`, which the
  bootstrap deletes and re-fetches every run;
- the step pulls sources, clears the read-only attribute (`attrib -R`; the tftp limitation plus the
  read-only bit is what produced the stale-script runs), builds with `cl`, runs the end-to-end
  sequence, and uploads `build_xp.log` and `run_xp.log`;
- every script prints its own size (`%~z0`) so the log proves which revision executed, and the step
  prints `GET failures: N` so a partial pull is impossible to miss.

Expected evidence: `kdbctl_cl_rc=0`, `kdbsvr_cl_rc=0`, both `*_link_rc=0`, `dir /b *.exe` listing
`kdbsvr.exe` and `kdbctl.exe`, and a run reproducing the host baseline line for line
(`ok` / `hello` / `ok` / `(not found)`).

Foreign toolchains report warnings without enforcing them; the 0-warning contract remains pinned to
the gcc version this project builds with.  MSVC 6 reports C4244/C4761/C4133 on the same code - see
the P1 entry in `doc/gaps-audit.md` for which of those were real.

### Measured on the XP machine (see P2 in `doc/gaps-audit.md`)

Build, run and the unit suites, all on Windows XP SP3 with cl 12.00.8804, sources md5-verified
against the repository before transfer:

- build: `cl` and `link` return 0 for both executables; `dir /b *.exe` lists `kdbsvr.exe` and
  `kdbctl.exe`;
- run: `init` -> `server 1 client=127.0.0.1:7101 peer=127.0.0.1:7102` -> `SET`/`GET`/`DEL` reproduce
  the host baseline line for line (`ok` / `hello` / `ok` / `(not found)`);
- crash contract: after an abrupt `taskkill` and a restart on the same store, the key written before
  the kill reads back, and a key that was never written reads `(not found)` - the negative control
  that keeps the check from being unable to fail;
- unit suites: `cemon_test 5/5`, `kclient_test 16/16`, `raft_test 221/221`, `kserver_test 34/34`,
  `vfs_fault_test 4/4` - identical to the local gate, including the storage injection seam, whose four
  cases (transparent wrapper / fsync failure / write failure / failure during open) build and pass under
  cl 12.00.8804 with the Platform SDK;

The fuzz drivers were added to that evidence afterwards.  With the seam test included, every one of the
twenty-four compile/link/run exit codes is zero as well (`cl` and `link` for eight drivers, then eight runs):

- fuzz drivers: `raft_fuzz 0 200` -> `done: 200 iterations`, `raft_cluster_fuzz 1 2` -> `done: 2
  iterations`, and `kserver_cluster_fuzz 1 1` -> `done: 1/1 clusters consistent` with
  `linearizability: 4 histories / 253 ops decided by the checker, 0 inconclusive` (non-zero, so the
  checker can be seen to have decided something - its own `lincheck_selftest` runs inside that binary).

So the XP verification now covers build (seven test programs plus the two applications), the end-to-end
run, the crash contract, the four unit suites and the deterministic fuzz drivers including the
linearizability oracle.  `bench_persist.c` is the one test program still outside that set: it includes
`<pthread.h>` and needs a port rather than a rename, and it is a benchmark, not part of the gate chain.

### Harness rule learned here: the SDK include path AND the target version

Two separate things were needed before the test programs would compile on that guest, and only
fixing both made the `SwitchToThread` C4013 disappear:

1. the SDK include path must be on the `cl` command line (`/I"%SDKINC%"`), exactly as in the
   application build script - it was missing from the test script, so VC98's headers were used;
2. the target version must be passed explicitly (`/D_WIN32_WINNT=0x0501 /DWINVER=0x0500`), because
   the test files include `<winsock2.h>`/`<windows.h>` *before* any project header, and it is
   `code/cemon.h` that sets `_WIN32_WINNT`.  With `<windows.h>` already processed under an
   undefined version, the XP-era SDK skipped that API in `winbase.h`, the compiler assumed a cdecl
   extern and the linker asked for `_SwitchToThread` instead of `_SwitchToThread@0`.  The
   application build never showed this because `kdbsvr.c` includes `cemon.h` first.

After both fixes: all four suites compile with no C4013 for that symbol, link with rc=0, and pass
(`cemon 5/5`, `kclient 16/16`, `raft 221/221`, `kserver 33/33`) - with the product carrying no
workaround, which is why the earlier `cemon.h` declaration was reverted.
