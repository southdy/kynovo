# kynovo full-project review — 2026-09d (fourth round)

## How this round is run

Seven read-only module audits run in parallel (an audit of the previous round's own fixes, thread-safety and
lifetime, crash recovery, the request/protocol path, the client and CLI, the tests and gates, and the
documentation's factual claims).  Each auditor was told: no builds, no edits; every claim needs `path:line` plus a
verbatim quote; label each claim VERIFIED (traced in the code) or ASSUMED; and **do not trust** the ledger, the
review documents or code comments - verify them against the code.  That last instruction is new this round,
because the previous round ended with three of its own write-ups refuted (two claims about the mem backend, and
one open item that a new test disproved).

Baseline evidence, taken before any conclusion:

- local, Windows/MinGW `REGRESS|full|pass=14 fail=0` (running while the audits are written);
- `tools/check-principles.py`: `PRINCIPLES|OK|rules=19 fail=0`;
- XP guest (MSVC 6.0 / SP3): `cemon 5/5`, `kclient 18/18`, `raft 221/221`, `kserver 37/37`, `vfs_fault 5/5`,
  fuzz `done: 200 iterations` / `2 iterations` / `1/1 clusters consistent`,
  `linearizability: 4 histories / 253 ops decided, 0 inconclusive`, crash contract `survive-me`.

## Findings

### 1. The mem backend's lock was reached through a pointer the caller could re-point (found by the CI-red investigation, fixed)

Severity: high.  Introduced by this project's own review-4.2 work; found while checking why two pushes had a red
linux-gate job.

`vfs_mem_read` / `vfs_mem_write` / `vfs_mem_close` took the new spinlock as

    vfs_spin_acquire(&((vfs_mem_ctx *)file->be)->lock);

`file->be` is only the mem backend when the file came straight from `vfs_open`.  The project's own
fault-injection seam (`tests/vfs_fault_test.c`) wraps mem and **re-points every file it opens at itself**, so that
cast locked, and wrote, memory belonging to the wrapper.  MinGW's struct layout made the write harmless, which is
why every local tier stayed green; on the Linux runner the same test hung inside its first case.

Evidence:

- the failing runs `cd4252e` and `1887455` both fail the linux-gate job's "Regression gate (quick)" step, and the
  `regress-logs-linux` artifact shows every other layer green (`cli_smoke: PASS`, `kserver 37/37`, `raft 221/221`,
  `selftest: PASS`, `selftest-cleanup: 0`, `PRINCIPLES|OK|rules=19 fail=0`) while
  `vfs_fault_test.log` simply stops after `BEGIN [1/5] vfs fault backend: no fault injected ...` - no PASS line, no
  SUMMARY: that layer hung;
- `1887455` touches one markdown file, so the failure cannot come from the diff - which is what sent the
  investigation to the toolchain-dependent code paths;
- the fix uses a file-scope instance instead (`static vfs_spin vfs_mem_lock`), so the lock cannot depend on how a
  caller reached the backend.

Lesson recorded in the skill reference: a lock must be reached through the object's own instance, and **a green
local gate is not a green gate** - the pushed CI run has to be read, and the per-layer artifacts are what name the
failing layer.
