# kynovo

A single-header **Raft-replicated ordered KV store** in C89: an ordered index (treap with
copy-on-write snapshots), a Raft core with joint-consensus membership changes, streaming snapshots,
an 8 KiB event loop, and pluggable runtimes/transports/VFS.

- **Constraints (non-negotiable)**: Windows XP and later (`_WIN32_WINNT=0x0501`), **MSVC 6.0 compatible
  C89** (no `inline`, no VLAs, no `stdint.h`, no `long long` literals), LF line endings everywhere,
  single-header libraries with zero external dependencies.
- **Status**: pre-production. Validated by layered gates (unit, CLI smoke, randomised single-node and
  cluster fuzzing, release soaks) - see `doc/testing.md`. Not yet run on real multi-machine clusters.

## Layout
```
code/        the libraries and the two applications (server + CLI)
tests/       unit drivers, fuzzers, end-to-end self-test, CLI smoke
tools/       C benchmarks/probes; tools/harness/ = reusable shell harnesses; tools/archive/ = past investigations
doc/         testing+regression method, gap/backlog audit, investigations, review, raw measurements
build.sh     the only build entry point
build/       pure output (created by build.sh; `./build.sh clean` removes it entirely)
```

## Build (git-bash on Windows)
```bash
export PATH="/d/MinW64-15.2.0/bin:$PATH"   # MinGW-w64 15.2.0; older headers lack LPFN_ACCEPTEX
export MSYS2_ARG_CONV_EXCL='*'            # MSYS must not rewrite argv for native binaries
./build.sh                                # all drivers + benchmarks; must be 0 error / 0 warning
```

## Run a single node
```bash
./build/kdbsvr.exe init  "disk://build/data/a"          # create the store
./build/kdbsvr.exe server 1 8501 8502 "disk://build/data/a" "1@127.0.0.1:8501:8502" &
./build/kdbctl.exe 127.0.0.1:8501 SET k v
./build/kdbctl.exe 127.0.0.1:8501 GET k
./build/kdbctl.exe 127.0.0.1:8501 STATS
```

## Test
```bash
./build.sh run-test        # raft unit suite
bash tests/cli_smoke.sh    # end-to-end CLI semantics
RUNS=24 bash tools/harness/soak_release.sh   # release soak with per-round liveness
```
## Gate (the single entry point - CI, cron and agents all use it)

```bash
./build.sh regress quick   # principles + build (0-warning assertion) + 4 unit suites + selftest + cleanliness + CLI smoke
./build.sh regress fuzz    # adds raft_fuzz 2000 / raft_cluster_fuzz 200 / kserver_cluster_fuzz 1  (what CI runs)
./build.sh regress full    # release-sized fuzz (20k / 2k / 10 clusters) + a 24-round release soak  (the nightly)
```

Every layer prints a `GATE|<layer>|...` line and the run ends with one machine-readable verdict
(`REGRESS|quick|pass=9 fail=0 duration=150s`).  A layer that exits 0 **without** its verdict line counts as
a failure, so an empty run can never read as a pass.

The full method (layers, exact commands, verdict lines, time budgets, the regression discipline and the
known harness pitfalls) is in [`doc/testing.md`](doc/testing.md) - read it before adding tests.  The
constraints and how each is enforced: [`doc/principles.md`](doc/principles.md); what survives a crash:
[`doc/crash-contract.md`](doc/crash-contract.md).

## Licence
Apache License 2.0 (see `LICENSE`).  Copyright 2026 kynovo contributors.
