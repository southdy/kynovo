# Plan: onboarding GitHub and Hermes Agent for automated development and maintenance

**Status**: decided and being executed —— see §6 for the decisions (settled), P0 complete, P1 in progress. **Basis**: the measured verification in §1 of this file + the authoritative Hermes official skill (`hermes-agent`) on
project context files and background systems. **Precondition for execution**: the five decision points in §6 must be decided first.

---

## 1. Current-state analysis (measured, not impressions)

### 1.1 Favorable conditions
| Item | Fact |
|---|---|
| Dependencies | **Zero external dependencies**: single-header library + MinGW/ws2_32/winmm. Only `tools/cov_report.py` and `tests/lincheck.py` need Python |
| Line endings | Spot-checked `build.sh`/`code/kbase.h`/`code/raft.h`/`tests/selftest.c`/`doc/testing.md` are all **LF** (meets the established requirement) |
| Credentials | No password/key/token found anywhere in the tree (the hits in `cli.h` and `dissertation.md` are ordinary words) |
| Artifacts | Binaries and command outputs all live under `build/` (now a pure artifact directory, gone after `clean`) |
| Docs | Six items in `doc/`: `dissertation` (semantic baseline), `gaps-audit` (gaps/backlog), `testing` (gates and method), `investigations` (historical investigations), `code-review-2026-09` (review), `measurements` (178 raw records) |
| Tooling | `git 2.28` and `python 3.11.16` available |
| Gates | Already layered, with a verdict-line convention (`doc/testing.md`); key gates currently all green |

### 1.2 Blockers (ordered by impact on GitHub/automation)
| # | Blocker | Evidence | Impact |
|---|---|---|---|
| B1 | **`doc/dissertation.md` is a third-party paper** (the full text of Ongaro's PhD dissertation) | 8k lines, not this project's copyright | **Must not enter a public repo**; recommended to move it out of a private repo as well |
| B2 | **No LICENSE / no README** | the root holds only `build/ build.sh code/ doc/ tests/ tools/` | GitHub landing page and compliance missing |
| B3 | **28 files hardcode `D:/kynovo`** (data paths, e.g. `disk://D:/kynovo/build/...`) | `grep -rl` counts 28 | No off-machine/CI environment can run the data-bearing scripts |
| B4 | **`python3` on this machine is the WindowsApps stub** (Permission denied) | measured | Scripts/CI must use `python` or probe for it |
| B5 | **Runs only on Windows** (5 headers include `windows.h`/`winsock2.h`; the non-Windows branch has never been compiled) | `grep -lc` | CI must use `windows-latest`; no cross-platform claim may be made |
| B6 | **No version control** (no `.git`) | `ls -a` | No rollback, no diff, no collaboration——**the highest-value-for-effort fix** |
| B7 | No "one-shot gate" entry point: a full regression means typing 8 commands by hand | `doc/code-review-2026-09.md` T-1 | Automation and cron lack a stable entry point |
| B8 | No machine-readable result: the verdict relies on a human reading the output | same as above | The agent cannot reliably assert "pass/fail" |

---

## 2. GitHub onboarding preparation

### 2.1 Repository shape
- **Public** (settled - see D1 below); `main` is the only long-lived branch.  Direct pushes to `main` are allowed, so the discipline is `./build.sh regress quick` immediately before every push (see AGENTS.md); the nightly job is the safety net, not the first line.
- Commit messages: first line ≤72 characters, imperative; the body states **why** and **how it was verified** (which gates, verdict-line results). Keep the debugging process out of history.
- Tagging policy: `v0.x.y` (no production use yet, so 0.x for now).

### 2.2 Required files (deliverables of this plan, produced during execution)
| File | Content outline |
|---|---|
| `.gitattributes` | `* text=auto eol=lf`, `*.md text eol=lf`, `*.c/*.h text eol=lf`, `*.exe binary` —— enforce LF (established requirement) |
| `.gitignore` | `build/`, `*.exe`, `*.log`, `*.o`, `*.gcov`, temporary data-disk directories; **do not ignore** `doc/measurements/`, `tools/` |
| `README.md` | 20–40 lines: what the single-header Raft KV is, the Windows XP+ and MSVC6/C89 constraints, dependencies, table of `./build.sh` targets, minimal run example (`init` → `server` → `kdbctl SET/GET`), pointer to `doc/testing.md` |
| `LICENSE` | **Apache-2.0** (official full text; consistent with the copyright line in the README) |
| `AGENTS.md` | see §3.1 (it is also Hermes's project-context entry point) |
| `.github/workflows/ci.yml` | see §2.3 |
| `.github/pull_request_template.md` | requires: what changed, **which gates were run**, verdict-line results, risk and rollback method |

### 2.3 CI design (Windows runner, layered by "minute budget")
| Stage | Content | Budget | Trigger |
|---|---|---|---|
| build | `./build.sh` + **0 error/0 warning assertion** | ~2 min | every push/PR |
| unit | `raft_test` / `kserver_test` / `kclient_test` / `cemon_test` / `selftest` | ~1 min | same as above |
| smoke | `tests/cli_smoke.sh` (real processes + real socket) | ~10 s | same as above |
| fuzz-fast | `raft_fuzz 1 2000`, `raft_cluster_fuzz 1 200 0`, `kserver_cluster_fuzz 1 1` | ~1 min | same as above |
| nightly | `raft_cluster_fuzz 1 20000 0` (splittable, parallelizable), `soak_release.sh RUNS=24`, `./build.sh coverage`, `./build.sh run-sanitize` | ~20 min | daily cron |
- Toolchain: MSYS2 + `mingw-w64-x86_64-gcc` (**do not pin 15.2.0**; record the actual version in CI; locally still pinned to 15.2.0). If some version lacks `LPFN_ACCEPTEX` (a known issue in 9.2.0), CI exposes it immediately.
- Artifacts: nightly uploads `build/*.exe` and `build/perf/` logs as artifacts; `doc/measurements/` is archived by hand on the local machine, CI does not write to the repo.
- Security: `GITHUB_TOKEN` read-only; pin Actions to SHA where possible; no secret requirements at all (zero dependencies, zero external services).

---

## 3. Hermes Agent automation preparation

### 3.1 Project context file: `AGENTS.md` (single, cwd-only)
Authoritative convention: `.hermes.md` (can be inherited upward beyond the git root) and `AGENTS.md` (**cwd only**, portable to Codex/Claude Code/OpenCode)
**First match wins**. This project is flat and should be usable by other agents too ⇒ choose **`AGENTS.md`**, keep the content within 20,000 characters, and reference `doc/testing.md` for details.

Suggested skeleton (written to file during execution):
```
# kynovo — agent working notes
## Hard constraints (violation means rejection)
- C89 / MSVC 6.0 compatible / Windows XP+; C99 facilities forbidden (inline, VLA, stdint.h, long long literal suffix ULL)
- Line endings always LF; never hand-write any file into build/ (pure artifact directory)
- Do not change the contracts of raft.h/treap.h without first aligning with the paper semantics in doc/dissertation.md and saying so
## Workflow
1) run the baseline gates before changing; 2) run `./build.sh regress` after changing (all gates + verdict lines + machine-readable summary);
3) reports must carry evidence (verdict line / log path / exit code); "should be fine"-style conclusions are forbidden
## Discipline (bought with real cost in this project)
- Confirm the build succeeded before interpreting run results (three times a stale binary led to wrong conclusions)
- A/B within the same build; for low-probability defects use dozens of samples or a long-run gate; one clean run is not evidence
- Multi-invariant harness failure: find the "first divergence" first; do not reason backwards from the failure point
- Instrumentation (temporary prints/probes) must be reverted; only the fix itself and long-term diagnostics may remain
- Do not bulk-edit control flow line by line; release points must be at contract endpoints; shutdown is always "save first, then clear, then close"
## Key entry points
- Gates and method: doc/testing.md   backlog: doc/gaps-audit.md   harness: tools/harness/
- Historical investigations and conclusions: doc/investigations.md   raw measurements: doc/measurements/
```

### 3.2 One-shot gate: `./build.sh regress` (the stable entry point for automation and cron)
- `./build.sh regress [--quick|--full|--fuzz N]`: executes in the order given in `doc/testing.md` §6;
- **last line prints a machine-readable summary**: `REGRESS|quick|pass=10 fail=0 duration=142s` (the agent only needs to read this line);
- stops on failure and prints the failing gate's **verdict line** and log path; `--full` appends L4/L6;
- one-to-one with the table in `doc/testing.md`, avoiding drift between the two.

### 3.3 Codifying skills
Already present: `kynovo-storage`, `performance-benchmarking`, `debug-instrumentation`, `consensus-fuzz-testing`, `code-audit-remediation`.
Proposed additions/revisions (execution phase):
| Skill | Content | Division of labour with the existing ones |
|---|---|---|
| `kynovo-build-verify` | one-shot gate usage, verdict lines, common false failures (port in use, appverif not disabled, stale binary) | new |
| `kynovo-release` | tagging, build artifacts, updating the `doc/measurements/` baseline | new |
| `kynovo-raft-contract` | paper semantics ↔ code location mapping (`raft.h` contract entries and line numbers) | complementary to `raft-consensus-review` |
> Principle: skills state **process and verdicts**, `AGENTS.md` states **constraints and discipline**, `doc/` states **facts and evidence**; the three do not repeat each other.

### 3.4 Background and automation (designed around the authoritative capabilities)
| Mechanism | Usage | Caveat |
|---|---|---|
| `cron` | nightly: `workdir: D:\kynovo` (auto-loads `AGENTS.md`) + `script` pre-runs `./build.sh regress --quick` to collect data + an agent task interprets it and reports **only on failure** | **3-minute hard interrupt per run** ⇒ long tasks (20k-round fuzz, 24-round soak) must be launched by a script as a **background terminal process** and logged to disk; the agent only reads the logs |
| `cron` chain | `context_from` feeds the "nightly gate summary" into the "next-day review/fix task" | keeps the main session's roles alternating, do not mirror |
| `kanban` | turn `doc/gaps-audit.md` entries into task cards (ID/acceptance criteria/evidence requirements); the orchestrator dispatches, a worker does exactly one card | durable, retryable; either/or with GitHub Issues, or double-write (decision point D3) |
| `delegate_task` | parallel read-heavy, write-light analysis (audits, cross-checking, report writing) | **not durable**: lost as soon as the process exits ⇒ use only for minute-scale subtasks |
| Worktrees | agents writing code in parallel use `hermes -w` (worktree) | avoids same-branch conflicts; hand merges to merge-reconciler |

### 3.5 Acceptance conventions for "AI can be trusted" (written into `AGENTS.md`)
1. Any "done/fixed" claim must carry **reviewable evidence**: verdict line + log path + exit code;
2. High-risk changes (raft semantics, persistence, concurrency) require **two independent pieces of evidence** (e.g. unit test + cluster fuzz both green);
3. A fix must **re-run the very gate that once reproduced the problem**, and write the result into `doc/gaps-audit.md`;
4. Falsified hypotheses must be explicitly retracted (this repo already has that convention; see sections E/F of `doc/gaps-audit.md`).

---

## 4. Phased execution plan (each phase can be delivered/rolled back independently)

| Phase | Output | Acceptance | Estimate |
|---|---|---|---|
| **P0 Version-control baseline** ✅ complete | `git init`; `.gitattributes`, `.gitignore`; move `doc/dissertation.md` out (document how to obtain it instead); first commit | `git status` clean; `grep -c dissertation .gitignore` hits; after `./build.sh clean && ./build.sh`, `git status` is still clean (proving `build/` is ignored) | 0.5 day |
| **P1 Automatable** (`AGENTS.md` ✅, `regress` ✅; remaining: data-path parameterization, `python` probing) | `AGENTS.md`; `./build.sh regress` (with the `REGRESS|` summary); unified `python` probing; data-path parameterization (`KDB_DATA`, defaulting to `build/data`) | `./build.sh regress --quick` green with a parseable last line; a copy in a directory other than `D:\kynovo` also runs (proving there is no absolute-path dependency) | 1 day |
| **P2 GitHub + CI** (`ci.yml`/`nightly.yml`/PR template ✅ ready; pending remote and authentication) | private repo; `ci.yml` (build/unit/smoke/fuzz-fast); PR template; nightly workflow | a PR triggers an all-green CI; deliberately introduce one warning ⇒ CI red (proving the assertion works) | 1 day |
| **P3 Automation closed loop** | the three skills (§3.3); cron nightly gates + report-only-on-failure; kanban/Issues aligned with the backlog | 3 consecutive nightly tasks report only on failure as expected; one backlog card goes the whole way through "dispatch → change → regress → PR" | 1 day |
| **P4 Steady-state maintenance** | backlog-driven: each task = branch → change → `regress` → PR → report (with evidence) | weekly review: the green/red trend of the gates, whether `doc/measurements/` was updated, whether the skills need revision | ongoing |

**Risks and mitigations**
- Once CI is introduced, the local and CI gcc versions differ ⇒ CI records the actual version; locally the pin stays, and failures caused by the difference are investigated as a "toolchain difference" first.
- Automated agents changing persistence/concurrency code carries regression risk ⇒ from P4 on, enforce "branch + PR + two pieces of evidence + nightly gate".
- Parameterizing data paths touches 28 scripts ⇒ do it in one pass and give each one `bash -n` + at least one run (the scripts can already be run bare).

---

## 5. Relationship to the existing documents (avoiding duplication)
- `doc/testing.md`: gates and method (**single authority**)——the `regress` implementation must match it;
- `doc/gaps-audit.md`: backlog and evidence (every fix updates its status and retraction record here);
- `doc/investigations.md` + `doc/measurements/`: history and raw data (verifiable without re-measuring);
- `doc/code-review-2026-09.md`: structure and hygiene review (upstream of this plan);
- `AGENTS.md`: **constraints and discipline for agents** (shortest, hardest, portable).

## 6. Decision points (to be executed once decided)
| # | Decision | Options and impact |
|---|---|---|
| D1 | Repo visibility | **Public** (settled) ⇒ `doc/dissertation.md` stays in place and is `.gitignore`d (references stay valid, it is never committed) |
| D2 | CI scope | **Full + nightly** (settled) |
| D3 | Backlog carrier | **Double-write** (settled): GitHub Issues for humans + Hermes kanban for agents |
| D4 | Agent permissions | **Direct pushes to `main` allowed** (settled) ⇒ compensating measures: run `./build.sh regress quick` locally before every push; nightly is the safety net (written into `AGENTS.md`) |
| D5 | License | **Apache-2.0** (`LICENSE` already in place, official full text taken verbatim from apache.org): permissive like MIT, plus an **explicit patent grant** and a patent-retaliation clause; requires retaining copyright/notices and marking modifications |

---

## 7. Environment constraint: the dev machine is often off (this plan is adjusted accordingly)

**Constraint (stated by the maintainer)**: this Windows machine is not online long-term——it is not here during working hours and is usually not switched on.
This invalidates an implicit premise of this plan's earlier version: **anything that depends on this machine being resident cannot go on the critical path**.

### 7.1 Things that cannot serve as infrastructure (local side)
| Component | Why it does not hold up |
|---|---|
| Hermes **cron** scheduled tasks | the scheduler is hosted by the gateway, and the gateway only runs after login on this machine; tasks coming due while the machine is off are **missed outright** (there is no catch-up run) |
| Hermes **kanban dispatcher** | likewise ticked by the gateway once a minute; without the gateway, cards **sit in `ready` forever** |
| local **`regress full`** long gate (20 minutes) | requires someone to actually keep the machine on; fine as a "run while present" gate, not as a nightly gate |
| local **0-warning assertion on the pinned toolchain** | only the local machine can run it (CI is a foreign compiler, see §G2)——this is a check that **can only be done locally**, so it is kept as a "while present" gate |

### 7.2 Plan: **move the periodic/verification work to the cloud, leaving the local machine for interactive development only**

| Work | Owner | Trigger | While the machine is off |
|---|---|---|---|
| nightly full gates (20k fuzz / 2000 cluster / 24-round soak / coverage / UBSan) | **GitHub Actions** | cron `17 18 * * *` (02:17 CST) | **runs as usual** ✓ |
| per-push/PR fast gates (build+unit+smoke+fast fuzz) | **GitHub Actions** | `push` / `pull_request` | runs as usual ✓ |
| run the full gates in the cloud on demand | **GitHub Actions** | **push a tag (`v*`)**——over SSH, **needs no token at all** | runs as usual ✓ |
| failure evidence | **GitHub branches** | on failure, push `ci-logs` / `ci-logs-nightly` (publicly `git fetch`-able) | available as usual ✓ |
| failure → **todo item** | **GitHub Issues** | the failing workflow uses its built-in `GITHUB_TOKEN` to open/update an issue automatically (no personal token needed) | available as usual ✓ |
| interactive development, qualitative investigations, strict gates that need the pinned toolchain | **local machine** | driven by Hermes while someone is present | does not run (**not on the critical path**) |

This way "the machine is often off" affects only **development speed**, not **verification and record-keeping**: the cloud runs every day, a red light automatically becomes an issue, and the evidence lands automatically on a public branch.

### 7.3 Degraded settings on the local side
- The local daily gate watch task (cron) is kept, but **downgraded to best-effort**: switched to `monitor` change detection (the agent is woken only when the cloud run state **actually changes**, with no cost otherwise), and the frequency raised to every 2 hours——it self-checks shortly after boot, and while the machine is off it merely misses runs without piling up.
- **Do not** treat the gateway as part of the critical path: installing it (autostart on login) makes "while present" automation smoother, but every key conclusion already has a cloud source.

### 7.4 Known cloud limitations (recorded honestly)
- **Scheduled workflows in a public repo are automatically disabled after 60 days of repo inactivity**; any push resets that timer, and they can also be re-enabled by hand on the Actions page.
- The cloud uses a foreign toolchain (currently gcc 16.2.0) ⇒ **the 0-warning contract is enforced on the pinned toolchain only** (see `doc/gaps-audit.md` G2); new findings from the **foreign toolchain** in the cloud (such as G1) are signals, not verdicts.
- Actions logs and artifacts are **not readable anonymously** (403) ⇒ failure evidence always goes through public branches + the issue body, which is also the reason for the two design choices above.
