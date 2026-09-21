# kynovo engineering principles (and how they are enforced)

> **The criterion for this file**: a principle only really exists if **violating it fails loudly**; anything that
> lives only in prose or comments and relies on "remember not to do that" is filed in the **danger list**
> in §7.

Mechanical check: `python tools/check-principles.py` → `PRINCIPLES|OK|rules=21 fail=0` (a non-zero exit is a violation).
A ratchet rule keeps its budget in ONE variable and derives both the failure threshold and the message from
it, and it prints the measured count next to the budget - the earlier form kept the number in the message
and a different number in the slice, so the message walked down while the threshold stayed put.
It is wired into `./build.sh regress` (one `GATE|principles|…` line per tier), so it **runs both locally and in CI**.
Design constraints (all of them scar tissue): **comments must be stripped** (otherwise prose such as "the worker inline"
would be a false positive, and a checker that reports false positives ends up being ignored); `long long` is
**not on the ban list** (this repo deliberately uses `unsigned long long` to define 64-bit types, see §7-1); for
**known and already-known** violations use a **budget (ratchet)**: it may only shrink, never grow, and the message
names the corresponding card; CRLF under `doc/measurements/` is **explicitly exempt** —
that is captured archival evidence, and reflowing it destroys it.

## 1. Language and platform (hard constraints)

| Principle | Why | Code location | Current enforcement |
|---|---|---|---|
| C89: no `inline`, no VLA, no `stdint.h`, no declarations after statements | the targets include MSVC 6.0 and Windows XP | `code/*` | **Mechanical**: checker (bans `inline`/`stdint.h`) + compile gate `-std=c89 -Wdeclaration-after-statement` |
| No `ULL` literals | MSVC 6 does not recognize them | `code/*` | **Mechanical**: checker (`tests/`, `tools/` exempt — they are compiled by gcc only, and the message says so) |
| 64-bit types/literals/print formats always go through the macros each header carries (`k_u64`/`vfs_u64`/…, `*_U64_C`, `*_U64_FMT`) | MSVC 6 has no `long long`, and its printf only understands `"I64d"` | every header | **Mechanical**: the checker asserts that all five headers contain both the `__int64` and the `long long` form, and that `code/` has no bare `%lld`/`%llu` |
| Use only XP-and-earlier Win32 APIs | the target is XP+ | `code/*` | **Mechanical**: checker blacklist (`GetQueuedCompletionStatusEx`/`GetTickCount64`/`CreateFile2`/…); an explanation inside a comment does not count |
| Line endings are always LF | the scripts must run under bash; `sed`/`grep` behave consistently | whole repo | **Mechanical**: the checker asserts the **repo index** `i/crlf = 0` (actual repo storage ✓) + scripts `w/crlf = 0`; CI has a separate LF step |
| Archival evidence is not reflowed or "normalized" | reproduction judgements depend on the original record | `doc/measurements/` | Discipline + the **exemption** above is the enforcement (the CRLF records are kept on purpose) |

## 2. Directories and artifacts

| Principle | Enforcement |
|---|---|
| `build/` is a pure artifact directory, starts empty, **must never contain hand-written files**, and `clean` deletes it wholesale | **Mechanical**: checker (`git ls-files build/` must be empty) |
| Hand-written material each has its home: reusable harness → `tools/harness/`; one-off investigation → `tools/archive/` (conclusions go into `doc/investigations.md`); raw records → `doc/measurements/` | Discipline (code review gatekeeps where a new script lands) |
| The repo root inside a script **must be self-derived**, never hard-coded | **Mechanical**: checker (no drive-letter paths in tracked `*.sh`; URI prefixes such as `disk://` do not count) |

## 3. Contracts and ownership

| Principle | Why | Enforcement |
|---|---|---|
| **`ready` must be reported in full**: all application-layer state comes from the ready returned by `raft_advance`, and `raft_inspect` / `treap_inspect` is **for the observation endpoint only** | otherwise the observation interface leaks into the decision path | **Mechanical ratchet**: budget for `raft_inspect` / `treap_inspect` sites in the application layer and the tests = **1** (the only legal site = the STATS endpoint, `kserver.h:3433`); growth is an error |
| The snapshot view is **read-only for non-owner threads**; `capture` and `finish` must run on the owner thread | while the snapshot thread stream-reads, the main thread is still mutating the treap | Discipline (the `treap.h` header comment states the invariant) |
| The free point must be the **contract endpoint**, not "the place where the data was copied" | once a request has been handed to raft it is still that request's cookie and payload | Discipline + regression (single free point, `k_request_free`) |
| At every close site: **save first, clear second, close third** | once cemon has closed, the object is void; a leftover handle = a delayed UAF | Discipline + crash-class regressions |
| **Lose no request**: any path that accepts a request must end in a reply or a visible log | silent drops once hung clients forever | Partly mechanical (`selftest`/`cli_smoke` regressions) |
| **fatal must print the reason** | a silent exit was once taken for "it did not happen" | ✓ enforced by `k_open_fail` (`code/kserver.h:5395`) |
| Transaction replicas must not call `capture`/`save`/`load` | the copy semantics of the treap | Discipline (`treap.h` comment) |
| Do not change the `raft.h`/`treap.h` contracts without first aligning with the paper's semantics | the semantic baseline is Ongaro's paper | **Advisory-mechanical**: the checker prints a NOTE when it detects that either of those two files changed (the message must name the paragraph it relies on) |

## 4. Layering (mechanism layer vs. policy layer)

| Principle | Enforcement |
|---|---|
| The mechanism layer is responsible only for **legal input and usage**; preconditions are guaranteed by the caller (`treap.h`: the seed is decided by the layer above; `treap_load` does not validate ordering/duplicates) | Discipline |
| The application layer **must not read** raft internal fields (`config_new`/`config_joint`/`config_learners`) directly to make policy decisions | **Mechanical ratchet**: budget = **0** (the three original sites are gone; the ready bundle carries the facts) |
| Fault injection goes only through the **four seams the architecture already has** (transport vtable / `elapsed_ms` / runtime backend / vfs backend); no new intrusive hooks | Discipline |
| Test code lives in `tests/` and does not intrude into production code | Discipline |

## 5. Concurrency and lifetime

| Principle | Enforcement |
|---|---|
| Reference counts are **attributed only on a successful commit** | Discipline |
| Never free/rewrite shared state on a non-owner thread | Discipline (root cause of crash class ③) |
| Clear the handle immediately after closing (`void *dead=h; h=0; close(dead);`) | Discipline + regression |
| A multi-threaded debug allocator must be locked | Discipline (the instrumentation hurt itself once; it is locked now) |

## 6. Measurement and evidence

| Principle | Enforcement |
|---|---|
| **"0 failures" is not "0 data"**: the rig must prove itself (assert a positive signal, `ok=<N>`/`done:`/liveness) | **Mechanical**: the verdict-line mechanism of `regress` (no verdict line = FAIL) + `tools/harness/regress_selftest.sh` |
| Single-source trust: a `PHASE` line is not evidence ⇒ you need `PROBE`/`RESP` accepted + the commit advanced + a read-back | Discipline (the `kynovo-storage` skill) |
| **Same-build A/B**, never compare across builds | Discipline |
| A single clean run is not evidence (for low-probability defects use dozens of samples or a long-run gate) | Discipline |
| When a multi-invariant harness fails ⇒ **find the first divergence**; do not work backwards from the failure point | Discipline (`AGENTS.md`) |
| Instrumentation must be reverted; temporary prints must carry a **self-identifying tag** | **Mechanical**: the checker bans leftover tags (`TEMP-INSTR`/`TEMP-AB`/`INSTR-<X>`/`XXX-`/`HACK-`) — an explanation in a comment does not count, **one in code does** |
| A falsified hypothesis must be **explicitly retracted** (written into `doc/gaps-audit.md`) | Discipline (that is the point of that file) |

## 7. Danger list: principles left to discipline alone (no mechanical enforcement)

These are the class that is **easiest to break by mistake** ✓. Ordered by risk, with optional directions for mechanization:

1. **(formerly §7-1 "MSVC 6 × `long long` was never validated" — ✗ retracted, see below)**
   I once asserted on this basis that "MSVC 6.0 compatibility was never validated at the toolchain layer" ✗.
   **This was a false positive**: I grep'd only the `#else` branch of `typedef unsigned long long …` and drew the
   conclusion without reading its conditional compilation ✗. In fact **the repo has had a cross-platform 64-bit
   facility all along**, and every header carries its own:

   | Header | 64-bit type (`_MSC_VER` / other) | Literal macro | Print macro |
   |---|---|---|---|
   | `kbase.h:27-42` | `signed/unsigned __int64` / `long long` | `K_I64_C`/`K_U64_C` | `K_U64_FMT` (`"I64u"` / `"llu"`) |
   | `vfs.h:63-67` | `unsigned __int64 vfs_u64` / `unsigned long long` | `VFS_U64_C` | — |
   | `cemon.h:13` | `unsigned __int64 cemon_u64` / `unsigned long long` | (no literal macro yet) | — |
   | `raft.h:92-107` | `raft_u64`/`raft_i64`, both branches | `RAFT_U64_C`/`RAFT_I64_C` | `RAFT_U64_FMT`/`RAFT_I64_FMT` |
   | `treap.h:113-116` | `treap_u64`, both branches | `TREAP_U64_C` | — |

   And `raft.h:99` carries **a principle written explicitly as a comment**: *"MSVC 6.0 has no `long long`, so its printf spells a 64-bit
   conversion "I64d"; gcc spells it "lld". Never write `%lld` literally."*

   **The real principles that follow from this (mechanized, 2 rules)** ✓:
   - every header that defines a 64-bit type **must** contain the `_MSC_VER` branch and both the `__int64` form and the `long long` form ✓;
   - bare `%lld`/`%llu` are **banned in `code/`**; they must go through that header's `*_U64_FMT` ✓.

   **This correction did in fact hit one real violation** ✗: three lines in `code/kdbctl.c` wrote `%llu` directly
   (with the argument hard-cast as `(unsigned long long)` ✗), and have been changed to `%" K_U64_FMT "` + `(k_u64)` ✓ —
   i.e. "the principle really was broken by mistake", except that what broke it was **not the typedef but the format string** ✓.

2. **Snapshot view ownership** (capture/finish on the owner thread): a direction for mechanization = add an `owner_thread` assertion in `treap.h`.
3. **fatal must have a reason** (C4): a direction for mechanization = a check that "there must be a printf near every `exit(1)`" (noisy, deferred).
4. **Lose no request**: a direction for mechanization = insert a self-identifying tag in every "accept and drop" branch, then have a harness assert zero hits.
5. **Same-build A/B / a single clean run is not evidence / find the first divergence**: methodology, not mechanizable — but they are already written into `AGENTS.md` and the skills,
   and every review enforces them.

## 8. Rules for changing this file

When adding a mechanical rule: **first prove that it can fail** (inject one real violation → watch it go red → revert
precisely → the leftover count is 0), then wire it into `regress`. A rule that cannot "fail" should not be added —
putting it in §7 is the more honest move.
