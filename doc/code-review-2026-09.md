# kynovo full engineering review (2026-09)

> **Superseded (2026-09-20) by [`doc/code-review-2026-09b.md`](code-review-2026-09b.md).**  Read that one
> for the current state: several items below are already fixed (version control, `regress`, README, the
> `build/` cleanup), and the **body** of this document still lists them as open while its own appendix
> marks them fixed - that contradiction is itself recorded as finding F7 in the new review.  This file
> stays as the record of that round; its inventory numbers are from then (e.g. `kserver.h` was 4,850
> lines, it is 5,018 now).

**Scope**: `code/`, `tests/`, `build/*.sh`, `tools/`, `doc/`, `build.sh`, and workspace hygiene.
**Method**: inventory (size/reference relationships) + item-by-item reading of the critical paths + verification with **runtime evidence** in this session (the L0–L4 ladder all green, see `doc/testing.md`).
**Summary of conclusions**: engineering quality is high (clean layering, explicit constraints, complete test tiering); the main risks cluster around **size**, **classification and discoverability of scripts/docs**,
and **workspace hygiene**; no new functional defects were found.

---

## 1. Inventory

| Area | Files | Lines | Notes |
|---|---|---|---|
| `code/` | 12 | 18,666 | 10 headers + 2 applications (`kdbsvr.c`, `kdbctl.c`) |
| `tests/` | 15 | 19,287 | 4 unit drivers + 3 fuzz + selftest + 2 benchmarks + framework/linearizability checker |
| `tools/harness/` | 12 | ~700 | **reusable harness** (soak/hunt/perf/correctness; index in `doc/testing.md`) |
| `tools/archive/` | 16 | ~800 | early one-off investigation scripts (conclusions summarized in `doc/investigations.md`) |
| `tools/` | 11 | 1,834 | 5 benchmarks + 2 probes + coverage aggregation + treap stress |
| `doc/` | 2 → 6 | 11,500+ | `dissertation.md` (the paper, semantic baseline), `gaps-audit.md` (gap list); this round added `testing.md` and this file |
| Build | 1 | ~370 | `build.sh`: 10 drivers + 8 benchmarks + `coverage` / `sanitize` / `clean` |

Layering (dependency direction top-down, acyclic):

```
kdbsvr.c / kdbctl.c (applications)
  └ kserver.h (server core + peer protocol, 4850 lines) ── kproto.h ── kbase.h
  └ kclient.h (client state machine, 514 lines)
  └ raft.h (consensus + snapshot, 4666 lines) ── kproto.h
  └ treap.h (ordered index + COW, 1070 lines) —— mechanism layer, **does not depend on kbase**
  └ runtime.h (threads/event loop, 807 lines)／ vfs.h (IO abstraction, 460 lines)／ cemon.h (Win32 event loop, 3626 lines)
  └ cli.h (CLI message layer, 782 lines, used by kdbctl only)
```

The four seams (injectable surfaces): `transport` vtable, `elapsed_ms`, `runtime` backend, `vfs` backend —— see `doc/testing.md` §2.

---

## 2. Code (`code/`)

**Done well**
- **Clear layering and dependency direction**: `treap.h` is a pure mechanism layer (does not depend on `kbase.h`, carries its own `TREAP_MALLOC` macro),
  while policy (seed, boundary checks, concurrency gating) stays above; `raft.h` depends on `kproto.h` only.
- **Constraints are explicit and automatically checkable**: all three constraints (C89/MSVC 6.0/XP) live in the build gates (`-Wdeclaration-after-statement`,
  no C99 facilities, `kbase.h` wrapping `__int64`); all code added this round passes.
- **Error visibility**: invariant warnings and fail-stop branches print a reason; `Fail-stop` has preconditions such as `server_is_stopping`, avoiding reading a normal shutdown as a crash.
- **Persistence contract**: the `raft_persist` view + `raft_persist_complete(durable_index)` rule of "reporting only the prefix that is actually durable" is fully implemented library-side
  (a follower's AE response is `deferred_append_response`d until its own write to disk is confirmed, and only `durable_confirm` is reported).
- **Snapshot ownership**: after splitting `treap_save` (snapshot thread, **read-only**) / `treap_save_finish` (**owner thread**, reclaim + reset),
  non-owner threads no longer free or write shared state; the header comment spells out the invariants.

**Risks and recommendations**

| ID | severity | Observation | Evidence | Recommendation |
|---|---|---|---|---|
| C-1 | medium | `kserver.h` at 4,850 lines and `raft.h` at 4,666 lines make locating things in a single file costly | `wc -l` | no rush to split (the layering is already clear), but maintain a "section index" at the top of the file and place new features preferentially inside existing sections |
| C-2 | medium | the non-Windows (epoll/kqueue) branch has **never been compiled** (18 places) | `grep -rn epoll\|kqueue code/*.h` | keeping it as a compile-time branch is fine, but the docs should state plainly that it is "unverified"; do not claim cross-platform externally |
| C-3 | low | `cli.h` is used by `kdbctl.c` only, yet sits alongside the library headers | `grep -rl cli.h` | semantically it belongs to the application layer; consider moving it to `code/app/` or annotating its layer |
| C-4 | low | `R8`: a rejected AE can carry a new term that is not written to disk | `doc/gaps-audit.md` §R8 | low risk (a dual leader in the same term still needs a majority intersection); to be strict about §3.8, add a durability gate to the rejection response (cf. `deferred_vote_response`) |
| C-5 | low | a full disk leads to fail-stop; `README`/metrics deferred | existing decision | keep; but recommend an explicit "known trade-offs" list in `gaps-audit.md` so these are not re-raised as gaps later |

---

## 3. Tests and build (`tests/`, `build.sh`)

**Done well**
- **Tests are stricter than production**: drivers and release binaries use the **same `-O2`, the same warning gates** (including the MSVC 6.0-specific warnings); this principle blocks "tests masking production warnings".
- **Complete tiering**: unit → CLI smoke → single-node/cluster fuzz (with linearizability checking) → network soak → performance → coverage/UBSan.
- **Benchmarks are under the warning gates too**: `build_all` compiles the 8 benchmarks, preventing them from "rotting silently".
- **`build.sh coverage` failures cannot be masked**: each driver gets its own log + its own exit code (historically a subshell caused a red build to be reported green).
- **UBSan trap mode**: MinGW has no libubsan runtime, so `-fsanitize-trap=undefined` makes UB → SIGILL, and the last printed seed is the reproduction point.
- **Fault injection goes through the four seams only**, without intruding on business paths (the principle is explicit).

**Risks and recommendations**

| ID | severity | Observation | Evidence | Recommendation |
|---|---|---|---|---|
| T-1 | medium | no single entry point for the "**default gate**": one full regression means typing 8 commands by hand | this session's invocation sequence | add `regress` to `build.sh` (L0–L4 in one shot, per-gate verdict lines, stop on failure) and `regress-fuzz N`; align with `doc/testing.md` §6 |
| T-2 | medium | the "failing seed — event window" coupling in fuzz: a 1,024-entry ring buffer cannot hold one seed's history | this session F5 | enlarge the ring buffer or dump everything to disk on failure (test-driver side, not product) |
| T-3 | low | `raft_cluster_fuzz` determinism depends on "a complete global reset across seeds" | this session (fixed `fuzz_alloc_calls`) | any new global must be added to the per-seed reset list, and this constraint must be commented at that function |
| T-4 | low | `tests/lincheck.*` (the Python helper) is not covered by any `build.sh` target | `grep -rl lincheck` | document how to invoke it in `doc/testing.md` (currently used indirectly through `kserver_cluster_fuzz`) |

---

## 4. Scripts (relocated)

**Current state (after this round's clean-up)**: `build/` is a **pure artifact directory** (`./build.sh clean` deletes the whole directory, equivalent to a fresh checkout),
and all hand-written content has been moved out:
- `tools/harness/` (12): `soak_release`, `pipe_verify`, `pipe_frontier`, `burst`, `perf_matrix`, `watch_counters`,
  `cpu_probe`, `conn_leak`, `stall_hunt`, `crash_hunt`, `pageheap_hunt`, `diag_deep` —— all changed to **derive the repo root from their own location**,
  carry their own toolchain environment (`PATH`/`MSYS2_ARG_CONV_EXCL`), and have sensible defaults (runnable bare); index in `doc/testing.md`.
- `tools/archive/` (16): `cl3`–`cl29` (15 of them), `diag3`, `repro_deep` —— conclusions summarized in `doc/investigations.md`;
  the records for `cl11`/`cl13`/`cl14`/`cl15`/`cl29` are snapshots taken "while the problem was still unfixed" (marked as such honestly).
- `doc/measurements/`: 177 raw records (`*.txt`: 103 files with `PHASE` data; `*.out/.err`: investigation conclusions).

**Leftovers**: the scripts still finish with `taskkill /F /IM kdbsvr.exe` (crude but idempotent); historical scripts such as `cl7` use fixed ports and
conflict when run in parallel —— the precondition is noted in the index.

## 5. Tools (`tools/`, 11 of them)

`bench_fsync` (fsync microbenchmark), `bench_e2e`, `bench_mt`, `bench_pipe`, `bench_rate` (end-to-end/concurrency/pipeline/throughput),
`bench_client.h`/`bench_env.h` (shared benchmark headers), `cemon_tcp_probe.c` (connection-identity probe), `proc_time.c` (read-only CPU time),
`treap_stress.c` (treap-layer stress, 2 million iterations, zero warnings), `cov_report.py` (gcov aggregation).

**Observation**: all are aligned with `build.sh` (no missing source references); all except `cemon_tcp_probe`/`proc_time`/`treap_stress` are under the warning gates.
**Recommendation**: bring those three into `build_all` as well (into the `-Wall -Wextra -Wdeclaration-after-statement` gate); the cost is minimal.

---

## 6. Docs (`doc/`)

| File | Status |
|---|---|
| `dissertation.md` (7,988 lines) | the paper's original text, the **semantic baseline** (on conflict, the paper semantics + design consistency prevail) |
| `gaps-audit.md` (406 lines) | gap list + sections E/F from this session (8 falsified hypotheses, 4 ineffective patches, three root causes and their fixes) |
| `testing.md` (added this round) | test and regression method (the single authoritative description) |
| `code-review-2026-09.md` (this file) | full review |

**Gaps**: no `README` (build/run/CLI usage) and no architecture overview; `WSM.md` was judged unnecessary (transactions = FCALL + treap COW).
**Recommendation**: add a `README.md` next round (20–40 lines: dependencies, `build.sh` targets, minimal run example, CLI command table),
write in "why artifacts land in `build/`" (antivirus false positives), and reference `doc/testing.md` for the remaining details.

---

## 7. Workspace hygiene

| ID | severity | Observation | Evidence | Recommendation |
|---|---|---|---|---|
| H-1 | ~~medium~~ **fixed** | `build/` once reached **349 MB** (`build/perf/` held 437 investigation logs); and `./build.sh clean` was originally `rm -rf build/`, which **would have deleted the 28 investigation scripts along with it** (a latent hazard) | `du -sh build`; the original `do_clean()` text | cleaned up under "keep valuable process artifacts": **keep** the 28 `*.sh` and the archive records `build/records/` (177 files / 429 KB: each benchmark's `STATS_BEFORE/PHASE/LAT/STATS_AFTER` and the investigation scripts' `.out/.err`), delete everything else (33 `.exe`, `perf/`, the `bench-*`/`cl*`/`stall`/`soak`/`cov` data directories, `chk.o`/`rt_dbg3.exe`); `do_clean()` changed to **delete artifacts only, keeping `*.sh` and `records/`**. Result: **349 MB → 3.9 MB**, and after a rebuild L0 rc=0/0 warnings, L1 all green |
| H-2 | medium | **no version control** (no `.git`, no `.gitignore`) | `ls -a \| grep '^\.git'` is empty | the highest-value-for-effort process improvement: `git init` + `.gitignore` (`build/`), establishing a commit baseline at least for `code/`, `tests/`, `doc/`, `build/*.sh`, `tools/` |
| H-3 | low | the root directory once had `kdb-selftest-*` leftovers (30 of them, fixed) | this round F4 | closed; recommend continuously checking "no leftovers in the working directory" among the delivery criteria of `doc/testing.md` §6 |
| H-4 | low | debug artifacts such as `build/kserver_test_dbg.exe` are mixed in with release artifacts | `ls build` | already guaranteed by §testing's "zero trace of debug facilities in production"; a `build/debug/` subdirectory convention could be added |

---

## 8. Priority recommendations (ordered by value for effort)

1. **H-2 version-control baseline** (one-off, the largest payoff).
2. **T-1 `build.sh regress` one-shot gate** (turns `doc/testing.md` §6 into an executable entry point).
3. **§4 script classification + `build/README.md`** (turns 28 scripts from "only the author knows" into "discoverable").
4. **H-1 log and debug-artifact cleanup targets**.
5. **C-1/C-2 size and platform notes** (documentation annotations; no rush to touch the code).
6. **`README.md`** (aimed at new users/new sessions).

> Existing trade-offs not listed as recommendations (kept per the established judgement): fail-stop on a full disk, no directory-sync primitive, `vfs` not exposing error codes,
> `treap_save` retaining the reference count, exactly-once cured at the root by command idempotence, multi-node testing on real machines not yet feasible.

---

## Appendix: this round's fixes on record (P0/P1; plan in `doc/plan-github-automation.md`)

| Original ID | Problem | Current state |
|---|---|---|
| H-1 | no version control / no `.gitignore` | **fixed**: `git init` (branch `main`) + `.gitattributes` (LF enforced) + `.gitignore` (ignores `build/`, the third-party paper `doc/dissertation.md`, and the personal machine paths in `.vscode/`); first commit 255 files |
| T-1 | no "one-shot gate"; a full regression means typing 8 commands by hand | **fixed**: `./build.sh regress quick\|full`, one `GATE\|…` line per layer, last line `REGRESS\|quick\|pass=7 fail=0 duration=150s`; **an empty run (exit 0 but no verdict line) is judged FAIL**; the gate's own failure path is self-tested by `tools/harness/regress_selftest.sh` |
| new | `tests/cli_smoke.sh` hardcoded `disk://D:/kynovo/…` (B3, critical path) | **fixed**: now derives the repo root from the script's own location (`pwd -W` for the native form); runnable from any directory |
| new | `.vscode/c_cpp_properties.json` contained this machine's absolute gcc path | **excluded** (added to `.gitignore`, not committed) |
| B3 remainder | 28 scripts still contain hardcoded data paths (mostly `tools/archive/` historical investigation scripts) | **todo (P1 remainder)**: the core paths (`build.sh`/`cli_smoke.sh`) are already clean; parameterize the rest as needed |
