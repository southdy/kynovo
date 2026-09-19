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
its crash contract is measured.  Evidence, all from the CentOS 7.9 guest: `REGRESS|quick|pass=8 fail=0` with
the same eight layers Windows runs; every file in `code/` md5-verified against the committed tree before
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
