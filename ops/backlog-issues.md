# Backlog, posted as GitHub issues

This file is the single source of truth for the issue backlog on GitHub; the issues currently posted
carry exactly these titles and bodies.  `tools/ops/post-backlog-as-issues.py` creates one issue per
`## title` block and skips titles that already exist, so the file can be edited and re-posted safely
(tag `ops/post-backlog-as-issues`).  The card ids in each footer point at the Hermes kanban board
`kynovo`, where the same items are tracked for agent work - the board itself is internal and stays in
Chinese; everything that appears on GitHub is English.

## Pin down G1: gcc 16.2 reports 17 `ready.*` may-be-uninitialized sites that 15.2.0 does not

**Evidence**: the `ci-logs` branch, `ci-logs/build.log` - CI's gcc 16.2.0 reports 17 `'ready.*' may be used uninitialized` sites in `tests/raft_test.c`; the pinned 15.2.0 reports none.

Two very different causes, neither established yet:

* (a) an `-O2` optimizer false positive (the tests `memset` the struct before passing the pointer);
* (b) `raft_advance(..., &out)` does not write every field of the ready bundle - which, under this project's "ready must be complete" contract, would be a real `raft.h` gap (any state the application needs must arrive through `raft_ready`, never through `raft_inspect`).

**Acceptance**: compile the same site under all four combinations (`-O0`/`-O2` x gcc 15.2.0/16.2.0), record the verdict in `doc/gaps-audit.md` (entry G1), and if it is (b), fix `raft.h` rather than the test.

<sub>labels: `backlog`, `raft`, `ci` &middot; backlog card `t_18a8c63b` (Hermes kanban)</sub>

## C4: a startup failure never says why

**Evidence**: `code/kserver.h:4594-4600,4602,4642,4682` and the `kdbsvr` exit path print only `failed to start server N`; `code/kdbsvr.c:279` prints usage without a reason. Config parse, WAL metadata, allocation, thread creation and store open all collapse into the same sentence.

**Why this is not theoretical**: on the first nightly run the server refused to start and that one line was the only evidence available; the actual cause (a harness with a hard-coded store path, invalid on CI) had to be reconstructed from elsewhere.

**Acceptance**: every startup failure path prints its specific cause, and a minimal failing case demonstrates it.

<sub>labels: `backlog`, `observability`, `ci` &middot; backlog card `t_469a3143` (Hermes kanban)</sub>

## C7: unbounded `sprintf` into `char text[2048]` (INFO/STATS)

**Evidence**: `code/kserver.h:3254,3258,3260,3262` build the INFO/STATS text with unbounded `sprintf` into a fixed `char text[2048]`; adding one more field overflows it.

**Acceptance**: bounded writes (track the remaining space, or truncate explicitly), plus a demonstration that adding a field cannot overflow.

<sub>labels: `backlog`, `observability` &middot; backlog card `t_315a3c62` (Hermes kanban)</sub>

## C11: `cemon_ingress_close` spins without a bound; `cemon_destroy` may leak a held socket

**Evidence**: `code/cemon.h:1047-1066` (`cemon_ingress_close` spins with no exit condition); `code/cemon.h:2900-2910` (`cemon_destroy` caps its drain at 2000 ms and can leave a socket it still holds).

**Acceptance**: the spin has an exit condition and a visible warning; destroy leaves no held socket; both demonstrated with a reproducible slow-peer scenario.

<sub>labels: `backlog`, `transport` &middot; backlog card `t_f9bf6d96` (Hermes kanban)</sub>

## C12: UDP soft errors are swallowed silently; the UDP fallback reads the wrong buffer

**Evidence**: `code/cemon.h:2189-2194` discards UDP soft errors without any counter; `code/cemon.h:2207-2213` reads `recv->buf` where `udp_recv->buf` is meant.

**Acceptance**: soft errors are counted or logged, the correct buffer is used, and the fix comes with triggerable evidence - or the branch is documented as unused, with the reason, instead of being left ambiguous.

<sub>labels: `backlog`, `transport` &middot; backlog card `t_ed19113c` (Hermes kanban)</sub>

## C13: loop-level fatal errors report only -1; the cause cannot be retrieved

**Evidence**: `code/cemon.h:3170,3269-3275` - the loop returns `-1` on fatal errors and offers no way to learn what happened.

**Acceptance**: an accessor for the last error (Winsock code or errno) exists and the fatal paths print it, consistent with C4's rule that a fatal must name its cause.

<sub>labels: `backlog`, `transport`, `observability` &middot; backlog card `t_933e252c` (Hermes kanban)</sub>

## The non-Windows branches have never been compiled (including the Unix semantics behind C9/C10)

**Evidence**: the Unix `#else` branches in `code/cemon.h` have never been compiled in this project. Two known problems live there:

* **C9** - `code/cemon.h:2452-2461,3432-3435`: one arm can deliver `CEMON_DATA` more than once, contrary to the contract in the header; with `callback_depth==0` the callback is re-entered synchronously.
* **C10** - `code/cemon.h:2455,2395,2342`: the Unix recv/accept/flush loop does not consume a dispatch budget, so a single busy peer can monopolize the owner thread.

**Acceptance**: first make the Unix branch compilable (a minimal harness, or a documented reason why it cannot be verified in this environment), then address C9/C10. No "fixed" claim without compile evidence.

<sub>labels: `backlog`, `platform` &middot; backlog card `t_082e24b2` (Hermes kanban)</sub>

## R8: a rejected AppendEntries can propagate a not-yet-persisted term

**Evidence**: `code/raft.h:1277-1278` -> `code/raft.h:3660-3671`. Rejecting an AppendEntries can expose a new term before it is durable; the vote path and the successful-AE path both have a durable gate, this one does not.

**Acceptance**: align the change with the corresponding paragraph of the semantic baseline (Ongaro's dissertation, referenced as `doc/dissertation.md`) and say which paragraph it follows; then produce two independent lines of evidence (unit suite + cluster fuzz).

<sub>labels: `backlog`, `raft` &middot; backlog card `t_07585855` (Hermes kanban)</sub>

## C6: layering - the application layer reads raft internals for policy decisions

**Evidence**: `code/kserver.h:4546-4548` reads `raft->config_joint`, `raft->config_new.ids` and `raft->config_learners.ids` to decide policy.

**Rule**: mechanism versus policy - the boundary decides who owns the responsibility (`treap.h` established this for the store; `raft.h` owns the same distinction for consensus).

**Acceptance**: `raft.h` reports what the application needs (through the ready bundle or a query interface) and the application stops reading internal fields. `tools/check-principles.py` budgets this at 3 sites; the budget must come down, never up.

<sub>labels: `backlog`, `raft` &middot; backlog card `t_972b67e8` (Hermes kanban)</sub>

## C3: WAL segment-first-record generation continuity is unchecked

**Evidence**: `code/kserver.h:1465` vs `1478` - `prev_gen=0` is reset for every segment, so continuity of the generation across segments is never verified and a mismatched segment is not detected.

**Acceptance**: define and enforce cross-segment generation continuity (or argue in the documentation why it is unnecessary), and state how a corrupt segment is detected and reported.

<sub>labels: `backlog`, `storage` &middot; backlog card `t_39285829` (Hermes kanban)</sub>

## `lincheck` is in no build target (the linearizability check never runs)

**Evidence**: `tests/lincheck.py` and `tests/lincheck.h` exist, but no `build.sh` target references them, so the linearizability check has never run as part of a regression.

**Acceptance**: add a build/run target and wire it into the appropriate gate in `doc/testing.md` (or record why it is deliberately excluded), and produce one real verdict line from it.

<sub>labels: `backlog`, `tests` &middot; backlog card `t_9f03e69c` (Hermes kanban)</sub>
