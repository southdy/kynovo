# kynovo engineering gaps ledger (audit artifact)

Audit method: 3-way deep module audit (raft.h ↔ dissertation; cemon/runtime/vfs platform and mechanisms; kserver/kclient/treap/kproto/app)
+ independent verification by me on the same tree. **Verification status is marked per item**:
- `[verified]` = I read the code at that location and confirmed the conclusion holds
- `[unverified]` = given by the audit report, not yet independently confirmed by me (file:line cited for review)

The checked and **gap-free** parts and the **settled trade-offs** are at the end.

---

## A. High severity

### A1 `[verified]` The FCALL gate can be closed permanently → all later write requests queue forever, get no reply, and are not redirected
- Evidence: `code/kserver.h:2758` (`gate_closed=1` before a flush that may fail); cleared only at `2772` (FCALL succeeded) and `2814` (lost leadership);
  when a client disconnects, the FCALL request is `k_request_free`d at `3535-3551` and the **gate does not open**.
- Impact: after that connection drops, `2830` sends all non-read requests into `gate_head` to queue forever ⇒ the client hangs permanently (compounded by A2),
  and `gate_head` has no length cap and `request_count/request_bytes` are never released ⇒ admission refusals are eventually triggered.
- Related: `code/kserver.h:4236` (A5) means a paused connection is never woken either.
- Direction: when a connection disappears and one of its requests holds the FCALL, it must run the same teardown as `k_server_fcall_fail` (open the gate + reply/visible log).

### A2 `[verified]` The client has no timeout at all for "a request already sent" (only redirect/reconnect are bounded)
- Evidence: `code/kclient.h:109,392` (the only bounded mechanism, `K_CLIENT_RETRY_MS`, applies only to unknown leader/reconnect);
  `376-409` (`k_client_poll` handles only retry/reconnect).
- Impact: once the server silently loses a result (any of A1/A3/B1/B4), the client waits forever; a non-empty `pending` makes every later command be refused
  (`kclient.h:284-287` returns 0 without queuing) ⇒ the whole session dies.

### A3 `[verified]` `kdbctl` one-shot mode returns `EXIT_SUCCESS` on timeout (false success)
- Evidence: `code/kdbctl.c:503` (3s timeout break); `516` (only a `cemon_poll` failure sets `rc=-1`); `528` (`return rc`); `548` (`rc==0?EXIT_SUCCESS`).
- Impact: `kdbctl <host> SET k v` prints nothing and exits 0 when the server does not respond; scripts/CI take the failure for a success.

### A4 `[verified]` cemon allows cross-thread `cemon_send/cemon_recv`, but the send queue and quotas are completely unlocked (data race)
- Evidence: `code/cemon.h:238-244` (the header states explicitly "other threads may still post and send while the mailbox is open");
  `2749-2759` (the list/`send_cost` are mutated outside the brief lock held in `cemon_socket_admit`); concurrent counterparts `2238-2240,2364-2366,1133-1145`.
- Impact: a constructible interleaving can make a queued frame unreachable ⇒ never sent + quota leaked forever ⇒ once the cap accumulates, every `cemon_send` returns -1.
  Nothing in the repo currently sends across threads (owner thread only) ⇒ this is a **latent** defect, a **contract/implementation mismatch**.

---

## B. Medium severity

### B1 `[verified]` The RGET flush-failure path still "frees the request and does not reply"
- Evidence: `code/kserver.h:3071-3074` (compare with the fixed read path `2846-2852`; here it is still `k_request_free` + `return -1`).
- Impact: the client only sees a disconnect and gets no status code; other in-flight requests on that connection are lost with it.

### B2 `[verified]` A WAL write failure is treated as "graceful shutdown": exit code 0 + prints only `stopped`
- Evidence: `code/kserver.h:4186-4188` (`!wal_ok` → `poll_wal` returns -1) → `4496` (`drive()!=0` → `begin_stop`, **no fatal set**) →
  `kdbsvr.c:248` (`if(!stopped||fatal) rc=-1`) → `kdbsvr.c:320-321` (prints `stopped`, `EXIT_SUCCESS`).
- Impact: a disk/IO write failure looks like a clean exit to a supervisor, with no reason line (conflicts with the ops standard that "fatal must print a reason").
- Note: fail-stop on a full disk is itself a trade-off you have accepted; this item is a **exit-semantics and visibility** problem.

### B3 `[verified]` `k_client_queue` returns 0 ("success") when a pending request exists, but does not queue
- Evidence: `code/kclient.h:284-287`. Callers (kdbctl's 22 command handlers) only check `!=0`;
  the retry logic relies on `strstr(g_cli_last,"wait for the current request")` matching **user-visible text** (`kdbctl.c:499,505`).
- Impact: the command is silently dropped; any rewording of the text breaks CLI behavior.

### B4 `[verified]` Admission/limit-triggered behavior is invisible and silently harms connections
- Evidence: connection cap → immediate close with no log `code/kserver.h:3320`; receive buffer over limit → immediate close and that connection's in-flight requests get no reply `3336-3339`;
  the request-level limit reuses the `"out of memory"` text `1998` + `2835/2745`.
- Impact: a client cannot tell "capacity protection" from "peer OOM"; connection-level refusals are entirely unobservable.

### B5 `[verified]` `cemon_recv` leaves `recv_armed=1` behind after an arming failure, with no re-post path
- Evidence: `code/cemon.h:3426-3429` (sets `recv_armed=1` first, then posts; when `cemon_win_post_recv` fails, `busy=0` and no re-post happens;
  the only automatic re-post point is connect completion `2173`).
- Impact: `cemon_recv_active()`/the stats lie; if the application does not retry, the connection silently goes dead (this session already hit the same symptom).

### B6 `[verified]` Windows `cemon_poll_once` reads an uninitialized `key` (UB; can swallow a fatal error as a wake)
- Evidence: `code/cemon.h:3152-3171` (`ULONG_PTR key;` is passed to `GetQueuedCompletionStatus` uninitialized;
  MSDN: on failure with `lpOverlapped==NULL`, `lpCompletionKey` is not written).

### B7 `[verified]` The non-Windows (epoll/kqueue) branch **has never been compiled**, and contains C89 violations
- Evidence: `code/cemon.h:2459` (`int emit_rc=...` after a statement), `2621` (`int err=...` after a statement);
  `build.sh` is Windows builds only (no Makefile/CI).
- Impact: the "Unix platform" within this audit's scope is in fact unverifiable code; it fails the repo's own `-Wdeclaration-after-statement`.

### B8 `[verified]` Snapshot "verify after write" does not cover the whole file (only the 4-byte trailer + an end probe)
- Evidence: `code/kserver.h:3910-3925` (the CRC source is the in-memory stream, and the read-back compares only the trailer), `4000` (immediately followed by `raft_snapshot_data_ready`), `4004` (immediately followed by deleting the old snapshot/old segments).
- Impact: silent corruption in the middle does not prevent deletion of the old snapshot, so the rollback material can disappear at the same time; the failure only surfaces at the next start, as fail-stop.

### B9 `[unverified]` The recovery base-fallback path has a branch where "the base is forced high yet decoding still uses the newest record ⇒ the base check must fail"
- Evidence: `code/kserver.h:1531-1534` (`n_base=prev_base; base_of_use=n_base; pick_base=1;`) and `1579-1583` (`last_included_index!=base_of_use` ⇒ recovery refused).
- I read that this branch has another sub-path, "if the snapshot is unusable, fall back to prev_base" (`1535-1542`), so it contradicts itself **only in the sub-case "the n_base snapshot is usable but the newest record carries the old base"**;
  that case is fail-stop (the safe direction), but it disagrees with the intent the comment states about itself.

### B10 `[unverified]` Three places in runtime/vfs: `runtime.h:356/380/728/746/396` all wait `INFINITE` (a worker contract violation hangs forever);
  `runtime.h:443-451` ignores the `pthread_condattr_setclock` return value; `vfs.h:96` opens the WAL with `FILE_SHARE_DELETE` (on Windows it can be deleted/renamed while in use, and there is no file lock).

### B11 `[verified]` Application-layer fatal/argument errors exit silently (several cases)
- Evidence: `code/kdbsvr.c:285,289,296,297,299,327` (argument/config errors `return -1` with no output);
  `code/kdbctl.c:448,461` (seed parse/init failures are silent).
- Impact: a typo shows up as "the process exits immediately, with no reason at all".

---

## C. Low severity

| ID | Status | Finding | Evidence |
|---|---|---|---|
| C1 | `[verified]` | A lookup that finds no registry entry in `kserver.h` is silently dropped (no log, while adjacent branches do warn) | `3533-3534` vs `3536` |
| C2 | `[verified]` | Waking `recv_paused` requires all four counters to be **simultaneously** below their caps ⇒ one leaked enum silences it forever | `4236-4240` |
| **C3** | Generation continuity of the first record in a WAL segment is not checked ⇒ **FIXED (377dc4f)**: enforced across segments, detected, reported with both generations, and fail-stopped; demonstrated on a real 3-segment store |
| C4 | `[verified]` | Startup failure causes are invisible (cfg/WAL meta/allocation/thread failures all print only `failed to start server N`) | `kserver.h:4594-4600,4602,4642,4682`; `kdbsvr.c:300` |
| C5 | `[verified]` | Header comment disagrees with the implementation: it claims a v1 data directory is "refused and re-initialized", but only refusal happens | `kserver.h:23-26` |
| C6 | `[verified]` → **FIXED (52c002e)** | Layer boundary: the application layer reads `raft->config_new/config_joint/config_learners` directly to decide policy (raft now reports these facts on the ready bundle; the ratchet is at 0) | `kserver.h:4411-4413` |
| C7 | `[verified]` | INFO/STATS text is written with unbounded `sprintf` into `char text[2048]` (adding a field overflows) | `kserver.h:3254,3258,3260,3262` |
| C8 | `[verified]` | When the snapshot worker's post fails it frees(task) itself while the main thread still holds the pointer ⇒ `snapshot_inflight` stays 1 forever | `3957` vs `3992,4351` |
| C9 | `[verified]` | On the `UNIX` branch one arming can deliver `CEMON_DATA` several times (contrary to the header contract); the callback is re-entered synchronously when `callback_depth==0` | `2452-2461`, `3432-3435` |
| C10 | `[verified]` | The Unix recv/accept/flush loops consume no dispatch budget ⇒ a single active peer can monopolize the owner thread | `2455,2395,2342` |
| C11 | `[verified]` | `cemon_ingress_close` spins without bound; after its 2000ms drain cap `cemon_destroy` can leak a held socket | `1047-1066`, `2900-2910` |
| C12 | `[verified]` | UDP soft errors are swallowed silently (no counter); the UDP fallback branch uses the wrong buffer (`recv->buf` instead of `udp_recv->buf`) | `2189-2194, 2207-2213` |
| C13 | `[verified]` | A loop-level fatal error is only `-1`, with no interface to retrieve the cause | `3170,3269-3275` |
| C14 | `[verified]` | `2147483647ULL` does not go through the repo's own `VFS_U64_C` convention (`-pedantic-errors` reports a C99 long long constant) | `cemon.h:655` vs `vfs.h:64-66` |
| C15 | `[verified]` | Depends on `mswsock.h`'s `LPFN_ACCEPTEX/WSAID_CONNECTEX/SO_UPDATE_CONNECT_CONTEXT` with no fallback declaration; measured compile failure on the old toolchain (`/d/MinGW` 9.2.0) | `cemon.h:606-608,1675-1677,2159`; `build.sh:47-58` already records this in a comment |
| C16 | `[verified]` | The Windows completion path uses POSIX `ENOMEM(12)` as a Winsock error code ⇒ `EV.status` is meaningless | `2242,2109` |
| C17 | `[verified]` | No directory-sync/atomic-rename primitive ⇒ after a crash the directory entry of an "O_CREAT + fsync(file)" sequence may not exist (the application layer cannot compensate on its own) | `vfs.h:19-24,99-101,170-177` |
| C18 | `[verified]` | vfs read returns -1 for both EOF and a real IO error (a caller cannot tell "end of segment" from "media error") | `vfs.h:137` |
| C19 | `[verified]` | `vfs_open/unlink` expose no error codes ("missing/permission/full/in use" are indistinguishable) | `vfs.h:96-97,118-120` |
| C20 | `[verified]` | `kproto.h`'s generic encoder does not know `K_REQ_MEMBER` (only a hand-written payload works) | `kproto.h:288` |
| C21 | `[verified]` | `kdbctl` RGET treats a trailing all-digit argument as the limit ⇒ a numeric end key cannot serve as the range end; an over-long command line silently drops arguments | `kdbctl.c:267-270,541` |
| C22 | `[unverified]` | `kdbsvr` does not check the `timeBeginPeriod/SetConsoleCtrlHandler` return values; the `[cfg]` line prints the configured value, not the effective one | `kdbsvr.c:273,314,188` |
| C23 | `[unverified]` | `runtime.h:607` uses `strcmp` without including `<string.h>` (clean under MinGW-w64 only by transitive inclusion) | `runtime.h:607` |

---

## D. raft.h vs. the dissertation (module audit, severity not above "medium")

| ID | Status | Finding | Evidence |
|---|---|---|---|
| R1 | `[unverified]` | The follower commit rule uses "the tail of its own log" rather than "the last new entry in this message"; when an AE is degraded to an empty heartbeat by OOM it can commit **a divergent tail that this round never checked** | `raft.h:3600-3602` vs `dissertation.md:7198,7442`; triggering requires the REALLOC failure at `2294` |
| R2 | `[unverified]` | A learner can start an election itself and win one (yet the transfer path explicitly forbids a learner from being leader — self-contradictory) | vote tallying `3432-3437,3459-3461` vs `4173-4174` |
| R3 | `[unverified]` | The read barrier's "majority confirmation" accepts only ACKs carrying a successful append (rejections do not count), stricter than the dissertation ⇒ reads may time out during catch-up/convergence | `3705` vs checkQuorum's "any contact counts" `3711-3713` |
| R4 | `[unverified]` | Streaming snapshot identity is decided only by `(last_index, byte_size)` ⇒ different content of the same index and length gets spliced together undetectably | `3814,3839` |
| R5 | `[unverified]` | `step_down` cancels a pending snapshot install while the caller may still be committing that install ⇒ `last_applied` can pass the log tail (unreachable under the current kdbsvr driving style) | `1300-1306` vs `4326` |
| R6 | `[unverified]` | Leadership transfer reports COMMITTED as soon as "TimeoutNow is sent" (the dissertation requires the target to be elected and the old leader to step down) | `3120-3126` vs `dissertation.md:1495-1507` |
| R7 | `[unverified]` | Bootstrap does not write the initial configuration as the first log entry (it uses the caller's static configuration instead) | `2845-2853` vs `dissertation.md:2119-2125` |
| R8 | `[unverified]` | A rejected AE propagates a **not yet durable** new term outward (the vote/AE success paths both have a durability gate; this one does not) | `1277-1278` → `3660-3671` |

---

## E. Coverage gaps (tests and process) — verified by me personally

1. **The CLI one-shot mode has no regression test at all**: the "wait loop polls zero times" defect from this session survived precisely because that path had no test;
   `tools/cemon_tcp_probe.c` now exists as a minimal protocol-level client, and an end-to-end regression (`SET/GET/DEL/STATS`) should be added on top of it.
2. **`kdbctl` false success (A3) has no test**: a case is needed for "must exit non-zero when the server does not respond".
3. **The non-Windows build path is in no build/test** (B7).
4. **No CI** (`build.sh` must be run by hand; there is no single entry point such as `make test`).
5. **The bench/probe tools under `tools/` are not part of the test ladder** (`bench_rate`/`cemon_tcp_probe` were key tools for this debugging round, but they are only invoked by hand).
6. **No fuzz coverage of extreme fence/term-regression scenarios** (carried over from before).

---

## F. Parts checked and found **gap-free** (highlights)

- **Windows XP compatibility**: Vista+ or XP-missing items such as `GetQueuedCompletionStatusEx/CancelIoEx/WSAPoll/GetTickCount64/InitializeCriticalSectionEx/CreateThreadpool*/SetFileInformationByHandle/snprintf/vsnprintf/strtoull/inet_pton` are **all unused**;
  `_WIN32_WINNT 0x0501` is defined before the headers are included (correct in all three files); `AcceptEx/ConnectEx` are XP+ and correctly paired with `WSASocketA + WSA_FLAG_OVERLAPPED`.
- **cemon mechanisms** (Windows path): the accept completion order (`SO_UPDATE_ACCEPT_CONTEXT` → associate completion port → mark OPEN → emit) is correct; triple idempotent guards on read/write/accept arming; a table-driven half-close state machine; timer heap and overflow protection; backpressure limits checked before allocation; the lost-wakeup bug is fixed and explained in a comment.
- **Ownership model is self-consistent**: `kdbsvr.c:301-304` and `kdbctl.c:449-451` call `cemon_bind_owner` explicitly; workers reach the loop only through `cemon_post`.
- **Durability**: the WAL metadata slot write + fsync complete in the same call; two slots 4KiB apart; magic/version/CRC validation; recovery splits three ways (skip a torn tail / set `have_bad` when CRC and generation disagree / refuse when no usable record); snapshots persist metadata before truncating the log.
- **Request lifecycle**: every registration failure gets a reply; a denied read barrier → REDIRECT plus correction of `is_leader`; hash unlinking runs unconditionally; the ready copy is consumed only after it succeeds.
- **Code hygiene**: across `code/`, `tests/`, `tools/` — **CRLF=0, non-ASCII=0**, no leftover `TODO/FIXME/XXX`; **0 error / 0 warning** under `-std=c89 -O2 -Wall -Wextra -Wdeclaration-after-statement` (Windows branch).

---

## G. **No longer listed as gaps** per the user's settled trade-offs

1. §6.3 client sessions / exactly-once not implemented (deliberate: solved at the root by making commands idempotent)
2. Fail-stop on a full disk (accepted as acceptable)
3. README / metrics (deferred)
4. Multi-node real-hardware throughput (conditions are not available now; not done for the time being)

---

## H. Suggested handling order (top three)

1. **A1 + A2** (FCALL gate permanently closed + no client request timeout): the only combination that produces "no response at all", and A2 amplifies any result-loss scenario.
2. **A3** (CLI false success): basic correctness for scripted use; add the E1/E2 regression tests alongside it.
3. **B1/B4/B11** (closing up the visibility of silent drops and silent exits) + **A4** (align cemon's cross-thread contract with the implementation: either add locking, or change the header wording to "only the owner thread may send").

---

# Fix status (this round, 2026-09)

## Fixed and verified

| Item | Fix summary | Verification |
|---|---|---|
| A1 FCALL gate permanently closed | Any FCALL freed inside `k_request_free` reopens the gate (`open_gate` clears the flag first ⇒ recursion-safe) | `kserver_test` new case ✓ 33/33 |
| A2 No client request timeout | `K_CLIENT_REQUEST_TIMEOUT_MS`(default 5s, configurable) + `pending_deadline_us`; a timeout reports an explicit error and frees the slot; clears the discovery flags | kclient 14/14 ✓ |
| A3 kdbctl false success | Success criterion is structural (`cmd_id` + `last_done_id/status` + `!busy`); failures print a reason and rc=1; overall deadline 10s; **no more `strstr` anywhere in kdbctl** | `tests/cli_smoke.sh` 13/13 ✓ |
| A4 cemon cross-thread contract | **Per user instruction**: the header contract becomes "only the owner thread may send/recv/close" (`cemon_post` remains the only cross-thread entry point); a non-owner send/armed read fails fast with -1 | full ladder ✓ (no cross-thread senders in the repo) |
| B1 RGET flush failure silently dropped | Restored to an error reply (consistent with the fixed read path) | kserver 33/33 ✓ |
| B2 WAL write failure disguised as graceful shutdown | `fatal=1` + prints `fatal: WAL write failed` + non-zero exit | suite + selftest ✓ |
| B3 Command not queued yet returns 0 | Added `k_client_busy()` and `last_done_id/last_done_status`; the CLI no longer matches on text | cli_smoke ✓ |
| B5 Arming failure left recv_armed=1 | The flag is cleared on failure (both the Windows and Unix branches); no more false "armed" reports | suite ✓ |
| B6 Uninitialized completion key | `ULONG_PTR key=0` | suite ✓ |
| B7 C89 violations in the non-Windows branch | Three sites moved to block-top declarations, catchable by the repo's own `-Wdeclaration-after-statement` | static fix ✓ (the Unix branch cannot be compiled on this machine, see outstanding) |
| C1 Unknown result silently dropped | Prints a warning (with status and cookie) | suite ✓ |
| C8 Snapshot task double free | The worker no longer frees (the main thread holds that pointer); changed to "a leak beats use-after-free" with an explanatory comment | suite ✓ |
| C14 `2147483647ULL` | Dropped the C99 suffix (`-pedantic-errors` clean) | build ✓ |
| C16 POSIX ENOMEM used as a Winsock code | Added `CEMON_ERR_NOBUFS` (Windows=WSAENOBUFS, Unix=ENOMEM) | build ✓ |
| C20 Encoder does not know K_REQ_MEMBER | Supported in `k_request_payload` (the body goes into key as-is) | suite ✓ |
| C21 RGET trailing-number ambiguity | Changed to the `limit N` keyword; usage text updated to match | cli_smoke ✓ |
| C22 kdbsvr ignores return values | A visible warning is printed when `timeBeginPeriod`/`SetConsoleCtrlHandler` fail | build ✓ |
| C23 runtime missing `<string.h>` | Added the include (plus `<stdio.h>` for the warning) | build ✓ |
| B10 All-INFINITE waits in runtime | Bounded waits (30s) + visible warning; `pthread_condattr_setclock` return value checked; `FILE_SHARE_DELETE` removed from the vfs open | suite ✓ |
| R1 Follower commit cap | Changed to `prev_log_index+entry_count` (Figure 2/§5.4.2) | raft 221/221 ✓ |
| R2 Learner election | `raft_become_candidate` rejects learners | raft 221/221 ✓ |

## Deliberately not changed after evaluation (with reasons)

- **R3** The read barrier counts only successful ACKs: the reason given in the comment holds (a rejection proves only reachability, not possession of the leader's committed prefix) ⇒ conservative and self-consistent
- **R4** Snapshot identity is only (index,size): kdb snapshot serialization is deterministic for a given (index,term,cfg); triggering needs "same length, different content"
- **R5** `step_down` cancels a pending install: the first fix broke the contract, encoded in the tests, that "the application may report applied beyond the log tail" ⇒ **retracted**, kept as low risk (reachable only under asynchronous install driving)
- **R6** Transfer reports COMMITTED immediately: the comment states it means "initiated"; strict §3.10 semantics are optional follow-up
- **R7** Bootstrap configuration not written as the first log entry: an API design choice
- **C17 directory-sync primitive**: **explicitly not needed by the user** ⇒ not implemented
## Still unhandled (complete list, 2026-09 second-round correction)

> Correction note: an earlier verbal summary wrongly listed **the subagent's cemon audit IDs** (its A3/A5/A6/A7) as this list's IDs ✗.
> Correspondence: subagent A5 = this list's **C16 (fixed)**; subagent A6/A7 = this list's **B10 (fixed)**;
> subagent A3 = this list's **B7 (statically fixed, not verified at runtime)**; subagent A1/A2/A4 = this list's C14 (fixed)/C15 (explained in a comment)/A4 (contract changed per user instruction).

### Safety/durability related (priority)

> **Status (2026-09-20, from the 2026-09b review).**  Three rows below are still listed as unhandled but
> are fixed in the tree, so they must not be scheduled again: **C11** (bounded ingress close with a
> printed reason - `cemon.h:1112-1137`, described in this file's own O2 section), **C12** (`udp_recv->buf`
> - `cemon.h:2269/2291`, issue #10) and **C13** (`cemon_last_error`/`cemon_fatal_note` -
> `cemon.h:602/794/823`, issue #11).  The rows stay as the record of that round.  Every other line
> reference in this file is a snapshot from the day it was written and has drifted since (finding F10 of
> the review): open the file, do not trust the number.

| Item | Symptom and evidence | Impact | Why not fixed | Fix outline |
|---|---|---|---|---|
| **C18** | `vfs_read` returns -1 for both EOF and a real I/O error (`vfs.h:137/140`); the recovery scan therefore takes a "read failure" for a **normal end of segment** (`kserver.h:1466-1472`) | A media error ⇒ the WAL is treated as ending early ⇒ **a truncated state is silently recovered** (the item most worth fixing) | Needs a vfs API extension (`vfs_size`/`eof` out-params) | Add `vfs_size(file,&size)`; in the scan `off<size` ⇒ print the reason + fail-stop; cost ~20 lines |
| **R8** | The response to a rejected AE is emitted **within the same call** after `step_down` (which only sets `persist_needed`) and carries the new term; a successful ACK, by contrast, has the `deferred_append_gen` durability gate (`raft.h` `deferred_append_response` block) | If the node crashes before the gate ⇒ a term it announced externally has no durable backing (§3.8); the safety impact is weak (two leaders in the same term still require a majority intersection) | Requires reusing the deferral mechanism for "rejection responses" (a medium change, and must not slow down leader retries) | Reuse `deferred_append_gen/deferred_append_response` so the rejection is also emitted after `persist_gen` completes |
| **B9** | Recovery base-fallback branch: after `n_base=prev_base; base_of_use=n_base; pick_base=1` (`kserver.h:1531`) decoding still uses the newest record, so the base check must fail ⇒ recovery refused | Disagrees with the intent the comment states about itself (the comment says "fall back to a higher base"); the **direction is fail-stop**, no wrong data is produced | This branch triggers only in the rare sub-case "the n_base snapshot is usable but the newest record carries the old base"; a thorough fix requires reordering the base/record-selection logic | Settle `base_of_use` first (the usable higher base), then pick the **newest record whose base matches it** |
| **C11** | `cemon_ingress_close` spins without bound (`SwitchToThread` polling); after its 2000ms drain cap `cemon_destroy` can leave a held socket struct behind | A producer contract violation ⇒ the process cannot shut down; in the extreme it leaks the socket struct (the port is already closed, so no completion can reach it) | Needs a constructible reachable sequence for a "lost completion event"; none constructed so far | Add a timeout + a print to the spin; after the destroy timeout print the residual count |

### Visibility (does not change behavior, but affects debugging)

| Item | Symptom and evidence | Impact | Fix outline |
|---|---|---|---|
| **C4** | A `k_server_open` failure (cfg/WAL meta/allocation/thread) only returns -1, and the caller prints only `failed to start server N` (`kdbsvr.c:300,308`) | No reason for a startup failure ⇒ violates the ops standard that "fatal must print a reason" | Print the concrete reason at each `k_server_open` failure point (following the existing `[cfg]`/`wal:` prefix style) |
| **C5** | The header comment says a v1 data directory is "refused and re-initialized directly" (`kserver.h:25`), but the implementation only refuses | Comment and behavior disagree, misleading users | Change the comment to "refuses to start (fail-stop)" |
| **C13** | A loop-level fatal error is only `-1` (`cemon_poll`), with no interface to retrieve the cause | An application cannot tell "normal shutdown convergence" from "backend error", nor record the errno | Add a read-only accessor such as `cemon_loop_last_error(loop)` |
| **B4** | Connection cap ⇒ immediate close with no log (`kserver.h:3330`); rx over limit ⇒ immediate close and the connection's in-flight requests get no reply (`3346`); the request-level limit reuses the `"out of memory"` text (around `3355` + 1998) | A client cannot tell "capacity protection" from "peer OOM"; connection-level refusals are unobservable | Print one line when a limit triggers (with the limit name and current value); use separate text for the request-level limit |
| **C2** | Re-arming `recv_paused` requires all four counters to be **simultaneously** below their caps (`kserver.h:4258`) | One leaked enum silences every paused connection forever (especially dangerous combined with the fixed A1/gate issue) | Record "why paused" (a reason bit) and wait only for that reason's counter to fall |
| **C12** | UDP soft errors are swallowed silently with no counter (the `cemon_win_udp_recv_soft_error` branch in `cemon.h`); the UDP fallback branch misuses `recv->buf` (the `else if(bytes>0)` branch in `cemon.h`) | UDP drops are invisible; the fallback branch is currently hard to trigger (`WSARecvFrom` always writes back addr_len) | Add a UDP drop counter + fix the fallback branch |

### Cross-platform: VERIFIED (this section used to say "platform-unverifiable")

The Unix branch took part in no build when this audit was written; it now builds, runs the same gate, and
its crash contract is measured.  Evidence, all from the CentOS 7.9 guest: `REGRESS|quick|pass=9 fail=0` with
the same nine layers Windows runs; every file in `code/` md5-verified against the committed tree before
each run, so the gate is a verdict about the tree that was built; and every acknowledged write surviving a
`kill -9` (2000/2000 keys read back, never-written prefix 0/2000, no corruption report).
`doc/crash-contract.md` states the sequence, the two named platform differences, and the audit showing the
application layer holds zero platform conditionals.

| Item | Symptom and evidence | Impact |
|---|---|---|
| **Verification of B7** | The three C89 violations are statically fixed but **cannot be compile-verified** (MinGW has no `sys/epoll.h`) | Correctness of the static fix rests on human review; a `-fsyntax-only` gate on Linux is recommended |
| **C9** | On Unix one arming can deliver `CEMON_DATA` several times; the callback is re-entered synchronously when `callback_depth==0` (contrary to the header contract) | The same application code has different flow-control semantics on the two platforms |
| **C10** | The Unix recv/accept/flush loops consume no dispatch budget | A single active peer can starve the timers and other sockets |

### Cost/benefit trade-offs (not implemented)

| Item | Symptom | Why deferred |
|---|---|---|
| **B8** | Verify-after-write reads back only the trailer + a 1-byte end probe (`kserver.h:3936`), not the whole file; the old snapshot is deleted right after | Two generations of rollback material are already retained; the I/O cost and benefit of a whole-file read-back need your confirmation first (possible middle ground: sampled read-back of head/middle/tail) |
| **C7** | ~~Unbounded `sprintf` into `char text[2048]`~~ **FIXED (509af05)**: bounded appends, truncation marked and counted, and a ratchet so bare `sprintf` cannot grow | The current field set does not overflow; a change should go together with a "truncate and warn when fields exceed the limit" policy |
| **C19** | `vfs_open`/`vfs_unlink` expose no error codes | This is an API extension; can be done together with C18's `vfs_size` |
| ~~**C3**~~ | ~~Generation continuity of the first record in a WAL segment is not checked~~ **FIXED (377dc4f)** — see the findings row above | Confirming the impact requires a case for "a cross-segment loss that leaves only term/vote/base records" |
| ~~**C6**~~ | ~~The application layer reads raft config fields to decide policy~~ **FIXED (52c002e)**: raft reports the facts on the ready bundle, the app caches them, ratchet at 0 | The mechanism layer has no corresponding query API; adding one is an interface extension that must be settled together with the "layer boundary" trade-off |
| **C17** | Directory-sync primitive | **Explicitly not needed by the user** ⇒ not implemented |


## New tests

- `tests/cli_smoke.sh`: end-to-end CLI semantics (SET/GET/STATS/MEMBERS/DEL/missing key) + **failure must exit non-zero** (dead host, unknown command) ⇒ covers E1/E2
- New case `[1/33]` in `tests/kserver_test.c`: the FCALL gate must reopen after its request dies (the A1 regression gate)

## E. Snapshot-view ownership (fixed; evidence and retraction record attached)

### E1 `[fixed]` `treap_save` performed owner-thread actions on the snapshot thread
- **Mechanism**: `treap_capture` (main loop) **claims** the live retirement chain into `t->snap` and empties the live chain; then `treap_save` is called by the **snapshot thread**, and besides walking `t->snap.root` to emit bytes it also did three things belonging to the owner thread: reclaiming the `t->snap` retirement chain (freeing nodes/decrementing blob refcounts), rewriting `t->live.tree_bytes`, and resetting `t->snap.*`. These run concurrently with the main loop's COW (`blob->refs++`, retirement-chain linking) ⇒ cross-thread frees/writes of shared state.
- **Fix** (keeping the existing design: main thread captures, snapshot thread saves, the main thread keeps mutating the tree meanwhile): split into `treap_save` (**read-only**, snapshot thread) + `treap_save_finish` (**owner thread**, reclaiming+resetting at the completion point after the snapshot thread's result arrives); the header comment states the invariant: **the snapshot view is read-only to non-owner threads; capture and finish must happen on the owner thread**. `kserver.h` calls finish only at snapshot-completion handling (called uniformly before the three branches).
- **Dead code removed**: the `treap_save(server->tree,0,0)` in the `runtime_task_post` failure path has been deleted — the retry task was already serialized and could never arrive; the new task's view is overwritten by the next `treap_capture`, which folds the retirement chain into the new view, and the new finish reclaims it all ⇒ that statement only delayed reclamation.
- **Evidence**: before the fix the system heap checker reported `HEAP: Free Heap block 0000000005331AB0 modified at 0000000005331E90 after it was freed` (predating any in-house instrumentation, so unrelated to it); after the fix, **86 runs with zero reproduction** accumulated across two channels (60 rounds of page-heap network soak + 10 serial in-process stress runs + 16 4-way concurrent stress runs), with the server alive throughout.
- **Status**: **strong evidence points to closed, but this is not a proof** — the defect's reproduction rate swings wildly with machine load and heap layout (changing the debug allocator's header size changes the rate), so a single clean run is not evidence.

### E2 `[retracted]` Two wrong conclusions from this round (on the record, so they are not reused)
1. **"Victim identity/phase"**: given by the **unlocked** in-house allocator — "the victim block is at `treap.h:338`, offset +48 is `next_free`, phase during snapshot save" — that instrumentation scrambles its global registry under **multiple threads**, reporting **registry entries belonging to other blocks**, and it double-`free()`s the same pointer when evicting from quarantine (those 9 `Invalid address specified to RtlFreeHeap` come from exactly this). With locking, 10/10 clean; the conclusion is **fully retracted**.
2. **"+992 points into `base[K_URI_MAX]`"**: `base` is a field of `struct k_server` (a live object), whereas the append write at `base_len+32/+40` targets a **local stack buffer** `char path[1024]`, which does not match "+992 lands inside a freed heap block" ⇒ **retracted**.
- **Kept**: the hard data from the system heap checker (the write point at **+992** inside a block) is still valid, but "which struct" is currently **unresolved**.

### E3 `[retracted: measurement artifact]` Connection stall `pipe: stalled after 5 s (queued=127 done=0 in_flight=0)`
- **Symptom**: appeared consecutively in run41-44 of the 60-round page-heap network soak; zero replies for 5 seconds, then recovery by itself; `alive=1` in every round.
- **Conclusion**: **a measurement artifact, retracted**. That batch ran a **debug build with Application Verifier Heaps enabled**; the server was markedly slower and the client's 5-second "zero replies" stall threshold was crossed.
- **Control experiment**: `build/stall_hunt.sh` — release build, **the same load** (`PIPE 128 4000 SET`), 20 rounds; result `ok=20 stalls=0 unexpected=0 server_alive=1`, throughput 22–25k ops/s.
- **Harness requirements (landed)**: the script first `init`s the store, after startup polls `STATS` for liveness (10s), requires the positive signal `ok=<N>` per round, and counts a round that is neither ok nor stall as `unexpected` and aborts — otherwise "0 stalls" can just mean "0 data" (I made this mistake once this round: a missing argv made the server print only usage, and 20 empty rounds were reported as clean).

## F. `raft_cluster_fuzz` fails deterministically at seed 2 (new finding, unfixed)

### F1 `[reproduced]` LEADER COMPLETENESS violation
- **Invocation form** (must be exactly this; the third argument is the persistence delay): `./build/raft_cluster_fuzz.exe <start_seed> <count> [persist_delay]`
  - Correcting an earlier record ✗: the audit's "`raft_cluster_fuzz 500` is consistent" was the product of **wrong usage** — with only one argument, `seed=500, count=1`, only a single seed runs, and the "green" it reads out is meaningless.
- **Reproduction**: `./build/raft_cluster_fuzz.exe 1 40 0` ⇒ seed 1 passes, **seed 2 fails**:
  `FAIL: LEADER COMPLETENESS: leader 2 term 14 has term 14 at committed index 5, expected 8`
  Event sequence (the log carries the last 75): `CLUSTER 3 nodes / PARTITION mask=1 / CRASH node 1 / LEADER node 2 term 2 / RECONFIG node 2 -> 3 ids`.
- **Unrelated to "responding before persisting"** ✗: `persist_delay=0/1/2` and the default **all fail** (in different modes: delay=0 is a term/index mismatch, delay≥1 is "the leader is missing committed entries") ⇒ this hypothesis is refuted and retracted.
- **Scope**: the driver `tests/raft_cluster_fuzz.c` **includes only `../code/raft.h`** (it does not link treap/cemon/runtime/kserver) ⇒ **unrelated** to this round's (snapshot-view ownership) changes; but `code/raft.h` was modified on 09-18 and may have been introduced by an earlier round ⇒ **bisect pending** (needs comparison against an earlier copy; an old raft.h under `D:/KylinDB` would serve as reference).
- **Oracle credibility checked** ✓ (this step decides the classification): the model's "committed" comes from **the node's own `commit_index` advance** (`raft_cluster_fuzz.c:1795-1802`), and by the library's contract that advance happens **after entries are durable** ⇒ what the model records is the library's self-reported commit fact ✓, not driver invention ✓; the check is also applied only when "committed term < new leader's term" (avoiding out-of-order false positives ✓), and it is fully black-box (it does not use `raft_inspect` ✓).
- **So only two explanations remain, and both point to a `raft.h` defect**:
  1. **A committed entry was overwritten** — some node really did commit index 5=term 8, while the term 14 leader has term 14 at index 5 ⇒ violates §5.4.1, a state-machine-safety break (the most severe class);
  2. **The commit report is distorted** — the library reported a `commit_index` that was never truly committed (a contract defect, equally needing a fix).
- **Commit rules checked, no problem seen** ✓: the main advance requires **both the old and the new majority under a joint configuration** (`raft.h:1975-1992`); a follower commits only the prefix established by this RPC (`raft.h:3610-3620`, §5.4.2) ⇒ the rules themselves are beyond reproach; the defect is at their interaction.
- **Mechanism located (trace evidence chain)** ✓:
  1. at term 8 under **cfg{2,3}**, `idx 5 = term 8` is **APPLY**d by both node 2 and node 3 (a 2-node configuration has quorum=2 ⇒ genuinely committed);
  2. `RESTART node 2 term 13 lii 4 count 0` — node 2 recovers from an **index 4 snapshot** with an empty log; it then wins election at **term 14** (cfg{1,2,3}, with only node 1's single vote — and node 1's log is equally empty) and **writes term 14 at index 5**, overwriting the committed entry;
  3. the root is a **configuration fork**: node 3 stays at **{2,3}** while node 1/2 are at **{1,2,3}** ⇒ two disjoint configurations each satisfy a majority.
- **Hypothesis one (refuted ✗)**: "a change initiated by the already-removed node 1 is applied locally by the library" — `raft_reconfig` explicitly requires `state==RAFT_LEADER` (`raft.h:2151`) ⇒ changes initiated from a follower are refused ⇒ **retracted**.
- **Correction: one over-assertion** ✗ — the earlier "`persist_delay=0` also fails ⇒ unrelated to 'responding before persisting'" **does not hold**: that parameter only controls the **duration** of persistence; it does not change the fact that "a crash can land inside the persistence window".
- **Smoking-gun evidence (apply-vs-persist order, §3.8 class)** ✓: in the trace node 2 first `APPLY node 2 idx 5 term 8 … commit 5 cfg{2,3}`, then `CRASH node 2` → `RESTART node 2 term 13 lii 4 count 0` — **applied up to index 5, yet after the restart the disk holds only up to index 4 with an empty log** ⇒ applied state and persisted state are out of sync.
- **Correction two** ✗: reading "APPLY reached 5 but the restart shows only 4" directly as a defect **does not hold**. `raft_advance_commit`'s durability gate constrains only **a leader's own entries when it has a voter peer** (`raft.h:1966` `require_durable=raft_has_voter_peer(r)`); **a follower committing/applying on `leader_commit` never needed its own entries durable first** (`raft.h:3610-3620`), and Raft permits a follower to lag in persistence and catch up by resync ⇒ on its own this is **legal behavior**, not a defect.
- **The real point of divergence (narrowed)** ✓: the library lets **a follower report `commit_index` when nothing is durable locally**, while the driver comment assumes that "**commit_index advances only after entries are durable**" (`raft_cluster_fuzz.c:1791-1794`) ⇒ the two **contradict**.
- **Why this contradiction is fatal (the chain of this failure)** ✓: the **configuration entry (C_new)** node 2 applied never reached its durable image ⇒ after the restart it returns to **{1,2,3}** per the snapshot configuration, while node 3 stays at **{2,3}** ⇒ configuration fork; the majority {1,2} under the old configuration (node 1's log is empty) does not contain the committed index 5/6 yet suffices to win ⇒ **committed entries overwritten**.
- **Fix direction**: in the spirit of Ongaro §4.3 (a configuration change takes effect only **after persistence**), the fix more likely belongs **on the library side** (configuration entries/commit reporting must take effect only after local durable confirmation); the other side is to make it an explicit contract (state that followers may lag) and relax the oracle accordingly — the two **cannot both hold**; this needs a decision before implementation.
- **Correction three (all three hypotheses of that round were refuted by the code ✗)**:
  1. "the library does not implement 'respond after persisting'" — **wrong**. `deferred_append_response` **defers** a follower's AE response until its own durable confirmation, and reports `durable_confirm` (only the durably persisted prefix): `raft.h:3160-3206`, `:3642`, `:4261-4269` ⇒ §5.3 is implemented;
  2. "the driver discards data when it crashes after reporting durable to the library" — **wrong**. `persist_copy_flush` (`raft_cluster_fuzz.c:1164-1175`) calls `disk_commit_pending` at the same place it calls `raft_persist_complete`, promoting the pending→committed image immediately, and crash recovery (`:1261`) reads exactly that image;
  3. "the driver model applying before persistence is the root cause" — **not sufficient**: the driver really does apply entries that are not yet durable (`:1531-1545` vs the `persist_pending` gate at `:1721`), but the library only ACKs the persisted prefix, so that behavior on its own cannot raise any majority.
- **Remaining contradiction (unresolved)** ✓: node 2 crashes after `APPLY idx 5` (cfg{2,3}, quorum=2) and the restart shows only lii=4 ⇒ **index 5 on node 2 is simultaneously "committed and applicable" and absent from its committed image**; yet by the library's deferred-ACK mechanism node 3 cannot have committed index 5 on the strength of node 2's non-durable ACK alone ⇒ the two cannot both hold, so one of them means I am misreading the harness.
- **Next step (targeted data capture, not guessing)**: add self-identifying prints for **index 5** on the harness's persist/apply/commit paths (`ev("P5 ...")`: persist request, durable reported value, image promotion, ACK emission) and rerun `1 40 0` ⇒ use the timeline to decide which side the contradiction falls on.
- **Targeted capture (`P5` self-identifying prints, added in `tests/raft_cluster_fuzz.c`)** ✓: the timeline shows node 2 and node 3 **both** reported `durable 5`, `durable 6`, with node 2 reporting as far as `durable 7`; yet **at the moment of the crash both of node 2's images are `disk_lii=4 count=0` / `pending_lii=4 pending_count=0`** (`P5 ... CRASH-TIME`). ⇒ **the driver reported to the library a prefix it had written into no image at all** (the library's contract requires reporting only indexes that are genuinely durable) ⇒ the library ACKs on that basis, the leader forms a {2,3} majority on that basis ⇒ after the crash the entry is gone and the configuration rolls back ⇒ LEADER COMPLETENESS violation. **Ruling: the defect is in the test driver's persistence model, not in `raft.h`**.
- **Two fixes already made to the driver** (changing `durable_index`/`inflight_durable` from "the live log tip `live_last`" to "that persist view's `last_included_index + log_entry_count`") **failed to change the timeline** ✗ ⇒ the persist view **itself** already contains entries up to 7, which means the entries were **written into no image** during the `disk_persist`/`persist_copy_capture`/`disk_store_log` interaction ⇒ the contradiction is now compressed inside that wrapper and needs a thorough pass next round (the `snapshot_dirty` branch of `disk_persist` writing only the pending image, and `disk_commit_pending` promoting only snapshots, are the prime suspects).
- **The `P5` instrumentation is a temporary probe**: the `P5` prints in `tests/raft_cluster_fuzz.c` are kept until this driver defect is fixed, to allow reproduction, and **must be deleted once it is fixed** (consistent with "instrumentation must be reverted").
- **Correction four ("reports more than it stores" also refuted)** ✗: on the normal branch `durable_index` takes **the persist view's own tip** (`live_last`, derived from `p->last_included_index`/`p->log_entries[]`), matching what `disk_persist` writes ⇒ there is no "reporting ahead".
- **Correction five (I truncated the evidence)** ✗: I earlier drew a conclusion from only the first 26 lines of `P5D`, violating "an audit conclusion must not truncate the evidence list". The full 78 lines show: every persist view of node 2 has `log_entry_count ≤ 1`, and the pre-crash image is `disk_lii=4 count=0`.
- **Prime suspect (to be verified, not yet a conclusion)** ✓: the driver's image is **rewritten wholesale per persist view** — a heartbeat view with `log_entry_count=0` **zeroes an already-durable log** (`disk_store_log(n,p,0)` sets `disk_log_count` directly to that view's entry count). A real application's durable log must be a **monotonically growing prefix** and must never be truncated by a later view; if it holds, the chain "reported durable, then wiped by a heartbeat, then lost on the crash" fully explains this violation.
- **Verification method (next step)**: print the sequence of `disk_log_count` changes at `disk_store_log(n,p,0)` (`P5D` already has OUT lines) ⇒ if "count=N(>0) first, then count=0 with no log-truncation event in between" appears, the suspicion is confirmed; then change image writing to **prefix merging** (keeping existing entries) and rerun `1 40 0`.
- **Correction six (the "driver defect" ruling downgraded)** ✗: based on the observation that "the image is zeroed by heartbeats" I made three fixes to the driver (`durable_index`/`inflight_durable` computed per view; the non-snapshot path changed to a `disk_merge_log` merge), and **all three had no effect on the result** (seed 2 still fails) ⇒ the observation is a fact but is not the cause of this violation ⇒ **all three changes reverted**, driver semantics restored as they were; the "driver defect" classification has **insufficient evidence and is downgraded to unclassified**.
- **Checkable fact list (no conclusions)**:
  1. `raft_cluster_fuzz 1 40 0` fails deterministically at seed 2 (LEADER COMPLETENESS: leader 2 term 14 has term 14 at committed index 5, expected 8);
  2. the driver **links only `raft.h`** and is unrelated to this round's snapshot changes;
  3. four library-side mechanisms checked in code are **self-consistent**: commit advance (joint-configuration dual majority + the leader's own durability gate), follower commit (§5.4.2 prefix cap), respond-after-persist (`deferred_append_response` + `durable_confirm`), and recovery reading the committed image;
  4. the driver's image really does **reset `disk_log_count` from 1 to 0 under a heartbeat view** (visible in the `P5D` sequence 1,0,1,0,1,0), and that behavior **did not fix this violation** (see correction six);
  5. before the crash node 2's image is `disk_lii=4 count=0`, while its `durable_confirm` had advanced to 7.
- **Correction seven (the "merge the image" fix is also ineffective)** ✗: the driver **already has** the correct primitive `disk_merge_log` (deduplicating merge by absolute index, `:926`), whose comment matches this observation exactly ("otherwise applied entries are silently lost on restart"), but it is **called only on the `snapshot_dirty` branch**. After using it on the non-snapshot branch as well, **the violation persists and the crash-point image is still `lii=4 count=0`** ⇒ that edit does not affect the result and **has been reverted** (driver semantics restored, baseline reproducible).
- **Established observation (not a conclusion)** ✓: persist views are **incremental** — an interleaved timeline shows one view as `first=6 last=6` (boundary lii=4, no index 5), yet the driver rewrites the image wholesale to that view and then takes the view tip as the durable report ⇒ **the reported durable position disagrees with what the image actually holds**. This is a fact about the driver's model, but (see correction seven) it is **not** the cause of this violation.
- **Correction eight (a fourth driver-side fix is also ineffective)** ✗: I added another promotion at "immediately try to promote the pending snapshot after reporting durability to the library" (rationale: the **configuration entries** compacted into the snapshot exist only in the unpromoted pending image, and pending is discarded on crash ⇒ the old configuration is recovered). **The violation persists and the crash-point image is unchanged** ⇒ reverted. Four targeted fixes have now all had no effect on the result.
- **Methodology correction (important)**: I made four fixes **working backwards from the failure point**, which is a directional error — the reported failure is the **end** of the chain, and the true cause lies at the **first divergence before it**; any repair made outside the upstream is idle motion (its signature: several individually sound fixes with pass/fail not moving at all). **The next round must change method**: first locate **the earliest "two independent records disagree about the same state"** in the event stream (e.g. harness model vs component report, image vs reported durable, applied vs durable), then bisect **against that event**; a fix that cannot demonstrate it "changed the event stream at that point" counts as untested.
- **Correction nine (classification reached: the first divergence is located; the cause = the driver's unconditional truncation in `disk_merge_log`)** ✓✓
  - **Located**: a new T2 probe (printing node 2's "library state / image / membership" three-line trace on every advance). The **first divergence** appears at the reconfiguration step: the library `L=(3,3)→(3,2)→(2,2)` (joint→final), while the image `I` stays `(3,3)`.
  - **Cause** (walked line by line): the end of `disk_merge_log` **unconditionally** truncates the image log to "this persist view's boundary" (`:951-980`, written for seed 70380's "truncation within the same snapshot"). The function was later reused for **ordinary/heartbeat views**, and such views are **incremental** (a heartbeat view has `log_entry_count==0`) ⇒ the truncation throws the **configuration entries** already written into the image back to the boundary ⇒ crash recovery restores **old membership** ⇒ a node can win election while excluding "the node that holds the committed log" ⇒ committed entries overwritten.
  - **Verification**: restricting truncation to **views with authority over the tail only** (snapshot_dirty's full tail) while ordinary/heartbeat views **merge without truncating** ⇒ **seed 2's LEADER COMPLETENESS violation disappears** (`1 40 0`, `1 200 0` and all three default-delay settings no longer report it), and that first divergence disappears from T2 with it ⇒ **ruling: a semantics defect in the test driver's durable image; the library side exonerated**.
  - **Lesson**: one "the fix is ineffective" judgment during this was **based on a stale binary from after a failed compile** ⇒ confirm the build succeeded before interpreting run results (running on when the script greps an error is a wasted run).
- **The newly exposed independent failure has also been attributed (driver side)** ✓: after the truncation fix it instead reported `FAIL: LIVENESS: no stable leader elected after heal (leader changes=0, term 0 -> 0)`. After adding the `RESTORE-REFUSED` diagnostic, measurement shows: on the tail-segment restart the driver's image is **illegal** — `img_lii=1 img_cnt=1 first=3` (boundary 1, first entry 3, **index 2 missing**), `img_lii=1 img_cnt=35 first=3 last=37`. The library's `raft_create` **correctly refuses** such a log with holes ⇒ the node stays dead ⇒ under the {1,2,3} configuration only node 1 remains ⇒ no majority ⇒ the liveness assertion necessarily fires.
- **Expert conclusion (F closed out)** ✓: **every** reproducible failure of this fuzz is attributable to the **driver's durable image model**; the library side is self-consistent at every checkable point. Three independent pieces of evidence: ① the first divergence (T2 three-line) = `disk_merge_log` also "truncating to the view boundary" for incremental/heartbeat views (`:951-980`) ⇒ durable configuration entries discarded ⇒ old membership restored ⇒ committed entries overwritten; ② holes appear in the image (measured via `RESTORE-REFUSED`) ⇒ recovery correctly refused by the library ⇒ no majority in the tail segment; ③ the reported durable position takes an incremental view's tip while the image does not hold that prefix (T2: `d=6/7` while `img_cnt` oscillates between 0/1).
- **Fixes verified and their effects (all **reverted**, kept as reference for the next round's full rewrite)**:
  ① applying truncation only to "tail-authoritative views" (snapshot_dirty's full tail) ⇒ **seed 2's LEADER COMPLETENESS disappears**, but exposes ②;
  ② having the image maintain "a contiguous prefix from the boundary" ⇒ **LIVENESS disappears**, but changes into an earlier `LEADER COMPLETENESS: leader 2 term 7 … index 2, expected 6`.
  ⇒ **patching one by one only moves the failure among several violations** ⇒ disposition: **stop patch-fixing the driver; its persistence model needs a rewrite** (a monotonic, contiguous durable log consistent with what is reported).
- **Current state**: driver semantics **restored** (the baseline is word-for-word identical to the original ✓); only **13 diagnostic probes** remain (`T2`/`P5`/`P5D`/`P5E`/`RESTORE-REFUSED`), to be deleted after classification. The diagnostic probes `P5`/`P5D` (`tests/raft_cluster_fuzz.c`, 7 sites in all) are **kept for reproduction** and must be deleted after classification.
- **Nature**: the library claims to support safe reconfiguration (deferred reconfiguration + building the joint configuration after catch-up, `raft.h:665/2054`), and the joint→final drop happens at entry apply (= committed time) and is logically sound ⇒ therefore **a configuration that fails to converge is a library-side defect**, not driver overreach. **High severity** (can cause committed entries to be overwritten).
- **Severity**: high (if it holds, it is a consistency/integrity violation of the committed log).
### F2 Driver persistence-model rewrite outline (directly implementable next round, not a patch)

**Invariants to maintain** (all three are currently violated; each measured):
1. the image log is **a contiguous prefix starting at `disk_lii`** (the first entry must be `disk_lii+1`, no holes);
2. the image content must **cover** the prefix already reported durable to the library (reported value ≤ the tip of the image's contiguous prefix);
3. the image **must not** discard an entry merely because "the view did not carry it" — **an ordinary persist view is incremental** (may be empty = a heartbeat), and only a **tail-authoritative view** (`snapshot_dirty` carrying the full tail) may trigger truncation; evidence of a library log rollback (conflicting overwrite) can only come from "the view carried an entry with a **different term** at that index".

**Single choke point**: add `disk_image_normalize(node *n)`, called at the end of every path that mutates the image (`disk_persist`'s two branches, `disk_commit_pending`'s snapshot promotion and compaction, install receipt, before restore); it uniformly (a) discards entries with `index <= disk_lii`, (b) truncates at the first hole, (c) asserts the first entry == `disk_lii+1` and that the term/kind sequence is self-consistent.

**Reporting rule**: `durable_index` uses the tip of the **normalized** image prefix (neither the live log tip nor the view tip).

**Verification gate**: on **every advance**, T2's three lines no longer show either kind of divergence (library config ≠ image config; reported value > image tip); then `raft_cluster_fuzz 1 2000 0`, the default-delay setting, and the three `g_clean` tail-segment settings must all be green.
### F3 Implementation result (the F2 rewrite has landed) and the new target

**Landed (`tests/raft_cluster_fuzz.c`)**:
- `disk_image_normalize()` as a single choke point (the image is always "a contiguous prefix from `disk_lii`") ✓, called at the end of `disk_commit_pending`, at the end of `disk_persist`, and before restore ✓;
- truncation now **triggered only by conflicting evidence** (a **different term** at the same index ⇒ the library really did overwrite that suffix) ✓, no longer truncating because "the view did not carry an entry" ✓;
- `durable_index`/`inflight_durable` now report **the tip of the normalized image prefix** (the deferred path computes it when the flush persists) ✓;
- `fuzz_alloc_calls`/`fuzz_oom_at` reset per seed (the OOM trigger is an **absolute** allocation ordinal ⇒ not resetting makes "the same seed change with the call volume") ✓;
- a `SEED n` marker added to the event stream ✓ (otherwise a failing seed could only be guessed at by bisection) ✓.

**Effect**: in `raft_cluster_fuzz 1 2000 0`, **seeds 1..689 all pass** ✓; the previously always-failing seed 2 and the LIVENESS-class failures are gone ✓ ⇒ **all of F's earlier violations have been eliminated** ✓ (consistent with the "driver-side defect" classification ✓).

**New target (unclassified, deterministic reproduction)**: `seed 690` ⇒
`FAIL: LOG MATCHING: index 4 applied with term 10 but term 9 was applied elsewhere (node 2)`
Scene: node 1's snapshot boundary is `LII=1/LIT=2`, node 2/3's is `LII=3/LIT=8`; the final events are `APPLY node 1 idx 2 term 6 / idx 3 term 8`, `APPLY node 2 idx 4 term 10`. Property = **state-machine safety** (the same index applied with different terms), suspected to involve the **snapshot/install** path. Reproduction command: `./build/raft_cluster_fuzz.exe 690 1 0`.
Next round's method: extend the `T2` three-line probe from "node 2 only" to **all nodes** (or make it a compact single line), locate the **first divergence**, then bisect.
### F4 `[fixed]` selftest leaves `kdb-selftest-*.snap.*` behind in the working directory

- **Symptom**: 30 `kdb-selftest-<pid>-<us>-n<id>.snap.<index>` files accumulated in the repository root (each selftest run adds 2–3).
- **Root cause**: both cleanup sites (single-node `k_selftest_node_cleanup`, three-node `k_embedded_clean`) only `vfs_unlink` one by one according to "the snapshot indexes recorded in the store metadata / live server fields". But ① some snapshots' indexes are **in no metadata at all** (measured indexes 97/64) ⇒ undiscoverable; ② some cleanup call sites are **before** `runtime_stop` ⇒ the handle is still open and on Windows the unlink **fails silently** (the return value is ignored).
- **Fix**: added a shared helper `k_selftest_snap_sweep(base)` — a bounded index scan over `0..K_SELFTEST_SNAP_SWEEP(4096)` that **unlinks while counting failures and retrying** (up to 8 times × 20ms, to ride out the window where "the just-stopped server still holds the handle"); both cleanup sites call it last; additionally the bootstrap path's snapshot index is now **captured before release and unlinked after**.
- **Verification**: build **0 error / 0 warning**; `selftest` run 3 times consecutively, all PASS; residual files in the root after a run: **0** (the 30 historical ones cleared).
### F5 Next step for seed 690 (targeted capture; the window is no longer big enough)

**Known facts** (`./build/raft_cluster_fuzz.exe 690 1 0`, event window of 1024):
- within the visible window `index 4` is applied only once: `APPLY node 2 idx 4 term 10` (commit 5, cfg{1,2,3});
- yet the oracle's "first applied term" is 9 ⇒ that apply happened **earlier, cut off by the window** ⇒ the current 1024-event buffer is not enough to see a whole seed;
- at the end the per-node three lines are: `T2 n1 23 t13 c3 a3 d5 L33 l1 i1 5 I33`, `T2 n2 23 t13 c5 a3 d5 L33 l3 i3 2 I33`, `T2 n3 22 t13 c3 a3 d5 L33 l3 i3 2 I33`
  ⇒ node 1's snapshot boundary is 1 (holding 5 entries: index 2..6), node 2/3's boundary is 3 (2 entries each); all three agree on the configuration ({1,2,3});
- events in between: `RESTART node 1 term 12 lii 1 count 5`, `RESTART node 3 term 10 lii 3 count 2`, `CRASH node 2/3` (their CRASH-TIME images `disk_lii=3 count=1/2`), etc.

**Next steps (two, both cheap)**:
1. widen the event buffer `EV_MAX` (currently 1024) as a seed needs (or change it to dump the whole ring buffer to disk only on fail);
2. add **targeted prints**: one line for every `check_apply` of `index == 4` (extendable to 3..6), and for every log change where a node "writes a term at index 4" ⇒ directly see **who wrote term 9 first and who wrote term 10 later**, and whether a truncation/install occurred in between.
### F6 `[closed out]` The F thread closed: all three root causes located and fixed, `1 2000` all green

**Root causes (all driver-side, each with evidence)**:
1. **Wrong image-truncation criterion (two generations)**: at first "truncate whatever the view did not carry" ⇒ a heartbeat view zeroes durable configuration entries ⇒ old membership restored ⇒ committed entries overwritten (LEADER COMPLETENESS, seed 2); changed to "truncate on the same index with a different term" ⇒ an old image entry displaces the committed new entry ⇒ committed index 4 (term 9) gets written as term 10 (seed 690 LOG MATCHING). **Final criterion**: **the library's own log tip** (`n->r->log.last_included_index + n->r->log.count`) is the sole authority — drop only what the library no longer holds, keep what it still holds, and merge by overwriting the term at each index.
2. **The black-box log tip came from the persist view**: an incremental/heartbeat view's tip is just the snapshot boundary ⇒ a restarted new leader was computed as "log tip 12" (it actually holds 13/14) ⇒ false leader-completeness reports (seed 914). **Fix**: `last_index` is always derived from the **library log**.
3. **Image invariants** (the earlier stage): `disk_image_normalize()` as a single choke point + `durable_index` reporting only the normalized prefix tip + the OOM trigger base reset per seed + the `SEED n` marker added to the event stream.

**Verification**: `raft_cluster_fuzz 914 1 0` / `690 1 0` / `1 40 0` / `1 200 0` / `1 2000 0` / `1 2000` (default delay) **all `done`** ✓ ⇒ the two violation classes that always failed within 40 rounds (LEADER COMPLETENESS, LIVENESS) and the LOG MATCHING that always failed within 2000 rounds have both been eliminated ✓.

**Cleanliness**: the temporary probes (`T2`/`P5`/`P5D`/`P5E`/`LOGCHG`/`RESTORE-REFUSED`, 15 sites in all) **have all been removed** ✓; what remains is **the fix itself** and the valuable diagnostics (the `SEED` marker ✓, the `--apply-stress` stress mode ✓).

---

## Observations recorded but NOT explained

**O1 — one guest gate run reported `pass=7 fail=1`, failing layer not captured (2026-09-19).**
`./build.sh regress quick` on the CentOS 7.9 guest reported `REGRESS|quick|pass=7 fail=1` once. The
failing layer is **unknown**: the gate output had been filtered with a grep before being read, so the
`GATE|...|FAIL|` line never reached the record — the exact mistake this project keeps a rule about
("never pipe a tool's output through a grep before reading its verdict"). Five consecutive runs
immediately afterwards were `pass=8 fail=0` (76-79 s), and the Windows side has no matching episode.
Nothing is concluded from it: it is recorded so that a recurrence is met with the full output rather
than re-discovered. First thing to capture next time: the `GATE|...|FAIL` line and `build/regress/`.

**O2 — the C11 demonstration forces the internal counters, and a slow peer cannot reproduce the hang.**
The `cemon_ingress_close` fix (issue #9) was demonstrated by setting `ingress_count` directly, not by
driving a slow peer. That is not a shortcut around a hard repro: the ingress section covers a post/send
call on the **owner** side, so a peer that sends slowly never holds it — only a thread that never leaves
the section can, which is exactly the hang the bound removes. The demonstration therefore proves the exit
condition and the warning, and is labelled as forcing the condition rather than as a peer-driven
reproduction.

**O3 — a UDP datagram was delivered with `len=0` in a probe (2026-09-19).** While producing triggerable
evidence for issue #10, a probe on one loop bound a server socket on 127.0.0.1:18700 and a client socket,
armed `cemon_recv(server)`, sent 9 bytes with `cemon_sendto`, and polled: the `CEMON_DATA` callback fired
**once with `len=0`**. It is not established whether that is a probe-usage error (two sockets on one loop,
send/arm ordering) or a real zero-byte delivery — the probe was not instrumented far enough to separate
them before the change was closed. Recorded rather than explained away. If it recurs: print `bytes` at the
completion handler, the sockets' `recv_armed`/`busy` flags at send time, and the `WSARecvFrom` return
code, all in one build.

## G. Open items exposed by compiler differences (raised by CI's gcc 16.2.0, **unclassified**)

**G1 — whether `raft_ready` is fully initialized by `raft_advance`.** **SETTLED, fixed in `raft.h`** (2026-09-19).
CI (MSYS2 gcc **16.2.0**) reports 17 items in `tests/raft_test.c`: `'ready.<field>' / 'ready' may be used
uninitialized` (around lines 713/748/902/1142/3153/3711/3893/4095/4119/6968/9107), while this machine's **pinned gcc 15.2.0 reports none**.
Two possibilities, **not yet classified**:
 (a) a false positive of the new compiler at `-O2` (the tests `memset` first, then pass the pointer);
 (b) `raft_advance(..., &out)` does not write every field of `out` ⇒ the caller reads uninitialized memory. If (b), then by the settled contract
     that "ready must be reported completely", **this is a raft.h contract gap and raft.h should be fixed**.
Evidence-gathering method (to run the next time this area is touched): read every assignment path of `raft_advance` in `raft.h`, and compile **the same site** under the four combinations `-O2`/`-O0` ×
`15.2.0`/`16.2.0`, comparing whether the warning follows the optimization level (optimization-related `-Wmaybe-uninitialized` is usually (a)).
**Do not start editing code on the strength of CI's 17 lines alone.**

### Verdict (settled by the method above, then by a deterministic probe)

**Neither (a) nor (b) as stated — a third cause, and CI was right to warn.**
`raft_advance` *does* zero the whole bundle (`memset(ready,0,sizeof(raft_ready))`), but it did so **after**
its guards, so an early return — `!r`, or a phase below `RAFT_PHASE_READY` — left the caller's struct
**exactly as it found it**. The tests that read a field after a *failed* advance were reading stack
garbage. So it is not an optimizer false positive (the read really is undefined) and not "every field is
missing" either; the path is the one a caller takes when the call fails.

**Why the pinned 15.2.0 says nothing:** its analysis did not follow that path; 16.2.0's did. The warning
count was a compiler-capability difference, not a code difference — which is exactly why the card's
"do not edit on the strength of the 17 lines" instruction was followed: the mechanism was established
first, independently of any warning.

**Proof (deterministic, no compiler-version dependency):** a probe poisons a `raft_ready` with `0xAA`,
calls `raft_advance(0,10,&rd)` (the failure path) and inspects it:

| | failed `raft_advance` leaves the caller's bundle |
|---|---|
| before the fix | `fields left at 0xAA: YES` — `has_work=-1431655766`, `is_leader=-1431655766` (uninitialized read) |
| after the fix | `fields left at 0xAA: none` — `has_work=0`, `is_leader=0` (well-defined empty bundle) |

**Fix (in `raft.h`, per the card's own rule — never in the test):** the `ready` null check stays first,
then the bundle is zeroed, then the `r` and phase guards. A failed advance now leaves a well-defined
EMPTY bundle while the return code still reports the failure; no field of `raft_ready` is ever undefined,
and the "ready must be reported completely" contract holds on the failure path too.

**Four-combination status, stated exactly:** `gcc 15.2.0` × `-O0` and × `-O2` were both run on
`tests/raft_test.c` with `-Wmaybe-uninitialized` — 0 uninitialized warnings in each. The two
`gcc 16.2.0` combinations were **not** run: that compiler is not installed on this machine and the
environment cannot fetch one reliably, so the warning count for it is unavailable here. The mechanism was
demonstrated directly instead, which does not depend on the compiler seeing it. Verification after the
fix: `REGRESS|quick|pass=8 fail=0` on Windows and on the Linux guest, with `raft_test 221/221`.

**G2 — CI differs from the local toolchain (accepted by design, not a defect).**
CI uses MSYS2's bundled gcc (currently 16.2.0), while the local toolchain is pinned at 15.2.0 (`MINGW_BIN=/d/MinW64-15.2.0/bin`). Therefore the
**0-warning contract is enforced only on the pinned toolchain**: the `regress` build gate, on detecting a foreign toolchain, changes to "report diagnostics but do not fail on them"
(`GATE|build|ok|rc=0 diagnostics=N binaries=yes [foreign toolchain, warnings reported not enforced]`),
while diagnostic lines are still printed as before, **never silenced**.

**G3 — ✗ retracted: I once asserted that "MSVC 6.0 compatibility was never verified at the toolchain layer".**
Based on (contemporary) observation: `vfs.h:16`, `raft.h:104`, `cemon.h:16`, `runtime.h` show `typedef unsigned long long …`,
and MSVC 6 has no `long long`. **That conclusion was refuted by the code and is retracted**: those typedefs are all inside `#if defined(_MSC_VER)` branches,
and it is `#else` that is `long long` (`vfs.h:63-67`, `cemon.h:13`, `raft.h:92-107`, `treap.h:113-116`, `kbase.h:27-42`);
the repo ships a complete 64-bit facility: `k_i64/k_u64`, `vfs_u64`, `cemon_u64`, `raft_i64/raft_u64`, `treap_u64`,
literal macros `K_*/VFS_/RAFT_/TREAP_*_I64_C|U64_C`, print macros `K_U64_FMT`/`RAFT_U64_FMT`
(`"I64u"`/`"I64d"` on MSVC, `"llu"`/`"lld"` on gcc), and `raft.h:99` carries an explicit comment forbidding a direct `%lld`.
**Lesson**: I drew a conclusion after grepping only as far as the `#else` line without reading the conditional compilation — "reading code" means reading the **context**, not one line.
**But this correction did hit one real violation** (the principle really had been broken by mistake; only the breaker was a format string, not a typedef): three lines in `code/kdbctl.c`
write `%llu` directly and cast with `(unsigned long long)`; they have been changed to `%" K_U64_FMT "` + `(k_u64)`.
Two mechanical rules added (`tools/check-principles.py`): the five headers must contain both the `__int64` and `long long` forms;
bare `%lld`/`%llu` are forbidden in `code/`. Self-proof: injecting one real violation into each → exactly 2 FAILs → precise revert → 0 residue.

---

## I. Linux (CentOS 7.9, gcc 4.8.5): first full build and test run

The project had only ever been built on Windows; the POSIX branches had never been compiled.  A
CentOS 7.9 VM (4 cores, glibc 2.17, gcc 4.8.5) was added and the tree was built and run there.

**Result: 18 of 18 targets build, and every gate that does not depend on the Windows-only test
passes.**  Verbatim verdict lines:

| Driver | Verdict on Linux |
|---|---|
| `raft_test` | `SUMMARY: 221/221 passed` |
| `kserver_test` | `SUMMARY: 33/33 passed` |
| `kclient_test` | `SUMMARY: 16/16 passed` |
| `raft_fuzz 1 2000` | `done: 2000 iterations` |
| `raft_cluster_fuzz 1 200 0` | `done: 200 iterations` |
| `kserver_cluster_fuzz 1 1` | `done: 1/1 clusters consistent` |
| `selftest` | `selftest: PASS` (storage, snapshot and network paths in one process) |
| `tests/cli_smoke.sh` | `cli_smoke: PASS`, zero failed checks |
| `cemon_test` | `SUMMARY: 5/5 passed` after porting its poisoned allocator to `mmap`/`mprotect`.
| | Confirmed on both platforms, and the poisoning still WORKS: re-introducing the old
| | post-`cemon_tcp_finish` socket read on a throwaway copy makes the ported test segfault
| | (rc=139) in case 1 instead of passing silently. |

**What had to change to get there** (all in `build.sh` and the sources' platform guards):

1. `build.sh` linked `-lws2_32`/`-lwinmm`/`-lpsapi` on every target; those are now variables that are
   empty on POSIX.  Artifact names keep the `.exe` suffix so the harnesses and the gate are unchanged.
2. `-D_POSIX_C_SOURCE=200809L` had to come from the COMMAND LINE: with `-std=c89` glibc hides
   `clock_gettime`/`struct timespec`, and a header that defines the feature macro after another
   header has included `<time.h>` is too late.
3. Eleven test/tool files included `<windows.h>`/`<winsock2.h>` unconditionally, and `code/cli.h`'s
   non-interactive fallback set `hstdin`/`hstdout`, which exist only in the Windows variant of
   `cli_ctx`.  Both are real defects for anyone building on POSIX.
4. `kserver_cluster_fuzz` and the threaded benchmarks linked `pthread_*` without `-pthread`.
5. Scripts in the repository had no executable bit (`core.fileMode=false` on the Windows side), so a
   fresh Linux clone could not run `./build.sh` at all.  Fixed in the index with
   `git update-index --chmod=+x`.

**Measurement worth carrying forward**: one `fsync`-equivalent flush measures **172 us** on this
CentOS box against **~1130 us** on the Windows machine, and this project's throughput ceiling is
`batch size / flush cost`.  The K-sweep numbers recorded in `doc/measurements/` therefore describe
the Windows machine only, and any comparison across the two must say which one it was measured on.

## J. The two Linux measurement anomalies

Asked to run these down.  One is closed, one stays open with its evidence.  Both were first seen in
the Linux records beside the Windows ones.

**J-1 (closed, not a defect): `kdbctl PIPE 1 4000` reported `ok=3509` at 58 ops/s.**
`code/kdbctl.c` polls with `cemon_poll(loop,10)` and leaves the queueing loop as soon as the single
in-flight slot is busy, so at k=1 every request costs a 10 ms poll plus a 4.6 ms round trip.  60 s /
~15 ms = ~3900, and 3509 is simply how many fitted before the tool's own 60 s cap
(`wall_us=60003845`).  The line that said so - `error: PIPE exceeded 60s` - was lost because I had
piped the output through `grep '^pipe mode'`, which hides precisely the diagnosis.  Windows shows the
same arithmetic (k=1 n=4000 -> ok=3865, capped), so the mechanism is the client on both platforms,
and 58 ops/s was never a throughput.  Fixed at the reporting layer: the verdict line now ends with
`end=complete|cap60s|stalled`, proven both ways - a normal run prints `end=complete`, and a
deliberate k=1 n=4000 run prints `end=cap60s` after its error line.

**J-2 (located; it is queueing, not a defect; two earlier claims retracted along the way): an
occasional ~13 ms single-request tail in an open-loop run (`bench_rate`, 2000 in flight, k=32, 1 B).**

Counters (all in STATS, all carrying sample counts so a maximum of 0 can never be read as "fast"):
`req_age_ms_last/max`+`slow_acks` (how long the server held a request, admission to terminal result,
in the server's injected milliseconds); `req_wait_ms_last/max` and `req_svc_ms_last/max` (that age
split where the request was handed to raft); `handoff_us_*`+`handoff_samples` (loop queued the bundle
-> the worker started writing it); `wake_pre_us_*`+`wake_pre_samples` (worker posted -> the loop's next
round STARTED - the scheduler question) and `wake_us_*`+`wake_samples` (the same, measured to the END
of the round that consumed it, so it also contains that round's work).  Real clocks appear only in the
driver and the workers' own stamps; the core still sees time solely through `k_server_advance`.

VM, 100 runs, 11 spikes over 8 ms (client maxima 8068-12759 us):
  - The server owns it.  11 of 11 spike runs had server-side holds, and `req_svc_ms_max` (12-13 ms)
    matches the client-visible maximum (12.7 ms) essentially exactly.
  - No single flagged event explains it.  Across those 11 spikes a slow sync fired once, a slow
    loop->worker handoff once, a slow pre-wake never, and the loop's own work stayed at or below
    1.1 ms (`slow_rounds` 0).  Individually each component is small; what is left is the request
    queueing behind other batches, which is the inherent cost of group commit at this offered load
    (2000 in flight at k=32 is ~62 batches, and a batch's last member waits for the whole batch).
  - Holds are rare: 362 of 200000 requests (0.18%) were held longer than 5 ms, and the spikes arrive in
    bursts of consecutive runs (48-54, 72-84) with the cumulative maxima unchanged across a burst -
    i.e. the machine was in a slow phase, not the engine in a bad state.
  - Platform: the same handoff measurements read sub-millisecond on Windows and up to 5.8 ms in this
    VMware guest, consistent with a loaded 4-vCPU guest rather than with an engine defect.
  Practical consequence: the tail tracks offered concurrency, not the storage path.  Where it matters,
    lower k / lower in-flight depth (measured medians: `mem://` k=64 -> 653 us; disk k=16 -> 2.8 ms).
  **RETRACTIONS.**  (1) An earlier revision excluded "the cross-thread wake" on the strength of
  `wake_us_max` reading 0 on the Linux box - but that 0 was never a measurement: only `code/kserver.h`
  had been copied there, so the driver half of the accounting was absent from the binary while the
  field still printed.  Both counters now carry sample counts and a run aborts unless they are
  non-zero.  (2) A revision after that blamed the wake ("5578 us") - that number came from an
  instrument that ran to the end of the consuming round, so it contained the round's own work; the
  corrected `wake_pre_us_max` reads 1.3 ms, and the wake is NOT the mechanism.
  Still open: how much of the queueing can be shaved without giving up group commit (a scheduling-side
  instrument, not another counter here).


---

## P1. Windows XP SP3 + MSVC 6.0: first real-machine verification (four defects found and fixed)

The hard constraint (C89 / MSVC 6.0 / Windows XP+) had only ever been supported by inspection - the
`#if defined(_MSC_VER)` branches existed, but nothing had compiled them.  A Windows XP SP3 VM with
Visual C++ 6.0 (cl 12.00.8804) plus the Windows Platform SDK was used to build and run the project
for real (bootstrap over TFTP: the host's SMB1 client is gone and the guest exposed only 135/139/445).
Four real defects surfaced, every one of them invisible to MinGW-w64 and to Linux/gcc:

1. `code/treap.h`: `(double)` applied to a `treap_u64` counter.  cl 12.00 cannot convert
   `unsigned __int64` to `double` at all (`error C2520: ... not implemented, use signed __int64`),
   at both operands of the average-length computation.  Fixed by giving the mechanism layer its own
   `treap_i64` next to its existing `treap_u64` and casting through it; a `grep -n '(double)'` over
   `code/` now returns only casts routed through a signed type.
2. `code/kdbctl.c`: MSVC 6 has no `snprintf` (only `_snprintf`).  The object compiled with C4013
   ("undefined; assuming extern returning int") and then failed to link (`LNK2001 unresolved
   external symbol _snprintf`).  Fixed by using the project's own `k_snprintf`.
3. Build recipe: `/MT` is required (VC98's `process.h` only declares `_beginthreadex` in the
   multithreaded model).  This lives in the XP build script, not in the repository.
4. `code/cemon.h`: `WSA_IO_PENDING` is **10035** (= `WSAEWOULDBLOCK`) in VC98's `winsock2.h`, not
   997.  Measured inside the failing branch with a temporary probe:
   `err=997  WSA_IO_PENDING=10035  ERROR_IO_PENDING=997`.
   The pending test therefore rejected a perfectly normal pending `AcceptEx`, the listener died at
   startup, and the report read `peer listen on 127.0.0.1:7102 failed (port in use or not
   permitted; socket code 997)` - a message that points at the port and away from the cause.  Fixed
   by comparing against `ERROR_IO_PENDING` in the single helper `cemon_win_iocp_pending`, now used by
   every overlapped post site (accept and connect).  Remaining mentions of the macro are comments.

Long-lived diagnostics kept (not temporary instrumentation): the listen path records a named fatal
for each sub-step - `AcceptEx`, `GetAcceptExSockaddrs`, `ConnectEx`, `bind/listen`,
`CreateIoCompletionPort` (reported with `GetLastError`; it is not a Winsock call), `accept slot
socket`, `accept slot AcceptEx` - and startup failure prints `last fatal: <site>; socket code <code>`.
That is what turned several rounds of "port in use or not permitted" into one line naming the branch.

**RETRACTIONS.**  Two intermediate conclusions of mine were wrong and are withdrawn: (a) that the
missing declarations meant the Platform SDK was not on the include path - it was, and the SDK does
declare all four symbols; the real cause was a header that *does* define the macro, with the wrong
value.  (b) that a socket code of 997 "proves" the pending case and therefore rules out the AcceptEx
branch - 997 is `WSA_IO_PENDING` only in modern headers, and using the modern value as a
discriminator excluded the branch that was actually at fault.

**Evidence** (all from the XP machine, sources md5-verified against the repository before transfer):
`cl` rc=0 for both translation units, `link` rc=0 for both, `dir /b *.exe` lists `kdbsvr.exe` and
`kdbctl.exe`, and the run reproduces the host baseline line for line: `SET alpha hello` -> `ok`,
`GET alpha` -> `hello`, `DEL alpha` -> `ok`, `GET alpha` -> `(not found)`.  The same tree passes
`./build.sh regress quick` locally (`REGRESS|quick|pass=9 fail=0`) with 0 errors and 0 warnings on the
pinned gcc.


---

## P2. Windows XP + MSVC 6.0: the test tier, and the unit suites on the real target

Continuing P1, the XP machine was used to compile and run the project's own test programs.  The four
unit suites now pass on the target, matching the host baseline exactly:

```
SUMMARY: 5/5 passed       cemon_test     run_rc=0
SUMMARY: 16/16 passed     kclient_test   run_rc=0
SUMMARY: 221/221 passed   raft_test      run_rc=0
SUMMARY: 33/33 passed     kserver_test   run_rc=0
```

Five more defects had to be fixed to get there.  Every one of them is invisible to MinGW-w64 (it ships
winpthreads and a C99-complete compiler) and to Linux/gcc:

1. `tests/cemon_test.c` included `<pthread.h>` unconditionally.  On Windows that header comes from
   winpthreads, not from the project, and MSVC 6 does not have it at all ("fatal error C1083: Cannot
   open include file: 'pthread.h'").  The probe needs two primitives and nothing else, so the file now
   provides the Windows side itself (`_beginthreadex` + `WaitForSingleObject`) and includes pthread.h
   only where it exists.
2. `tests/kclient_test.c` used `vsnprintf`, which MSVC 6 does not have (C4013, then a link error).
   Now guarded, using `_vsnprintf` on MSVC.
3. `tests/kserver_test.c` used `unsigned long long`, `ull` literals, `%llu` and `strtoull`
   ("error C2632: 'long' followed by 'long' is illegal", "bad suffix on number").  Converted to the
   test header's own `test_u64` / `TEST_U64_FMT` / `TEST_U64_C`, and the decimal parse is now a
   hand-written loop: MSVC 6 declares neither `strtoull` nor `_strtoui64` (C4013, then an unresolved
   `__strtoui64`), and the name differs across CRTs, so the test no longer calls one.
4. `code/cemon.h` called `SwitchToThread()` without a visible declaration on that toolchain (C4013),
   so VC98 assumed an int-returning cdecl extern and the linker asked for `_SwitchToThread` while
   kernel32 exports the WINAPI form `_SwitchToThread@0`.  The function is now declared explicitly
   under `_MSC_VER<=1200`.
5. Harness bug of mine: the XP test script passed `/Fo:name.obj`; MSVC's `/Fo` takes no colon, so the
   object was never produced and every link failed with LNK1181.  Also, the first revision wrote
   `%errorlevel%` inside a parenthesised block, which expands at parse time - it printed
   `build_rc=0` while every compile had in fact failed.  That is the project's own rule about never
   interpreting a run before confirming the build, applied to the harness itself.

**Debt, and the rule that now holds it.**  Each file defines what it needs locally rather than sharing
a header: `tests/test.h` carries the unit-test layer (`test_i64`/`test_u64`, `TEST_*_FMT`, `TEST_*_C`,
`test_strtoull`), and `tests/raft_fuzz.c` is fully converted with its own `fuzz_i64`/`fuzz_u64`/
`FUZZ_I64_FMT`/`FUZZ_U64_C`.  A shared `tests/t64.h` was tried and removed by review: a fuzz driver
that includes `test.h` only for the types gets five `-Wunused-variable` diagnostics for the harness
state (measured, not assumed), and a `TEST_TYPES_ONLY` switch inside the header was more machinery
than the problem deserves.  **Conversion complete:** the raw spellings no longer
appear anywhere under `tests/` outside the four sanctioned definition blocks, and the ratchet sits at
7 for exactly those (`tests/test.h` 2, `tests/raft_fuzz.c` 2, `tests/raft_cluster_fuzz.c` 2,
`tests/lincheck.h` 1).  Files that already include `code/kbase.h` use the project's own
`k_u64`/`K_U64_FMT`/`K_U64_C` (`kserver_cluster_fuzz.c`, `cemon_stress.c`, `bench_persist.c`); the two
that include only `code/raft.h` or stdio carry their own four-line layer.  The fuzz tier then went further than planned: on the
guest all three drivers now compile, link and run with every rc zero - `raft_fuzz 0 200` -> `done: 200
iterations`, `raft_cluster_fuzz 1 2` -> `done: 2 iterations`, `kserver_cluster_fuzz 1 1` -> `done: 1/1
clusters consistent` plus `linearizability: 4 histories / 253 ops decided by the checker, 0
inconclusive`.  That took one more link defect (`LNK2001: unresolved external symbol _strtoull` in
kserver_cluster_fuzz.c - MSVC 6 has neither strtoull nor _strtoui64, so its seed parse is hand-written
now), which is the same class the unit tier had already paid for.  `bench_persist.c` remains the one
test program outside the set: it includes `<pthread.h>` and needs a port, not a rename, and it is a
benchmark rather than part of the gate chain.  The gate's rule 9 (`no new C99 64-bit spellings in tests/`) is a ratchet at that
budget: it may shrink as files are converted, never grow.  **Retracted (2026-09b review).**  That rule was certified by injection at the time it was written,
but **it does not hold now**: the slice stayed at `sites[162:]` while the message in front of it walked
161 -> 127 -> 122 -> 60 -> 13 -> 7, so the enforcement threshold is still 162 and up to 155 new sites pass
silently.  Verified by injecting `unsigned long long probe = 1ULL;` into `tests/kclient_test.c` and
watching the gate print `PRINCIPLES|OK|rules=17 fail=0`, rc=0.  The earlier claim in this very entry - that
the ratchet had reached its floor and refused growth - was therefore false in effect.  See
`doc/code-review-2026-09b.md` item A1; the fix is to make the threshold one value (`len(sites) <= budget`)
instead of a message plus an unrelated slice, and to re-certify by injection.  Note a trap for whoever lowers the budget: a raw `grep` counts 236, because it also
matches comments; the budget must come from the checker's own count (157), or it leaves ~80 sites of
slack for new violations.


## K. Full-project review 2026-09b: retractions and newly verified gaps

Artifact: `doc/code-review-2026-09b.md` (scope, method, evidence and recommendations per item).  What
backs it: the top-tier gate was run on the reviewed HEAD and passed - `REGRESS|full|pass=13 fail=0
duration=307s` with the 24-round soak - followed by six read-only module audits whose findings I then
re-opened and verified individually.  Everything below is verified at the cited line or by an experiment;
items I could not verify are listed separately in that document (§3) and are not presented here as facts.

**Retracted in this review (my own errors).**

1. **The rule-9 ratchet "is at its floor of 7 and was self-certified" - wrong in effect.**  The slice is
   `sites[162:]`, so the enforcement threshold never moved while the message walked down; the ledger entry
   above is corrected in place.  Verified by injecting a C99 64-bit spelling and watching the gate stay
   green (A1).  This also retracts the accompanying advice that a raw grep leaves "~80 sites of slack":
   the slack was real and larger, and it was in the rule itself.
2. **"`mem://` + thread runtime fails every write" - my instrument lied.**  The probe compared a line
   ending `ok\r\r\n` against `ok`, so ten successes were scored as failures.  The combination works
   (a healthy single-node leader in STATS); the mem backend's lock-free global inode table stays a
   *structural* finding, not an observed failure (C3).
3. **A module audit's claim that CI's LF assertion makes CI permanently red - refuted.**  The CI job log
   shows the step passing *vacuously*: there is no `git` on the MSYS2 PATH, so it checks 0 files and prints
   `LF ok (0 tracked files checked)`.  The invariant is still enforced inside the gate's `principles`
   layer (the checker resolves git by absolute path; running it with git removed from `PATH` still yields
   `rules=17 fail=0`).  The defect is a green line that means "checked nothing" (F1).
4. **"A do-nothing fuzz can wear the same green line - partially refuted.**  True for `raft_fuzz` and
   `raft_cluster_fuzz` (`done: 0 iterations` passes both); false for the gate, which goes red because the
   linearizability layer requires a non-zero decided-history count (`./build.sh regress fuzz 0 0 0` ->
   `pass=11 fail=1`).

**Newly opened (verified; fixes not applied - the review was the deliverable).**

| id | sev | one line | evidence |
|---|---|---|---|
| A1 | high | rule-9 ratchet advertises 7, enforces 162 | `check-principles.py:180` + injection experiment |
| B1 | high | `durable_index` is the one frontier field never clamped on a log cut, and the leader's commit self-count reads it | `raft.h:1135-1143` vs `:2003-2006`, `:2436-2456`, `:4360` |
| C1 | high | `thread_create` returns `-1` from a pointer-returning function on the POSIX branch; caller's null check passes | `runtime.h:439, 462-466`; `kserver.h:4999-5005` |
| D1 | high | five teardown sites close the socket then write `conn->sock=0` after the inline CEMON_CLOSED callback already freed the conn | `kserver.h:3486-3532`; `cemon.h:1505, 2629-2655, 3572`; `kserver.h:2235` |
| A2 | medium | the "no `//` comments" rule scans comment-stripped text and can never fail | `check-principles.py:65-68, 78-84, 129` |
| A4 | medium | four unit layers accept `SUMMARY: 0/0 passed` (no third layer covers it) | `build.sh:373-376`; `tests/test.h:178-183` |
| A5 | medium | soak layer checks one of the two documented criteria | `build.sh:392` vs `doc/testing.md:58, 231, 245` |
| A6 | medium | `stall_hunt.sh` cannot run (`$ROOT` never assigned under `set -u`) and describes an abort it does not implement - while both docs cite it as evidence | `tools/harness/stall_hunt.sh:13, 22`; `grep -c 'ROOT='` = 0 |
| A7 | medium | the vfs injection seam is unused by every test (`grep` = 0), so disk-I/O failure paths are uncovered | `tests/` grep; `code/vfs.h:423` |
| B2 | medium | the heartbeat ACK fast path skips the persistence gate the reject path applies | `raft.h:3689` vs `:3747-3751` |
| C2 | medium | the uninitialised `key` read the same file documents and fixes elsewhere still exists at the other completion site | `cemon.h:3256` vs `:2994` |
| C3 | medium | `mem://` is a lock-free process-global while the default runtime is threaded | `vfs.h:222-225, 287-344, 414-422`; `kserver.h:4999` |
| D2 | medium | `k_server_maybe_finish_stop` has no latch and is reached three times per advance, so `on_stop` can fire up to three times | `kserver.h:4236-4239, 4295, 4503, 4565` |
| D3 | medium | `k_server_release` frees connections before the requests that dereference them | `kserver.h:4854-4870, 1977-1980` |
| D4 | medium | `release` clears `write_*` but not `gate_*`, so a stop inside the FCALL window can double-free | `kserver.h:4875-4877, 2005-2011, 2985-2989` |
| D5 | medium | every `k_server_drive` failure becomes a silent stop: no reason, no `fatal`, exit code 1 with `exited: fatal=0` | `kserver.h:4800, 4459, 4557, 4584, 4604`; `kdbsvr.c:436` |
| D8 | medium | the `fatal: snapshot failed` branch is dead code (`k_server_maybe_snapshot` never returns non-zero) and the snapshot failure paths print nothing | `kserver.h:4801-4804` + all `return 0` in the body |
| D9 | medium | a follower's snapshot-chunk write failure is answered with a silent disconnect, not fail-stop, so the node retries forever | `kserver.h:2342-2344, 3499-3501, 2269` |
| D10 | medium | WAL metadata with `generation==0` is accepted as "nothing to restore" and new writes overwrite segment 0's first record | `kserver.h:1549, 1900-1914, 4996` |
| D11/D12 | medium | the cross-segment continuity check depends on a `clean_end` that only "zero bytes" can set, and three scan break paths are silent | `kserver.h:1582-1597, 1667-1670` |
| D13/D14 | medium | silent `-1` paths in `kdbsvr` under a caller that says "see the fatal line above"; `kdbctl` drops trailing argv silently and can execute a different command | `kdbsvr.c:380, 397, 446, 455, 416`; `kdbctl.c:726-733, 322` |
| D15 | medium | `sscanf("PIPE %u %u %7s %64s %255s", ..., prefix[64])` can write 65 bytes | `kdbctl.c:453, 462`; `kproto.h:19` |
| E1 | medium | `kdbctl` hangs forever when stdin is not a TTY (measured: rc=124) although the CLI claims scripted use works | experiment; `cli.h:731, 602-606` |
| E2 | medium | an FCALL name over 64 bytes is accepted by the client and answered with a disconnect by the server | `kserver.h:3342`; `kproto.h:269-270`; `kdbctl.c:359` |
| E3 | medium | the one-shot client reports "the command was not executed" where the library says "outcome unknown" | `kdbctl.c:678, 687-692`; `kclient.h:480-481` |
| F1 | medium | the CI LF step checks 0 files and prints `ok` | CI run `35486663891` job log; `ci.yml:47-55` |
| F2 | medium | the nightly's summary pattern matches every line and its issue body quotes only the gate's (green) lines, hiding a failing coverage/UBSan step | `nightly.yml:71, 93-94` |
| F3/F5/F6/F7/F8 | medium | the ledger's "complete" unhandled list, the principles doc's numbers, the testing doc's broken table and stale counts, the old review's self-contradiction, and the automation plan's private/public conflict | see `doc/code-review-2026-09b.md` F3-F8 with line references |

**Not claimed:** the reachability of B1 in a production driver, of D10's metadata state, and of the
`mem://` race; the exploitability of C2 on XP; anything about the kanban cards.  See §3 of the review.

**State of the gate:** unchanged and green at the reviewed HEAD; no fix from this review has been applied
yet, deliberately - the audit and the remediation are separate batches.

### Fix status of the §K findings (2026-09-20, same day)

Fixed and gate-verified (`quick` + `fuzz` green before each push; every rule change was certified by
injecting a real violation, watching exactly that rule fail, and reverting precisely with a zero-residue
check):

| commit | items | what changed |
|---|---|---|
| `0a23ae0` | A1, A2, A3, A4, A5, A6 | the rule-9 budget lives in one variable and the measured count is printed (injection: 7 -> 8 = FAIL); the `//` rule now scans literal-stripped text so it can actually fail; the four unit layers require a non-zero numerator; the two raft fuzz layers require at least one iteration; the soak layer greps one `SOAK\|PASS` verdict line (proven by a short run, with a FAIL negative control); `stall_hunt.sh` gets a working base path, a real abort and a verdict line |
| `0a23ae0` | D1 (x6), C1, C2, D15 | all six teardown sites stash-clear-close instead of close-then-clear (the callback frees the conn inline); `thread_create` returns 0 from the pointer path on POSIX; the completion key is initialised; `%63s` into the 64-byte prefix |
| `cdbeaaf` | F3, F5-F11 | the docs that claimed things the tree or the tool does not do: principles' numbers, testing's broken layer table + stale scope + duplicate section, README's missing gate entry point, the old review's self-contradiction (banner), the plan's private/public conflict, the ledger's stale C11/C12/C13 rows and its drifted line references |
| `ef557ee` | D2, D3, D4, D5 | `release` frees requests before the connections they dereference; the FCALL gate fields are cleared; `maybe_finish_stop` has a latch; a failing `drive` prints a reason and marks the server fatal instead of stopping silently |

Still open, each needing a decision rather than an edit: **D8/D9** (snapshot failure visibility: the
`fatal: snapshot failed` branch is dead code and a follower's chunk-write failure is a silent disconnect
loop), **D10/D11/D12** (WAL recovery: metadata with `generation==0` accepted as empty, the continuity check
gated on a `clean_end` only "zero bytes" sets, three silent break paths), **B1** (`durable_index` not clamped
on a log cut - the one that could present as committed data lost), **B2** (heartbeat ACK skips the
persistence gate), **D13/D14/D16-D19**, the E-series (client) and F2 (the nightly's misleading issue body),
plus the coverage decisions A7/A9/A10/C3/C4/C7.

### B1 and B2 fixed (same day, second batch)

- **B1** `raft.h`: `raft_durable_clamp` now lowers `durable_index` as well.  Its own field comment already
  required that ("clamped to the log tip at report time, and lowered by any truncation"), so the change
  restores the documented contract rather than deciding a new one; the leader's `require_durable`
  self-count reads that field, which is why a stale high-water could commit an entry that was not on a
  majority's disk.  Basis cited in the message: Sec. 3.8, Sec. 5.4.2 and Ongaro 10.2.1.  Regression:
  `raft_fuzz 1 20000`, `raft_cluster_fuzz 1 2000`, a 10-cluster `kserver_cluster_fuzz` (40 histories /
  2,509 ops decided, 0 inconclusive), plus the fuzz gate (`pass=12 fail=0`).  Reachability in a production
  driver is still not demonstrated - the fix is correct at the contract level either way.
- **B2** `raft.h`: the empty-heartbeat fast path now applies the same two-part persistence gate as the
  reject path (`persist_needed || persist_gen < term_dirty_gen`), so no response advertises a term that is
  still in memory (Sec. 3.8).  The comment in the file records that a *different* narrowing once stalled
  the leader's read barrier on `raft_cluster_fuzz` seed 869; that seed range was re-run explicitly
  (`raft_cluster_fuzz 800 200`), together with a release-sized `1 2000` and a 10-cluster
  `kserver_cluster_fuzz` sweep - all green, so the wider gate is kept rather than documented as a
  deviation.

### ① WAL recovery strictness (D10-D12) - fixed

Rule adopted: **only a torn tail (an incomplete final record) is tolerated**; every other anomaly is
fail-stop with a printed reason.  That is etcd's decoder rule (it refuses to continue past a CRC mismatch)
and the conservative end of RocksDB's `kTolerateCorruptedTailRecords`, chosen over
`kPointInTimeRecovery` because this WAL is also Raft's durability layer: a mid-history cut amputates the
Raft log, and if a majority of nodes were cut the same way (one bad disk batch) a truncated majority could
elect and lose committed data.

Changed:
- metadata with `generation==0` no longer means "nothing to recover": the slot is fsynced only every 64
  records, so a healthy store inside its first 63 records - or one whose slot was lost - has it at 0, and
  recovery used to start from an empty tree and then write over segment 0's first record.  Without
  metadata the segments are scanned instead.
- magic/version mismatch, illegal payload size, `gen==0`, an intra-segment generation gap, a cross-segment
  gap after a cleanly-ended segment, and a CRC mismatch are all fatal now (each with its own message);
  they used to break out of the scan silently and the only fatal test was "the hole is past the newest
  record", so a hole in the middle of history was accepted.
- a torn header/payload still ends the scan quietly-but-not-silently: both now print.
- the `have_bad`/`bad_gen` machinery and its final check are gone with the leniency they implemented.

Two defects in the first attempt, found by probing rather than by reading:
- the scan used `vfs_open` as an existence test, but that opens with `OPEN_ALWAYS`/`O_CREAT`; with the
  metadata absent the loop therefore created empty segments without bound (83,643 of them in one run) and
  recovery never finished.  The existence test reads one byte now.
- the store is a set of files sharing the base as a PREFIX, not a directory: a probe that cloned it with
  `cp -r` cloned nothing, which is worth knowing when writing recovery tests.

Verified by probe (not by inspection): healthy restart keeps the data; a torn tail (last 7 bytes cut off
a segment) still recovers and prints `ends with a torn payload at offset ...`; a single flipped byte
inside a record refuses to start with `fails its CRC check (generation 1): refusing to recover` and exit
code 1; a corrupted metadata slot falls back to the segment scan and recovers.

### (2) Snapshot failure visibility (D8/D9) - fixed, and one review claim corrected

Correction first: D8 described the snapshot failure path as a silent unbounded retry.  It is a retry BY
DESIGN - `k_server_set_snapshot_failed_baseline` records the WAL generation and `last_applied` so the
snapshot policy can fire again against the CURRENT state, and `snapshot_failed` is already exported in
STATS.  What was actually missing was the CAUSE: every failure in the worker collapsed into `ok=0` and
every failure in the consumer collapsed into "clear the flag and try later", so a store that could not
write snapshots was indistinguishable from a healthy node until the WAL filled the disk.

Changed:
- `k_snapshot_worker_save` names the cause and prints it: path build failure, open failure, header write,
  treap streaming, trailer write, fsync, read-back of the trailer, bytes past the trailer.
- the consumer prints that the index did not persist and that it will retry when the state advances
  (the line above names the cause).

Verified by probe: the prints are on the failure paths, and the paths themselves are covered by the fuzz
tier (which drives snapshot capture/save/cleanup through the disk-image model).

### (3) auto_replace - made audible

`auto_replace` is Sec 4.4 add-before-remove replacement of a voter that missed `auto_replace_threshold`
heartbeat rounds: leader-only, driven by `ready.peer_health`.  The shape is right, but no decision, ADD,
REMOVE or phase transition was logged, so an automatic failover left no trace.

Changed: the decision prints once per change of target; each of the two submission failures prints (they
reset the phase, so they repeat until they succeed); the phase-1 -> 2 transition prints, the completion
prints, and the REMOVE submission failure prints.

Verified end-to-end with the real binaries - three nodes on 9001/9003/9005 with
`--auto-replace-threshold 8`, the third voter killed with taskkill:
```
auto-replace enabled: replacement=4@127.0.0.1:9007:9008 threshold=8
auto-replace: voter 3 missed 8 rounds; replacing it with node 4 (add first, then remove)
```
and the two survivors kept serving (`SET k2 = ok`, `GET k2 = v2`).

Open visibility gap, found by that same probe: the reaction needs ~8 heartbeat rounds, which took >24s
here, and once the decision is logged the phase-1 wait is silent - with the replacement node never
started, `auto-replace: node 4 is committed as a voter` never appears, so an operator sees one line and
then nothing.  Named cause: the replacement must catch up before the ADD commits, so a replacement that is
not running stalls the swap.  Fixing that needs a rate-limited reminder (a new field), so it is recorded
here rather than half-done.

### (4) Unverified platforms (C4) - Linux added to CI, macOS deliberately not added

CI only ever compiled the Windows/MSYS2 path, so the POSIX branches of `runtime.h` (including the
`return 0` fix in C1), `cemon.h` and `vfs.h` were never built by any automated job.

Added a `linux-gate` job to `ci.yml`: LF assertion, `chmod +x` for the harness scripts (a checkout made
on Windows does not carry the executable bit), then `./build.sh regress quick` on ubuntu-latest.  It runs
without a log publisher and without the issue reporter on purpose - the Windows gate owns both, and two
reporters would fight over the same issue title.

macOS was NOT added, although the plan said "compile only".  A compile job that cannot fail is not
coverage: the kqueue path in `cemon.h` has never been compiled anywhere, and from this machine I cannot
iterate on a macOS-only failure.  Adding it would either put a permanently red check on main or a job
wrapped in `continue-on-error` - a check that never ran pretending to be a pass.  Open item: add a
macOS compile job that is allowed to fail once someone can iterate on it.

### (5) The storage injection seam (A7) - used for real

The project declares four seams for fault injection (transport / elapsed_ms / runtime backend / vfs
backend).  Three were used by tests; the vfs one had no user at all, so "the seam exists" was an
assumption - every storage failure path was reasoned about rather than exercised.

`tests/vfs_fault_test.c` installs a wrapper backend under the same scheme name as the stock mem backend
(disk routing stays reachable), delegates every operation to mem, and re-points each file it opens at
itself, so every later vfs_write/vfs_sync on that file comes back through the wrapper and can be made to
fail on demand.  No new API was needed: the backend list and the router are file-local statics of the
single header, so a test in its own translation unit can substitute one.

Four cases, all asserted on content rather than chatter:
- no fault: the wrapper is really in the path (opens/writes/syncs all non-zero - a 0-sample would prove
  nothing) and nothing fail-stops;
- every fsync fails from now on: the WAL flush must fail-stop (observed: `writes=7 syncs=5 injected=1
  fatal=1`, plus the server's own `fatal: WAL write failed`);
- every write fails from now on: same class, same verdict;
- a fault during open: `k_server_open` reports the failure (`open_rc=-1`) instead of starting anyway.
  No fatal flag is asserted there on purpose - a failed OPEN is reported to its caller, which is the
  loud signal at that stage; asserting a flag the product does not set would be testing the test.

Wired as gate layer L1g (`reg_gate vfs_fault_test`), so `quick` is 10 layers and `full` 14; docs updated.
(Later the same round: the `regress_selftest` layer was added, so the counts are now `quick` 11 / `fuzz` 14 /
`full` 15 - see the E5 entry below.)
**Update (same round, later):** that test HAS now run on the XP guest - it is one of the five new cases in the
41/41 `kserver_test` line recorded in `doc/testing.md` section 7, so the deferral above is closed rather than
forgotten.

### Linux gate (4) - first real run found a rule that was wrong, not a port

The first Linux run failed, and not on the code: the LF step added to `ci.yml` reported 400+ violations,
every one of them `doc/measurements/*` with `attr/-text` - the project's documented exemption, where the
bytes ARE the observation and are deliberately never converted.  That step was a second copy of a rule
`tools/check-principles.py` already implements WITH the exemption (and which runs inside `regress quick`
on both jobs).  The duplicate was removed: a copy of a rule without its exemption is not a stricter
check, it is a wrong one.  The Windows gate passed on the same commit, so the code itself is fine.

### (6) "A new command type touches nine places" (E10) - now a mechanical rule

Before: the knowledge lived in a maintainer note, and forgetting a place is silent - a response body
that no client branch prints is swallowed and the client answers a bare "ok".

Now `tools/check-principles.py` rule 9 keeps an explicit, frozen table of every `K_REQ_*` declared in
`kproto.h` and the files that must mention it.  Two things make it more than a list:
- a type declared in `kproto.h` that is NOT in the table fails the rule, so a new command cannot be
  added without stating, on purpose, which places it needs;
- every place the table names must really contain the type, so the table cannot rot.

The table was written from the tree, not from memory: only `kproto.h` and `kserver.h` mention all 21
types; `K_REQ_SET`/`K_REQ_DEL` are absent from `kclient.h` (their response body IS a bare status, so
there is no print branch) and `K_REQ_RGET`/`K_REQ_MEMBER` are absent from `kdbctl.c` (both are reached
through kclient helpers).  Those four entries carry the reason inline.

Proved able to fail both ways, with byte-exact restores and an empty `git diff` for the injected files
afterwards: adding `#define K_REQ_ZZTEST 22u` to kproto.h gives `PRINCIPLES|FAIL|rules=18 fail=1`, and
listing `K_REQ_SET` as expected in `kclient.h` does the same.

### (4b) What the first honest Linux runs actually found

The Linux job was added to catch "a branch nobody compiles", and it did - but every finding so far has been
in the GATE, not in the engine.  All nine test layers pass on Linux (raft 221/221, kserver 33/33, kclient
16/16, cemon 5/5, vfs fault 4/4, selftest, cli_smoke, principles rules=18).

Found and fixed:
- `GATE|build|FAIL ... errors=19` with `rc=0`.  The 19 are real: `tools/proc_time.c` drives the Win32
  process API and has no POSIX branch, so Linux reported 19 errors for it - and `build_tools` returned the
  status of its LAST compile, so the failure never reached rc.  The tool is now built only on Windows (it
  is a measurement tool, not something to port) and every line of `build_tools` propagates its own status.
- The `errs` counter used to match the WORD "error:" anywhere, which is what made a green Linux build look
  red.  (My first theory was SIGPIPE noise from `grep: write error: Broken pipe`; reading the artifact
  refuted it - the errors were real compiler errors.  Retracted.)
- Both CI gate steps read `./build.sh regress <mode> 2>&1 | tee regress.log`, so the step's status was
  tee's: a RED gate reported a GREEN step and no failure log was published.  Both now set -o pipefail.

Still open, with the evidence:
- The Linux `selftest` fails intermittently at `selftest: remove-leader node start failed` (`server4->
  started != 1` after the store files were already created), and then the cleanup layer correctly reports
  2 leftover files.  The scenario bases are all distinct (15000/21000/26000/30000/32000/35000 with
  different moduli), so a scenario-to-scenario port collision was checked and REFUTED.  What fits the
  evidence now: the two highest bases can reach 34999 and 44999, i.e. past 32768 where Linux starts its
  ephemeral range, so a bind can lose to an outgoing connection - intermittently, and not on Windows
  (dynamic range starts at 49152).  All scenario bases now live in disjoint windows below 32768 (the move
  landed in a second commit: the first attempt's helper script died on a bad regex callback BEFORE writing
  the file while the commit still went through, so for one commit this paragraph claimed a change that was
  not there - recorded rather than quietly rewritten).  The fix is NOT yet verified: it can only be
  observed on Linux, and the next run of that job is the test.  The auto-replace lines this session added are what made the failure visible at all: they show the
  whole add -> remove -> done sequence immediately before it.

### (3b) auto_replace reaction time - my own earlier measurement retracted, and what the real gap is

Measured with the real binaries (3 nodes, `--auto-replace-threshold 8`, the third voter killed):

```
t0+1s: node 4 已是 pending（决策已发生、ADD 已提交）      <- polled every second via TOPOLOGY
[before kill] au1.log = 121 bytes      <- only the two startup lines
[after 1s]    au1.log = 211 bytes, auto-replace lines = 1  <- the decision line is there while the
                                                             process is still RUNNING
```

Retracted: the reaction time I reported earlier ("8 rounds took >24s", "~3s per round", "silent for 24-60s").
Those were artifacts of my own probe (polling/grep timing), not product latency.  The decision fires at
about `threshold x K_HEARTBEAT_MS` (50ms), i.e. about 1s at threshold 8, and the ADD is submitted in the
same step.  Retracted too: the idea that stdout buffering hides the lines - the log grew while the server
was alive, with the line present.

What the real gap is, stated precisely: `TOPOLOGY` already exposes every pending catch-up target
(`<id>@host:port role=pending`), so an operator CAN see that a replacement is stuck.  What is missing is
the story around it - no line when the wait STARTS (`pending` was already true at t0+1s, so the reason is
that node 4 does not exist yet), no line while it lasts, no reason/age/progress, and no counter anywhere
that says "a voter has been unreachable for X while a replacement is pending".  The wait is unbounded on
purpose (a config change that is not committed must not simply be abandoned - Ongaro Sec 4.1), so the fix
is observability, not a give-up timer.

### (5b) The storage injection seam ran on the XP guest

The seam test added for A7 (`tests/vfs_fault_test.c`) now builds and runs on Windows XP SP3 with cl
12.00.8804 + the Platform SDK, which is what the plan for item 3 asked for.  Evidence from the guest's own
uploaded logs (`D:\kynovo-xp\inbox`):

```
GET failures: 0 (0 is required)          <- 23 sources + 4 scripts, each md5-verified over the wire (27/27)
build_exit=0   run_exit=0   tests_exit=0
vfs_fault_test_compile_rc=0  vfs_fault_test_link_rc=0  vfs_fault_test_run_rc=0
SUMMARY: 5/5 passed / 16/16 / 221/221 / 34/34 / 4/4 passed
done: 200 iterations / done: 2 iterations / 1/1 clusters consistent / linearizability: 4 histories, 0 inconclusive
```

Two supporting facts worth keeping: the guest ran the NEW scripts (it echoes `ver=G` / `ver=F`, the bumps
made for this run - a stale `xp_tests.bat` would have reported `ver=E` and no vfs block at all), and the
end-to-end run still reproduces the crash contract (`taskkill`, restart, `survive-me`) with its negative
control reading `(not found)`.  `kserver_test` reports 34/34 rather than the previously recorded 33/33
because the membership-visibility work added a deterministic case, and it passes here too.

### §M Full-project review 2026-09c

The review itself is `doc/code-review-2026-09c.md`: seven read-only module audits (raft / recovery+WAL+snapshot /
request+protocol+membership+shutdown / client+CLI / platform layer / tests+gates / docs+hygiene) over the tree at
`33b3eaa`, with every high-severity claim re-checked against the code by hand and each finding marked VERIFIED or
REPORTED.  Baseline: `REGRESS|full|pass=14 fail=0 duration=330s` on this machine, all fourteen layers green.

Status corrections this review forces (they were fixed, and the ledger still listed them as open):
- **D13-D19, F2** - fixed in `152786a`; **A9/A10** - fixed in `1b3f813`; **C4** (a `k_server_open` failure
  printing its cause) - fixed in `9962d09` (`k_open_fail`, `code/kserver.h:5395`).  `doc/principles.md:47` still
  cited C4 as the open justification and has been corrected.
- The stale counts the review found (fuzz `pass=12`, `quick pass=9`, 4 unit suites, `kserver_test 33/33`,
  "22 sources") are corrected in the documents that state them.

Two findings in this review are **defects in my own work from this same day**, recorded as such rather than as
newly discovered pre-existing issues:
- **2.1** - the D10 recovery fix (`09e6a3e`) only closes the case where segment 0 still exists: with an unusable
  metadata slot and a snapshot-released prefix, recovery returns "never-written store" and starts empty.  It is
  open, and fixing it needs a read-only existence primitive because "fresh store" and "damaged store with a
  released prefix" are indistinguishable through the current vfs interface.
- **3.1** - the membership note written for step 3 of the same day's work never reaches the operator: the client
  print whitelist does not include `K_REQ_MEMBER`, so the CLI shows a bare `ok`.  Open; one line plus a
  regression test.

Also carried forward from this review as open, with the reasoning in the document: 1.1/1.2 (a failed send frees
the `k_conn` while the frame reader still uses it, and `rx_buffer_bytes` double-subtracts into a wrap), 2.2/2.3
(missing or unopenable segment, empty `.wal.meta`), 2.4 (snapshot cleanup can delete the rollback segment), 3.3/
3.4/3.5 (phantom pending after a refused change; the synchronous rejection path bypasses the new backoff; the note
and the source are server-global), 4.1 (a real C89 violation at `code/cemon.h:2584`, in a branch no gate can
compile), 4.2 (the mem backend's inode table is an unlocked process-global), 6.1/6.2/6.3 (three gates that go
green on nothing: `scan()` re-scoping to the whole library, the naive LF step still in the primary CI job, and
the `//`-comment rule blinded by apostrophes in comments).

## N. 恢复（第三轮审视 2.1/2.2/2.3）—— 已修，并记录一条保留的残留

- **2.1 元数据不可用 + 快照已释放前缀 ⇒ 静默空库启动（已修）**。原扫描在**第一个空段**就 `break`，于是被快照
  释放掉的前缀让恢复错过后面所有记录，`records==0` 直接当"从未写过的库"返回，历史被孤儿化、随后被覆写。现在
  空前缀只跳过不终止（有界：未用过的槽最多 2 段、没有元数据最多 64 段），恢复继续走到持记录的段。
  真机探针：删掉 `<base>.wal.meta` 后重启 ⇒
  `wal: the WAL metadata is missing under '<base>': rebuilt the index from 6 record(s) in the segments`，
  全部键读回如初（旧行为：空库启动）。
- **2.2 元数据指向的最新段缺失 ⇒ 静默缩短历史（已修）**。记录是随写随 fsync 的，元数据槽每 64 条才落盘，所以
  槽无法见证全部——段才是"已确认"的唯一权威。现在恢复到的最远位置若**早于**元数据里"最后一条已确认记录"的
  位置，一律 fail-stop 并指名位置。确定性单测
  `test_wal_recovery_refuses_missing_newest_segment`（小段配置 + 删掉持记录的最新段 ⇒ `k_server_open` 必须失败）；
  把该校验临时禁用即红（`35/36`，断言 `k_server_open(&s)!=0`）。旧行为是把写位置回退到更早的记录上继续跑。
- **2.3 零字节 `.wal.meta` ⇒ 静默当作新库（已修）**。零字节文件与"从未写过"在读取探针下同形，所以旧代码短路成
  新库。现在元数据"在但无字节"和"文件缺失"一样落到**段为准**的扫描路径：探针实测清零元数据后重启 ⇒ 同样是
  `rebuilt the index from 6 record(s)`，数据完整。同时把**行为一致性**补齐：文件在但解不出槽（短/魔数/版本/CRC
  ⇒ `k_wal_meta_load` 返回 -1）**保持 fail-stop**，但不再静默——新增原因打印
  `wal: the WAL metadata under '<base>' exists but holds no decodable slot (bad magic/version/CRC): refusing to recover`。
- **顺带修掉一条未被审视列出的潜在缺陷**：**合法**元数据 + 已被释放的前缀。旧扫描同样在第一个空段 `break`，
  于是快照清理过的库重启会走到 `wal: no usable record found in segments 0..N: refusing to recover` ——即"清理过
  前缀的库重启会在门里假红"。上面的空前缀跳过把这一类一并覆盖（单测 `test_wal_recovery_across_segment_rotation`
  在多段旋转下通过，探针的"健康重启"一例打印为空 = 无副作用）。
- **保留的残留（明确记录，不做）**：**完全没有元数据 + 已释放前缀长于 64 段 + 记录全在前缀之后**时，恢复会安静地
  按空库启动。要把这种情形和"确实从未写过"区分开，需要一个**只读存在性探针**；该层明确不提供（用户裁定
  `vfs 不需要 exists 接口`），因此这里用 64 段（64 MiB × 64 = 4 GiB 历史）作为"前缀还可能有多长"的上界，
  超界即安静停下。判断依据：**最新记录所在的段永远不会被快照释放**（清理只丢弃完全位于快照基点之下的段），
  **（第四轮撤回，2026-09-21）** "元数据存在时不会发生"是错的：天花板 `if(!records&&seg>=N) break;`（`code/kserver.h:1635`）与元数据是否存在无关，而 `if(!records){ return (meta_rc==0)?0:1; }`（`:1775-1783`）在 ack 检查（`:1789`）之前就返回成功。所以已释放前缀 ≥64 段时**带着元数据也会静默按空库启动**，元数据不被重建 ⇒ 之后每次重启都重复、期间写入也一并不可见。被证伪的推理原文是"清理永不释放最新记录所在段"（`:1780-1781`）—— 这句本身对，但它假定扫描能走到那一段，而天花板先把它拦住了。另：无元数据时的上界实现是 **2 段**（`K_WAL_SCAN_EMPTY_PREFIX_MAX_UNUSED_SLOT`，`:66`），不是 64 段。详见 `doc/code-review-2026-09d.md` A1（第四轮第一优先级，尚未修）。。
- 探针自身的两处失误已更正：把"单段库里移走最新段"当成 2.2（该段没有已确认记录 ⇒ 照常启动是**正确**行为，
  真正的 2.2 需要小段配置 ⇒ 做成单测）；以及 `GET` 取值时把客户端横幅当成返回值。

## O. 成员可见性说真话（第三轮审视 3.3/3.4/3.5）—— 已修

- **3.5 提交者身份是服务器级全局（已修）**。`membership_change_source` 被下一次提交直接覆盖，于是"谁提交的"
  会串台；而跟随者只是复制到 ADDR，却被标成它自己发起的变更。现在**来源挂在每条待定项上**
  （`pending_source[i]`：0 = 由复制来的 ADDR 学到、1 = 本节点 auto-replace、2 = 本节点的客户端请求），
  新增项在唯一知道真相的地方（提交点与 ADDR 应用点）打标，毕业搬运时随条目一起搬。
- **3.3 被拒绝的变更留下永久幽灵（已修）**。Raft 同步拒绝时，ADDR 应用已经往 `pending[]` 里放了目标，而没有任何
  代码会清它——于是 `TOPOLOGY` 永远显示 `role=pending`、`STATS` 的年龄无界增长。现在提交点先记下
  `pending_before=pending_count`，拒绝即回滚到该值并清计时；并且**只有本节点提交的待定项才会被报告**
  （STATS 的 `membership_pending`、`TOPOLOGY` 的 `role=pending`、限速日志）——跟随者不再为一个自己无法判断
  结局的变更背书。
- **3.4 同步拒绝绕过退避（已修）**。auto-replace 的三处提交点（决策里的 REMOVE、决策里的 ADD、committed 里的
  REMOVE）在 `k_server_submit_member` 返回非零时只把 `phase` 清零并打印，**不计入 `auto_replace_fail_streak`**
  ——于是每次心跳轮都会重试一次（与之前修掉的异步重试风暴同类）。现在它们计入连续失败，打印也按实际情形说明
  下一次尝试的时间。
- **顺带修掉 note 的静默截断**（审视 3.5 提到的 note 文本）：缓冲 128 字节，原措辞需要 131 字节，`k_snprintf`
  截断且无人知晓。改成实际长度 118 字节以内，并带上来源标签。
- **回归守卫**：`test_membership_wait_reporting` 增加断言——"外来的待定项既不累计年龄也不被报告"、"同一条目
  改由本节点提交后立刻开始累计并报告"、"等待结束后归零"。把过滤退回旧行为（任何待定项都报）即红：
  `35/36`，失败断言 `a foreign pending entry never accumulates an age here`。
- 真机（3 节点 + 打死一个选民）复跑：auto-replace 决策行与 `TOPOLOGY`/`STATS` 输出与修前一致，未见任何多余打印。

## P. 门的假绿（第三轮审视 6.1/6.3/6.4）—— 已修

- **6.3 `no // comments` 规则被撇号打瞎（已修）**。`strip_literals` 把**任何** `'` 当作字符字面量的开始，
  于是注释里一句 "doesn't" 就会开启一段"字面量"，一路吞到下一个撇号（可能隔着很多行），其中的真 `//` 注释
  全部从规则视野里消失（`raft.h` 一处就有 111 个 `//` 不可见）。现在只有**确实是**字符字面量的撇号才被当作
  开始（`'x'`、`'\n'`、`'\''` 的形状），散文里的撇号原样保留。注入自证：在某注释里写
  `/* ... doesn't want this // hidden ... */` ⇒ 规则**红**（`FAIL no // comments in C sources`），
  旧实现看不见它。
- **6.1 空清单把扫描扩大成全库（已修）**。`scan()`/`scan_raw()` 原来写 `for path in (files or SRC)`：
  空清单是 falsy，于是规则要么去查它从未为之编写的整个库，要么（对 git 派生的清单）**零数据变绿**。
  现在 `files is None` 才表示"全部"，空清单就是"没有"；并且新增一条规则
  `git-derived file lists are non-empty`：git 派生的清单为空即红。注入自证：把 `tests/*.c` 换成
  匹配不到的模式 ⇒ 该规则红。（`build/` 那条清单刻意不走这条规则：它为空才是对的。）
- **6.4 sprintf 棘轮名不副实（已修）**。消息里写死 `budget 13` 而切片也是 13，实测却是 **10** —— 三个免费违规。
  现在预算与消息都来自同一个常量 `BUDGET_SPRINTF`（取实测 10），并且 **ok 行也打印实测值**，这样"预算与树
  不再匹配"会在绿行里就看得见，而不是等到某天有人注入一个测试才暴露。
- 规则总数 18 → **19**（新增"git 派生清单非空"这一条），`doc/principles.md` 与门判定行同步。

## Q. 文档漂移：崩溃契约的四处代码引用（第三轮审视 7）—— 已修

`doc/crash-contract.md` 的表格是"崩溃在某一步会丢什么"的依据，它引用的**四个行号全部指错**：例如
`code/kserver.h:1914` 落在载荷编码里，而记录写入实际在 `k_wal_append_bundle`。逐条重新核对后的真位置：

| 表中所述 | 现在引用 | 真实语句 |
|---|---|---|
| 记录写入 + sync | `k_wal_append_bundle`（`code/kserver.h:2061`） | `vfs_write(header)` + `vfs_write(payload)` + `vfs_sync(file)` 同一个 `if` |
| 元数据槽写入 + sync | `k_wal_meta_store`（`code/kserver.h:2014`） | `vfs_write(next_slot*GAP,slot)` + `vfs_sync(file)` |
| 快照数据块（接收侧） | `k_server_write_inbound_snapshot`（`code/kserver.h:2432`） | 流式写块；仅 `snapshot_done` 时才 `vfs_sync` |
| 快照尾部 + 回读校验 | `k_snapshot_worker_save`（`code/kserver.h:4571-4581`） | 写 4 字节 CRC 尾 → `vfs_sync` → **回读**尾部比对 → 再探尾部之后是否还有字节 |

**顺带把这类缺陷按"类"处理**：引用一律改成 **`函数名` + `code/<file>:<line>`**，并在文档里写明"裸行号无法在
不读代码的情况下核对，且会随文件增删腐烂"；同时确认 `doc/testing.md` 里那条被门废除的 L0 规则描述已在更早的
提交里改对（现在描述的是 build 层的真实判据与日志路径 `build/regress/build.log`，与 `build.sh` 一致，已核对）。

## R. mem 后端与工作线程（第三轮审视 4.2）—— 拒绝保留，但**理由两次被推翻后重写**

**撤回（本轮用户追问后逐条核实）：**
1. ✗ **"WAL 与快照线程会互相破坏写入"** —— 不成立。写者是唯一的：段与元数据只由 WAL worker 写
   （`k_wal_seg_file`/`k_wal_meta_file`），快照文件只由快照 worker 写；主线程发送的是**当前**快照而 worker 正在
   保存的是**新**索引，入站安装的索引高于本地上次保存的索引 ⇒ **同一 mem 文件不存在并发写，也不存在并发读-写**。
2. ✗ **"同一 inode 的 close/unlink 生命周期竞争"** —— 也不可达。清理只删 `0 .. keep_segment-1`，而
   `keep_segment = snapshot_prev_wal_segment`（`kserver.h:4677`，即**上一个**基点所在段）；写入方缓存的句柄始终在
   **当前**基点所在段或更高（它持有最新那条记录所在的段，而记录是追加的、基点记录必然更早）⇒ 两条路径永不指向
   同一个 inode。

**核实后成立的（这条才是现在写进代码与拒绝信息里的理由）：**
`vfs_mem_open` 的插入（`vfs.h:339-362`，函数在 `:339`）与 `vfs_mem_unlink` 的摘链（`vfs.h:371-377`）都是对**共享桶头**的读-改-写；
`n->refcount`（`vfs.h:364` 的递增 / `:382` 的判定）同理。两个线程**同时**进入 `vfs_open`/`vfs_unlink`、且两个路径**撞进同一个桶**时
（`vfs_hash` 是 FNV-1a + 混洗的强哈希，`VFS_MEM_HASH_BUCKETS=1024` ⇒ 约 1/1024）会丢一次更新 ⇒ 某个文件"凭空
不在"。它需要 1/1024 **并且**微秒级同时，而 `mem://`+线程不是产品配置（手工压力工具已改走 `disk://`）⇒ 量级约
1e-9/次快照。**本机无 TSan/ASan**（MinGW 无运行时库），所以这条无法在此自动抓取——属"低概率缺陷"，按纪律不被
"单次干净样本"否定，也同样不被"一次推理"确认。

**最终处置（维护者裁定 B：加锁 + 恢复允许）**：`vfs.h` 的 mem 后端加了一把小型自旋锁
（Win32 `InterlockedExchange` / 其他编译器 `__sync_lock_test_and_set`），保护**表与引用计数**（`vfs_mem_open`、
`vfs_mem_unlink`、`vfs_mem_close`），`read`/`write` 的拷贝也在锁内（这个后端的全部意义就是简单、且与 disk 行为
一致）。服务器核心里的 `mem://` + 线程**拒绝已删除** ✓；vfs.h 顶部的线程契约改写为"两个后端对线程的答案现在
一致" ✓。

**验证方式与量级修正**：先前我按"1/1024 桶碰撞 × 微秒窗口"估成约 1e-9/次，**偏低到离谱** ✗。新增确定性用例
`vfs mem backend: two threads, two paths in one hash bucket`（两个路径**现场断言**同哈希桶 ⇒ 用例不会悄悄停止
测它名字里的东西；两线程同步起跑，各自 20000 轮 open→write→read→close→unlink）给出：
- **有锁**：`SUMMARY: 5/5 passed`，40,000 轮迭代 190 ms ✓；
- **把锁改成空操作（同一构建路径）**：`SUMMARY: 4/5 passed  453 us`，失败断言
  `a.opens == a.iterations`，`expected 20000` / `actual 124` —— 两万轮里只有 124 次打开成功，其余全部因
  桶链被并发插入/摘链破坏而失败 ✓。
⇒ 两个线程持续夹击同一个桶时，这个竞争**极易命中**（不是微秒级罕见事件），锁是**必要且充分**的 ✓。

真机复核（`mem://` + 真实线程后端）：`SET k1 hello` ⇒ `ok` ✓、`GET k1` ⇒ `hello` ✓、`STATS` 显示
`wal_records=2`（WAL worker 确实在跑）✓。手写压力工具的线程模式此前已改走 `disk://`：那条改动**保留**（它更贴近
真实路径，且工具离开时会清理自己写出的库），但不再是"因为被拒绝才绕开" ✓。

## S. 客户端与 CLI（第三轮审视 5.x）—— 已修五项，一项转为开放

- **5.1 管道输入被完全忽略、且进程永不退出（已修）**。根因：`cli.h` 的 `cli_poll` 把输入循环写成
  `while(cli->tty && ...)`（`cli.h:731`）——脚本模式（`tty=0`，即管道/重定向）**根本不读**，命令不执行，
  EOF 也看不到。修复前实测：`printf 'SET pk1 v1\nGET pk1\n' | kdbctl 127.0.0.1:9601` ⇒ **rc=124**（被调用方
  kill）、输出只有连接横幅（30 字节）。现在 `kdbctl` 在脚本模式下自己按行读入并走同一个分发入口
  （`cli_exec_line`），EOF 即收工。修复后实测同一条命令 ⇒ rc=0、三条命令都执行、数据可读回
  （`ok`/`ok`/`x`）。`tests/cli_smoke.sh` 新增三项断言（含"管道里第一条命令必须执行"）。
- **5.1b 由此暴露：连接建立前派发的请求会丢（转为开放）**。修 5.1 时发现脚本里**第一条**命令总是超时
  （`request timed out: no response from the server`），因为它在连接/握手完成前就入队并被当作已发送。脚本模式
  现在等 CLI 与服务器真正说上话后再读第一行（与 one-shot 路径同一个信号）⇒ 该现象在 CLI 上不再出现。但
  **客户端 API 层面**仍可构造"连接前入队 ⇒ 无人重发"的窗口，**记为开放**（未伪造修复）。
- **5.2 one-shot 把"可能已提交"说成"未执行"（已修）**。一次性等待是 **3 s**，而客户端自己的请求超时是
  `K_CLIENT_REQUEST_TIMEOUT_MS=5000`（`kclient.h:18`）⇒ CLI 先放弃，然后断言"命令未执行"——这是它无从知道的。
  现在等待预算由常量派生为 `K_CLIENT_REQUEST_TIMEOUT_MS+1s`，消息改为
  `no response from the server within 6000ms; the request may or may not have been applied`；**并且**把"本地就被
  拒绝（根���没发请求）"与"发了没等到响应"分开说——前者现在打印
  `the command was not run: no request was sent (see the message above)`，不再把用户引向网络。
- **5.3 PIPE 把 REDIRECT 记成"该操作的结局"（已修）**。完成回调是**按响应**触发的（`kclient.h:243-245`），而
  客户端收到 REDIRECT 后会**重发**同一请求 ⇒ 同一个操作被计两次：重定向的那次往返进了延迟样本、计数进了吞吐、
  状态落进 `other` ⇒ **全部成功也可能退出码非 0**。现在回调直接跳过 `K_STATUS_REDIRECT`（它不是结局）。
  实测：`PIPE 4 200 SET pk` ⇒ `ok=200 not_found=0 other=0 end=complete`、rc=0（`cli_smoke.sh` 亦断言此三项）。
- **5.4 `avg_us` 的分母是封顶的样本数（已修）**。延迟样本上限 `K_PIPE_SAMPLES=8192`，而 `g_pipe_us_sum` 累加
  **每一条**操作 ⇒ 平均值被放大约 `ops/8192`（n=100000 时约 12 倍）。现在单独统计"参与求和的操作数"并按它求平均，
  报告行同时给出 `ops_timed=`（统计量必须自带采样数）。实测：`avg_us=5574 ops_timed=200`。
- **5.5 `RGET limit abc` 静默变成无界（已修）**。`strtoul` 把 "abc" 解析成 0，而 0 在这里表示**无上限**；
  `"10abc"` 静默变 10、`"-1"` 回绕成 4294967295。现在严格解析：只接受正整数，否则报
  `error: limit must be a positive integer ("limit N")` 并 rc=1（实测修复前：rc=0、零报错、跑了一个无界扫描）。

**验证**：`tests/cli_smoke.sh` 从 12 项扩到 19 项，全部 PASS（新增：管道脚本 3 项、非法 limit 2 项含一条"必须不
把本地拒绝归咎于网络"的反向断言、PIPE 的 ok/other 与 ops_timed 2 项）。这些断言的"能失败"证据就是修复前的同路径
实测（rc=124 零执行 / rc=0 静默无界），已记在上面。

## How to read the line numbers in this file

Every `file:line` here was true on the commit that wrote it, and the tree keeps moving, so a reference that no
longer lands on the symbol is expected drift, not evidence of a mistake.  The **symbol names are authoritative**;
the numbers are a pointer.  The fourth round re-verified the references the auditors flagged (`vfs.h:339-362` /
`:371-377` for the mem-backend bucket update, `cli.h:731` for the tty-gated reader, `code/kserver.h:5395` for
`k_open_fail`) and refuted two more: `tools/archive/` really does hold 16 scripts and `testing.md` really does list
five unit suites, so those numbers were right all along.

## T. mem 后端锁的范围（维护者评估）与 5.1b（客户端层面被推翻）

- **锁的范围收窄为"表 + 引用计数"，且必须包含 `close`**。维护者建议"只锁 open/unlink 即可"。核实后：
  **open/unlink 不够，`close` 必须一起锁** —— `vfs_mem_close` 做 `--n->refcount` 并可能释放 inode，而
  `vfs_mem_unlink` 做 `n->linked=0` 后判 `refcount==0` 释放：不锁 `close` 就存在"关句柄"与"摘链"之间的双重释放
  /释放后使用。`read`/`write`/`sync` **不需要锁**（只碰本句柄自己的 inode 内容，由"一个句柄一个线程"的规则
  保证）——已按此收窄，`vfs.h` 顶部契约同步改写。实测收益：并发用例 190 ms → **93 ms**（临界区少了一半）。
- **不换成 `EnterCriticalSection`/`pthread_mutex_lock`**。理由（三条，均已核实）：(1) `CRITICAL_SECTION` 需要
  `InitializeCriticalSection` 这样一次性的初始化，而这个后端的实例是静态对象、没有任何 init 钩子，懒初始化又
  得回头用原子操作做守卫；(2) `pthread_mutex_t` 会把 pthread 链接依赖塞进这个**无依赖的单头文件**，凡是包含
  `vfs.h` 的目标都被迫带 `-lpthread`；(3) 收窄后的临界区只有几个指针更新，用阻塞锁换不到任何东西——需要阻塞锁
  的是"锁内做大拷贝"，而我们刚刚把大拷贝移出了锁。当前的 `InterlockedExchange`/`__sync_lock_test_and_set`
  自旋锁同时满足 MSVC 6（Windows XP ✓）与 GCC，且零依赖。
- **5.1b 在客户端层面被推翻**。上一轮我把它记为开放（"连接前入队的请求可能被丢"）——错。新增确定性用例
  `client: a request queued before the connection is sent once it comes up`（连接前入队 ⇒ 断连时不发送 ⇒
  `k_client_connect` + `k_client_on_connected` ⇒ 循环把请求发出去 ⇒ `sent==1`）。`kclient_test 18/18` ✓ ⇒
  客户端在连接建立后会补发，`send_pending_now` 机制（"发送必须在循环里做，不能在完成回调里做"）工作正常。
  修复前脚本首条命令超时的**真实原因**是 CLI 在连接可用之前就派发了该行；脚本模式的"等横幅"门已经把它挡住。
- **顺带量了一条基线**：干净服务端上，**第一条**一次性请求约 **1.0 s**（其后约 0.68 s）⇒ 不是 5 s 超时；先前两次
  探针里出现的首条超时在干净服务端上**不可复现**（那两次打的是被反复 kill/重启的服务端），记在此处以免被当成
  缺陷复现步骤。
- **store 级快照 harness（第四轮残余项，已开始收口）**：`tests/kserver_test.c` 的 `drive_store` 分三阶段造出"记录带
  **快照基址**、且 WAL 已越过扫描 ceiling"的存储（每阶段都自证：`snapshot.index>0` 且快照文件校验通过 ✓、unlink 返回 0 才算"前缀确已释放" ✓）；
  首个用例 `server WAL recovery keeps a snapshot base behind a released prefix past the scan ceiling` 用 `treap_get` 断言恢复出的状态 ✓，
  **红证成立**（停掉 A1 的尾部跳转 ⇒ 47/48 ✓）。过程中实测到两条机制：`k_server_open` 会以持久化配置覆盖 open 前设置的 cfg 字段；
  mem 后端 `vfs_open` 对已 unlink 的路径会**新建**文件（存在性探测会复活被测前缀）。**A5 / A7 均以覆盖**（A5 用伪造载荷 ✓、A7 无需伪造：快照记录带真实基址、其后普通记录带 0 ✓ 即是"强制抬高"现场 ✓；两者红证成立 ✓）。
  **仍未覆盖**：仅剩 C3（需"变更在飞行中"的集群 harness ✓）。
- **XP 客人机验证（C3-C8 + A4/A7 批，2026-09-22）**：`[kynovo_step.bat] size=4672 ver=H`、`[xp_tests.bat]
  size=5996 ver=G`、`build_exit=0 run_exit=0 tests_exit=0`、`error C`=0、`LNK`=0、17 条 `C4761`（与历次同数、同类别）；
  `SUMMARY: 5/5 · 19/19 · 221/221 · 47/47 · 5/5`。六条新用例逐字 PASS（41–46 号），含 A4 的
  `server WAL recovery judges a segment that continues a torn tail by generation  3425 us`。**新鲜度由产物证明**：
  取件日志含 `Transfer successful: 278831 bytes`（`code/kserver.h`）与 `102251`（`tests/kserver_test.c`）—— 正是这两批改过的两个文件；
  `doc/testing.md` 第七节已收口。
- **两次作废的客人机运行（同一批，取件路径错位 —— 我的操作错误）**：`.bat` 取的是带前缀的路径
  （`GET code/kserver.h`），服务端因此解析到 `staging/code/`、`staging/tests/`；我前两次把仓库源码复制到了暂存**根目录**，
  那两份从未被更新 ⇒ 客人机编译的是**上一次刷新**的源码。证据：两次都报 `41/41`，且取件字节数为 `270417`（`code/kserver.h`）与
  `84890`（`tests/kserver_test.c`）= 批改之前的树，与我"三向一致"的自检**都不矛盾**（我核对的是根目录文件，不是被取件的那份）。
  脚本自身的 `ver=`/`size=`/`GET failures: 0`/退出码/`error C`=0 全程正常 ⇒ 唯一能识别空跑的证据是**字节数与用例数**。
  修正：刷新 `staging/code/`、`staging/tests/`，并用**与客人机相同的带前缀路径**取回自证（`GET code/kserver.h` ⇒ 278831，md5 与仓库一致）；
  同时删除根目录的扁平副本，避免下次自检再被它骗过。
- **XP 客人机验证（已跑完，第四轮收口复跑）**：暂存刷新后由客人机执行 `go.bat`（`kynovo_step.bat` size=4672
  ver=**H**、`xp_tests.bat` size=5996 ver=**G**，脚本自报尺寸+版本、且与暂存区一致 ⇒ 可证明跑的是新脚本），
  `build_exit=0 / run_exit=0 / tests_exit=0`，两份日志里 `error C` 与 `LNK` 均为 **0**；MSVC 6 的 17 条 `C4761`
  在同样这些行上一直都有（`git blame` 显示本轮未改动那些行，`doc/testing.md` 也把该类列为已知），不是回归。
  五套：`cemon 5/5`、**`kclient 19/19`**、`raft 221/221`、**`kserver 41/41`**、`vfs_fault 5/5`，与本机门同数；
  本轮新增的五条用例逐字通过：
  `PASS  proto: a handler that frees the rx buffer cannot be parsed through in the same feed  38 us`（去掉守卫时该用例会段错误）、
  `PASS  server InstallSnapshot replaces a stale longer file instead of keeping its tail  999 us`、
  `PASS  server WAL recovery refuses a cut in the middle of the log (fail-stop)  5057 us`、
  `PASS  server WAL recovery refuses to start empty behind a released prefix past the scan ceiling  32880 us`、
  `PASS  membership: the note is bound to the request that submitted the change, not to the server  961 us`；
  真线程用例仍在（`PASS vfs mem backend: two threads, two paths in one hash bucket  202747 us`，主机同项为 93 ms，
  同量级）。fuzz：`done: 200 iterations`、`done: 2 iterations`、`1/1 clusters consistent`、
  `linearizability: 4 histories / 253 ops decided by the checker, 0 inconclusive`；崩溃契约 `survive-me` 复现。
  **客人机不覆盖的**：`tests/cli_smoke.sh` 是 shell 脚本、XP 无 shell ⇒ D 系列的退出码语义（死主机脚本必须失败、
  服务端拒绝的命令必须让脚本失败、CAS 冲突退出码 2、被截断的 PIPE 不是通过）只有本机门与 CI 覆盖；客人机的
  `piped.bat` 只走管道 CLI 的正常路径（`piped_rc=0`、`ok`、`v1`）。

## U. Round-4 review: findings and their terminal state

The fourth full review (7 read-only audits, `doc/code-review-2026-09d.md`) runs with one extra instruction the
third round earned: **do not trust the ledger, the review documents or code comments - verify them against the
code**, because the third round ended with three of its own write-ups refuted.  This section records each finding
of the fourth round with its terminal state.

| # | finding | state |
|---|---------|-------|
| 1 | The mem backend's lock was acquired as `((vfs_mem_ctx *)file->be)->lock`.  `file->be` is the mem backend only when the file came straight from `vfs_open`; the fault-injection seam in `tests/vfs_fault_test.c` wraps mem and re-points each file's `be` at itself, so the cast locked and wrote memory belonging to the wrapper.  MinGW's layout made that harmless (the local full gate stayed `14/14` through the same code), the Linux runner hung inside the test's first case. | **FIXED** (`62790f5`): a file-scope `static vfs_spin vfs_mem_lock`, so the lock cannot depend on how a caller reached the backend; the ctx no longer carries a lock field.  Found by investigating two pushes that had a red linux-gate job (`cd4252e`, `1887455`) - not by the audits.  The `regress-logs-linux` artifact named the layer: every other log green, `vfs_fault_test.log` stopping after `BEGIN [1/5]`.  The same job is `success` on the fix. |

### The seven audits' findings (consolidated, with my own verification state)

`doc/code-review-2026-09d.md` is the consolidated document: 7 read-only audits, every finding tagged `[me]`
(I re-opened the code) or `[audit]` (the auditor's trace, not yet re-checked), plus the one already fixed (`62790f5`).

| area | findings | state |
|------|----------|-------|
| Recovery | **A1 CRITICAL - FIXED (`kserver.h` scan + `test_wal_recovery_refuses_a_prefix_past_the_scan_ceiling`)**; **A2/A3 REFUTED (reachable forms) - the misleading slot comment is rewritten and the property pinned by `test_wal_recovery_refuses_a_middle_segment_cut`**; **A5/A6 FIXED (`prev_base>0` guard; the install probes past its end and discards, after the fuzz refuted the delete-at-offset-0 version 5/5)**; **B4 FIXED with a red case (the suite segfaults at that case when the guard is neutered)**; **A4 FIXED** (a segment continuing a torn tail must still be newer, exactly `+1` after a payload tear and `>` after a header tear, and term/vote now merge by term so a term cannot regress - red case: the stale continuation is accepted when the rule is reverted); **A7 FIXED** (the forced-high base decodes the record that first carried it; the verify helper no longer creates the file it verifies - both unred-proofed, see the note below; `skipped_prefix` was already gone); and one residual that no test can reach yet - a store with a snapshot base needs a snapshot-driving harness before A1's tail-scan branch, A5's guard, the recovery side of A6 and A7's forced-high-base path can be covered | **open (residuum only)** |
| Threads/lifetime | **B1 FIXED** (`wait_exit` reports whether every worker is gone; release closes only then); **B2 FIXED** (`thread_destroy` leaks instead of freeing when a thread outlived the join); B3 the event loop writes an inbound snapshot unlocked while the worker may write the same path `[audit]`; B4 rx buffer freed inline while the reader is inside it `[me]`; B5 a failed snapshot-result post wedges the stop path (OOM) `[audit]`; B6 mem open/unlink still read the table through `be` `[me]` | **open** |
| Protocol/membership | **C1 FIXED** (the note now lives on the `k_request` that submitted the change, so it cannot be misdelivered, overwritten or left behind - the earlier claim that this had already been done was false); **C2 FIXED** (a note that would not fit ships empty and says so, instead of a cut sentence); **C3 FIXED (defensive - see the note below the table: no red proof, stated as such)**; **C4 FIXED** (`pending_since_ms` per entry from the server's internal clock, printed per entry); **C5 FIXED** (the counter is named `membership_targets_graduated` for what it counts, and the `STATS` label it was printed under was misspelled `..._gompleted` - fixed); **C6 FIXED** (TOPOLOGY is answered locally with INFO/STATS, sharing the single `raft_inspect` site, and no longer enters the barrier it is exempt from); **C7 REFUTED** (freeing a failed FCALL re-opens the gate and leadership loss drains the queue by design); **C8 FIXED** (both halves: SHUTDOWN stops regardless of its ack; a refused change's leftover pending entry is marked `source=-1` and reported instead of hidden) | **closed except A4/A7** |
| Client/CLI | **D1 FIXED** (the opening wait is bounded and a failed command fails the script); **D2 FIXED** (a final redirect fails, a CAS conflict exits 2, OK/NOT_FOUND stay 0); **D3 FIXED** (the table is per mille now); **D4 FIXED** (a truncated or short run exits non-zero; the rate counts only redirect-excluded completions); D5 deadlines armed at queue time `[audit]`; D6 give-up paths drop requests and can leave `discovering` set `[audit]`; D7 6 s budget on local refusals, argv re-join splits args, connect never retried `[audit]`; D8 EXIT reports failure, no redirect bound, limit guard dead on 32-bit `[audit]` | **open** |
| Tests/gates | **E1 FIXED** (every layer runs under a watchdog; a hang is FAIL with rc=124); **E3 FIXED** (one emptiness report sits after every `git_files` call and prints the file count); **E2 FIXED** (the tautological assert now measures the bounded return - `4/5` with the bound set to `0ul`, `5/5` at 10 s against a measured 2.0 s drain; a true hang remains the watchdog’s job and the comment says so) `[audit]`; E3 the new git-list rule reports before the tests list is built `[me]`; **E4 FIXED** (`is_char_literal` now consumes hex/octal/simple escapes; the `//` rule catches `case 3://note` and a trailing `//` while still excusing `scheme://` - narrowing the `:` guard instead of dropping it, which the first attempt proved necessary by going red on `mem://` prose) `[audit]`; **E5 FIXED** (`watch_counters.sh` now greps `wal_inflight`/`pending_requests`/`pending_request_bytes`/`client_connections`, certifies it measured something and FAILs on growth; 11 report-only harnesses are labelled as reports; `regress_selftest.sh` extracts the real `reg_report`/`reg_gate` from `build.sh` and runs as gate layer `regress_selftest` (11/14/15) - where it caught `reg_gate` reporting `[layer timeout 1800s]` for a layer that merely exited 0 with no verdict; `bench_persist` prints its error count; the `raft_fuzz done:` half of the finding is **REFUTED** - that loop runs exactly `count` times) `[audit]`; E6 `unique` workload flag unreachable (`atoi("unique")`) `[audit]`; **E7 FIXED** (the `sprintf` ratchet now matches column 0; the `raft_inspect` rule prints its sanctioned site; `git_out` records non-zero exits => rule 20, `PRINCIPLES|OK|rules=20 fail=0` (its first version flagged every git call and both CI jobs went red: `git diff HEAD~1 HEAD` legitimately exits 128 in CI's shallow clone - the rule now records only `git ls-files` lists, reproduced in a `--depth 1` clone); `cli_smoke`'s piped needles are the stored values instead of `"a"`) `[audit]` | **open** |
| Docs | F1 the ledger's own §N claim about the ceiling is false (corrected above) `[me]`; F2 a list of wrong/stale references and counts `[audit]` | **F1 corrected; F2 open** |

Fix order and the evidence each fix owes: section H of `doc/code-review-2026-09d.md`.

### The A4/A7 batch: the round's last HIGH item, and the evidence it owes

**A4 is a Raft safety fix, not a cleanliness one.**  After a torn tail the scan required no generation relation at
all, so a stale, foreign or mis-ordered segment was accepted and treated as the newest state; and term/vote were
assigned unconditionally from whichever record the walk reached last, so such a segment could hand back an **older
term with that older term's vote** - a node that comes back believing an older term can grant a vote it already
granted elsewhere or unseat a legitimate leader (Ongaro Sec. 5.1: `currentTerm` "must never decrease").  The rule is
now split by where the segment was torn, because that is what decides how much can be known: a payload tear leaves
the torn record's header intact, so the continuation is exactly `+1`; a header tear leaves even that record's own
generation unknowable, so only `>` can be required.  `>` alone is not enough after a payload tear - it would still
admit a segment from another history - and requiring nothing (what the code did) admits everything.

**Evidence, and one trap worth recording.**  `test_wal_recovery_continues_a_torn_tail_by_generation` covers all four
cases.  Its first version was **vacuous**: it wrote the continuation into segment 1 while the scan stops at the
write position (`meta->next.segment`), so the walk never reached it and all four cases "passed" - including the
two that must refuse.  The suite was green and measuring nothing.  The case now sets a tiny `wal_seg_size` so the
store really rotates, reads the segment numbers instead of assuming them, and **asserts the rotation happened**
before using it.  Red case for the continuity rule: with it reverted to the old behaviour the case fails on
`header tear + 0`.  The term merge has no red case of its own - with the continuity rule in place a stale segment
no longer reaches the decode, so it is defence in depth (labelled as such in the review document).

**The store-level snapshot harness now exists** (`drive_store` in `tests/kserver_test.c`), and its first case
covers **A1 with a snapshot base**: a store that really took a snapshot (certified: `snapshot.index > 0` and the
file verifies) whose released prefix passes the scan ceiling must recover the snapshot's state, asserted through
`treap_get` rather than `treap_inspect` (a new checker rule, 21, forbids branching on the latter).  **Red proof:**
neutralising the tail jump so the walk falls through to the pre-fix `break` fails the case (47/48, on `rc==0` /
`it recovers`).  Two measured facts went into the harness: opening a store loads its persisted configuration over
whatever the caller set before `k_server_open`, and on the mem backend `vfs_open` creates an unlinked path, so an
existence probe resurrects the prefix it is checking (the unlink return value is what certifies a release).

**What still has no case.**  A7's forced-high rollback is now covered - and it needed no forging: a store whose
newest record carries base 0 while an earlier one carried the real base is the observed restart shape, so the
harness builds it directly.  Red proof: making the decode use the newest record again fails both this case and
A1's (48/50), since they share that path.  A5's `prev_base>0` guard is **now covered**: `forge_last_record_base`
rewrites the last complete record so it names a base with no snapshot file (CRC recomputed and stored back), and
the case requires the load to refuse rather than roll back to base 0 - red proof: without the guard the store
opens (48/49 at `rc!=0`), i.e. the silent empty start the guard exists to stop.  The verify helper's failure mode is only observable on a filesystem - and without a vfs existence probe,
"absent" and "present but empty" are indistinguishable from inside the process, so every assertion the suite could
make passes either way.  A first version of that case was written, found not to discriminate (it passed with the fix
reverted) and **removed**: a test that cannot fail is worse than no test, because it is read as coverage.

### The C3-C8 batch: what the evidence is, and where it is missing

`C3-C8` were fixed in one commit, each with a case in `tests/kserver_test.c` (`TEST_PLAN` 41 -> 46).  Three of
them were red-proofed by reverting the fix in a scratch copy of `code/kserver.h`, rebuilding, and reading the
suite's own verdict:

- **C4** - with the per-entry age replaced by the server-wide accumulator, the new case fails (it asks for 15 s and
  5 s on two targets added 10 s apart).
- **C6** - with TOPOLOGY put back on the read barrier, the new case fails: the answer is a `REDIRECT` naming the
  node that received it.
- **C8** - with the refused-id mark neutered to `0`, the new case fails on `pending_source[0]==-1` (expected -1,
  got 0).  The `SHUTDOWN` half of C8 is red-proofed by the same revert: with `if(send(...)!=0) return -1` restored it
  reports no shutdown.

**C3 is the exception, and it must not be read as covered.**  The fix guards the clock clear with
`k_server_membership_own_pending(server)==0`.  The unit harness cannot stage the refusal the audit describes: the
only refusal shape reachable there is the *address-log* submit failing before the reconfig is attempted (a stale
leadership view), where nothing touches the clock - so removing the guard leaves the new case green.  Staging the
reconfig refusal needs a real in-flight change, and the harness cannot keep that change's catch-up target from
graduating into the desired config, which legitimately ends the wait.  The guard is therefore a defensive fix with
the audit's reasoning accepted on the code, and the case pins only what it can: a refusal answers an error and
leaves a running wait's clock and notice budget alone.  Covering it properly needs a harness that can hold a config
change in flight across an advance.  The store-level snapshot harness added for A1 does not provide that: it drives
recovery from a written store, not a live change, so C3's guard still has no case.

One further defect was found **while** fixing C5 and is recorded here because no audit found it: the `STATS` format
string printed `membership_targets_gompleted`.  Any external scraper reading that field by name would have missed
it, and the misspelling survived the earlier rounds because nothing parses that line - the counters harness greps
other fields.  Fixed with the C5 rename; `doc/`, `tools/` and `tests/` carry no other reference to either name.
