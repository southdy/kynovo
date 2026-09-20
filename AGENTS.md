# kynovo - working rules for agents and humans

Read this first. It is deliberately short; the details live in `doc/`.

## Hard constraints (a change that violates one of these is rejected, not negotiated)
- **C89 / MSVC 6.0 compatible / Windows XP+**: no C99 (`inline`, VLAs, `stdint.h`, `//`-only details),
  no `long long` literals with `ULL` suffixes; `__int64` is wrapped by `kbase.h`.
- **LF line endings everywhere** (see `.gitattributes`). Never introduce CRLF.
- **`build/` is a pure output directory.** Never hand-write a file into it. `./build.sh clean` deletes
  it wholesale. Tooling lives in `tools/harness/`, archived investigations in `tools/archive/`,
  raw measurements in `doc/measurements/`.
- **Do not change the `raft.h` / `treap.h` contracts** without first aligning the change with the
  semantic baseline (Ongaro's dissertation, kept locally, and `doc/dissertation.md` references in the
  code) and saying which paragraph the change follows.

## Workflow
1. Run the baseline gates **before** changing anything.
2. Make the change; keep it in one logical unit.
3. Run `./build.sh regress quick` while iterating and `./build.sh regress fuzz` before merging (that is
   what CI runs); read the last line, which is machine-readable:
   `REGRESS|fuzz|pass=12 fail=0 duration=161s` (`quick` is 9 layers, `full` 13).  `regress full` is the
   nightly/release gate.
4. Report with **evidence**: the verdict lines, the log path, the exit code. No "should be fine".
5. Update `doc/gaps-audit.md` for any gap you fixed, opened, or refuted.

## Discipline (each rule here cost real time to learn)
- **Confirm the build succeeded before interpreting any run** - three separate wrong conclusions came
  from reading results of a stale binary after a failed compile.
- **A build that fails with `Permission denied` on a build output is a lingering process, not a code
  error** - kill it (`pkill -x kdbsvr.exe`) and `rm -f build/kdbsvr.exe` first.  Twice now the gate line
  read `GATE|build|FAIL|... errors=1` for that reason alone.
- **Never commit or push on a red gate line**, whatever the apparent cause: a locked output means the
  compiler never re-ran, so "it is only the environment" is a claim that still has to be proven by a green
  rerun before the push, not after.
- **Same-build A/B.** Never compare across builds: moved prints read as different paths.
- **Low-probability defects**: a single clean run is not evidence. Use tens of samples or a long gate,
  and after a fix re-run the exact gate that used to reproduce.
- **Multi-invariant harness failure**: locate the FIRST divergence in the event stream; do not reason
  backwards from the reported failure. If two well-argued fixes change nothing, stop patching and go
  find the first divergence.
- **Measurement must certify itself**: assert the positive signal (`ok=<N>`, server alive, health probe)
  and abort on rounds that match neither success nor the expected failure - "0 failures" must not mean
  "0 data".
- **Instrumentation comes out clean**: revert temporary prints/probes; keep only the fix and long-lived
  diagnostics. Check residue with exact tag greps, never loose character classes.
- **Never batch-edit control flow line by line**; free at the contract's end, not where the data was
  copied; every close site copies-then-clears-then-closes.
- **Don't over-claim**: state assumptions as assumptions, and retract refuted ones explicitly in
  `doc/gaps-audit.md` (that file keeps a running list of retractions on purpose).

## Accepted automation decisions (agreed with the maintainer)
- Repository: **public**. `doc/dissertation.md` is ignored (third-party copyright) and stays local.
- CI: full - build (0-warning assertion) + unit + CLI smoke + fast fuzz on every push/PR, plus a nightly
  job (20,000-iteration single-node fuzz + 2,000-cluster fuzz + 10-cluster linearizability run, a 24-round
  release soak, coverage and a UBSan trap build).
- Backlog: **dual** - GitHub Issues for humans, Hermes kanban for agent workers.
- **Direct pushes to `main` are allowed**, so the gate is discipline: run `./build.sh regress --quick`
  immediately before every push; the nightly job is the safety net, not the first line.

## Entry points
- **Principles and how each one is enforced: `doc/principles.md`** (`python tools/check-principles.py` runs as a gate layer)
- Gates and method: `doc/testing.md` · Backlog and evidence: `doc/gaps-audit.md`
- Crash contract (what survives a crash, per platform): `doc/crash-contract.md`
- Harnesses: `tools/harness/` · Past investigations: `doc/investigations.md`
- Raw measurements: `doc/measurements/` · Review: `doc/code-review-2026-09.md`
- Plan for GitHub/automation: `doc/plan-github-automation.md`
