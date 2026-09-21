# kynovo full-project review — 2026-09d (fourth round)

## How this round was run

Seven read-only module audits in parallel: the previous round's own fixes, thread-safety and lifetime, crash
recovery, the request/protocol path, the client and CLI, the tests and gates, and the documentation's factual
claims.  Each auditor was told: no builds, no edits; every claim needs `path:line` plus a verbatim quote; label
each claim VERIFIED (traced in the code) or ASSUMED; and **do not trust the ledger, the review documents or code
comments - verify them against the code**.  That instruction was earned by the third round, which ended with three
of its own write-ups refuted.

Every finding below carries the state of **my own** verification, because an auditor's trace is still a self-report:

- **[me]** - I opened the code myself and confirm the finding as written; the quote is what I read.
- **[audit]** - traced by the auditor, quote reproduced, not yet re-opened by me.  Not settled.
- **[fixed]** - closed before the audits returned.

Baseline evidence, taken before any conclusion: local `REGRESS|full|pass=14 fail=0 duration=346s`,
`PRINCIPLES|OK|rules=19 fail=0`; XP guest (MSVC 6.0/SP3) `cemon 5/5 · kclient 18/18 · raft 221/221 · kserver
37/37 · vfs_fault 5/5`, fuzz `done: 200 iterations` / `2 iterations` / `1/1 clusters consistent`,
`linearizability: 4 histories / 253 ops decided, 0 inconclusive`, crash contract `survive-me`.

## A. Recovery - the worst cluster

### A1 `[me]` **[FIXED]** CRITICAL: the 64-segment ceiling broke the scan *before* the acknowledged-record check, so a store with a released prefix of 64+ segments restarted **silently empty, metadata present**

`code/kserver.h:1635` `if(!records&&seg>=(meta_absent?K_WAL_SCAN_EMPTY_PREFIX_MAX_UNUSED_SLOT:K_WAL_SCAN_EMPTY_PREFIX_MAX)){`
→ `:1648` `break;`; then `:1775-1783` `if(!records){ … return (meta_rc==0)?0:1; }` - which returns **before** the
ack check at `:1789` `if(!meta_absent&&meta->record_size>0u){`.

The ceiling applies whenever `records==0`, metadata present or not.  A released prefix is the store's age in
segments (`:4677` keeps segments above the previous snapshot base, and segment numbers never reset), so any store
whose lifetime WAL passed segment 64 - 4 GiB at the default 64 MiB segments, far less with a small configured
`wal_seg_size` - restarts as an empty state machine: no print, empty treap, and the metadata is not
re-initialised, so every later restart repeats it and writes made afterwards become invisible too.

My own comment on that path carries the wrong reasoning, which is why the third round missed it:
`code/kserver.h:1780-1781` `The segment holding the NEWEST record is never released by snapshot cleanup … so
"nothing anywhere" cannot mean "the history was released".`  True and irrelevant - the break stops the scan before
it can reach that segment.  The ledger repeats the mistake (`doc/gaps-audit.md:1128`).

**Fix (this round).** The walk no longer gives up at the ceiling when the metadata is present: it knows the store
has acknowledged records, so it jumps to `last_seg - K_WAL_SCAN_TAIL_SEGMENTS` and looks there (the newest records
are always near the write position), and a second ceiling test is suppressed after that jump (`!jumped_to_tail`) -
without it the walk re-trips the ceiling a few segments later and dies before reaching the records, which is exactly
what the first attempt did. If the walk still finds nothing while the metadata names an acknowledged record, recovery
now refuses loudly (`refusing to recover an empty store`) instead of returning success, and the `if(!records)`
comment that carried the wrong reasoning is rewritten. `K_WAL_SCAN_EMPTY_PREFIX_MAX`, `..._UNUSED_SLOT` and the new
`K_WAL_SCAN_TAIL_SEGMENTS` are `#ifndef`-guarded like the file's other tunables; the dead `skipped_prefix` (A7)
is gone.

Evidence: `tests/kserver_test.c` gains `test_wal_recovery_refuses_a_prefix_past_the_scan_ceiling` - it drives a
`mem://` store past segment 64 with a 900-byte segment, releases the prefix by hand exactly as cleanup does, and
asserts the store does **not** come back empty. With the fix neutered back to the old behaviour the case fails
(`SUMMARY: 37/38`, `-- FAIL server WAL recovery refuses to start empty behind a released prefix past the scan
ceiling`); with the fix it passes and the store refuses with `wal: replayed log has a hole at index 400 (expected
383): refusing to recover` - loud, named, and not an empty state machine. Gates: `REGRESS|quick|pass=10 fail=0`,
`REGRESS|fuzz|pass=13 fail=0`.

Residual, stated rather than implied: the test proves the *no-silent-empty* property. The other branch - the tail
walk rebuilding a store whose retained records still carry the state, i.e. one with a snapshot base inside the tail -
is what the jump is for and is **not** covered by a test yet; it would need the test to drive a snapshot (this suite
does not). The next store-level test that drives snapshots should cover it.

### A2 `[me]` **[REFUTED in its reachable form; the comment was the defect]** HIGH: "the metadata stores the last ACKNOWLEDGED record" is a misleading description - the slot is a watermark

`code/kserver.h:1786` claims it, while the write path syncs the slot only on a segment change or every
`K_WAL_META_FSYNC_EVERY` records: `:2082` `durable=(record.segment!=worker->meta_sync_segment)||(worker->meta_since_sync>=(int)K_WAL_META_FSYNC_EVERY);`
A client is acked when its record write+fsync returns.  So the check catches a *deleted older segment* but not a
hole among the newest ≤63 records: it silently truncates acked entries there and rewinds the append position.


**Verdict.** The description is wrong and was corrected in place (the slot is fsynced on every segment change *and*
every `K_WAL_META_FSYNC_EVERY` records, so it lags; the comment now says watermark and what the check really catches).
The *consequence* the audit drew - "a hole among the newest records slips past, recovery silently truncates acked
entries and rewinds the append position" - does not survive tracing or experiment: a hole *inside* a segment is
caught by the CRC and generation-continuity checks, and a hole *between* segments is caught by this very check,
because the segment-change sync keeps the slot's record in the newest segment.  The residual is a byte-level hole
inside the newest segment *after* the slot's record that still passes CRC - that is storage corruption, not a log
structure the code can be blamed for.  Guard added: `test_wal_recovery_refuses_a_middle_segment_cut` (see A3).

### A3 `[me]` **[REFUTED - and now guarded]** HIGH: a hole or missing segment *after* records were read is a silent chain end, not fail-stop

`code/kserver.h:1651-1652` `if(records) break;` (open failure) and `:1667` the same for an empty segment.
`doc/crash-contract.md:54-55` claims a hole inside the retained range is fail-stop.  It is not; the only guard is
A2's lagging check.


**Verdict.** The code does break the chain silently at that point (`if(records) break;`), so the auditors read the
code correctly - but the *outcome* is not silent: the acknowledged-record check above catches it.  Experiment (an
one-off diagnostic case, since promoted): an 8-segment store, one MIDDLE segment unlinked (records intact on both
sides) - the walk stops at segment 3 and the store refuses with `wal: the metadata says the record at segment 8
offset 122 was acknowledged, but the log ends at segment 3 offset 854 (records are missing): refusing to recover`,
`k_server_open` returning -1.  The reason is structural: the slot is synced at every segment change, so its record
is always in the newest segment and any cut before it is detected.  `doc/crash-contract.md`'s claim that a hole in
the retained range is fail-stop therefore holds for the reachable forms, and it is now pinned by
`test_wal_recovery_refuses_a_middle_segment_cut` (39th case).  Deliberately labelled: that case is green before and
after any fix - it is a regression guard for a property, not evidence for a change.

### A4 `[audit]` HIGH: after a torn tail, cross-segment generation continuity is not enforced, and term/vote can regress

`:1687` gates `:1712` `if(!have&&prev_seg_clean&&prev_seg_gen&&gen!=prev_seg_gen+1u){` on `prev_seg_clean`, so a
segment following a torn tail is accepted with no relation to the previous generation; `:1757` adopts term/vote from
the *last record read* (segment order), not the highest generation.

### A5 `[me]` **[FIXED]** MEDIUM: base 0 was accepted as a "verified snapshot"

`:1545` `if(index<=0) return 1;   /* a zero base needs no snapshot file */` makes `:1806`
`if(n_base>0&&k_snapshot_verify_file(base,prev_base)==1){` accept `prev_base==0`, so recovery can "roll back to
base 0" - printing it as if genuine - and start from an empty tree with a full-log replay.


**Fix.** The rollback branch now requires `prev_base>0` as well (`k_snapshot_verify_file` answers 1 for any
`index<=0` by construction, so without it an INVENTED base 0 passed as verified and the log was replayed from an
empty tree, silently whenever the retained entries happened to start at index 1).  With the guard, an unusable
snapshot and no earlier base falls into the existing refuse branch - the honest answer.  No test covers this yet:
constructing the case needs a store with a snapshot base, and this suite does not drive snapshots (the same gap
recorded under A1's residual).

### A6 `[me]` **[FIXED]** MEDIUM: InstallSnapshot wrote its target in place, with no truncate and no end probe

`:2430-2435` `file=vfs_open(path);` … `vfs_write(file,snapshot->snapshot_offset,…)` …
`(snapshot->snapshot_done&&vfs_sync(file)!=0)`.  The save path guards against a stale longer incarnation
(`:4581` `bytes found past the trailer (stale longer incarnation)`), the install path does not, so `.snap.<index>`
can be a mix of two writings; the app then *loads* it (`:1291` `treap_load` mutates the live tree before the CRC
verdict at `:1297`) and turns a torn install into `fatal: snapshot load failed` (node exit) rather than a failed
install.


**Fix, and how the first version of it was refuted by the fuzz.** A stale, longer incarnation of the same index must
not survive past the new end.  The first attempt deleted the target when the install's first chunk arrived, plus an
end probe at `done` - and `kserver_cluster_fuzz` seed 1 diverged **5/5** with it: the fuzz injects duplicated and
dropped frames, and a duplicated first chunk truncated a file whose later chunks were already in place, leaving a
torn snapshot that the node then could not load.  Same-build A/B, both directions: reverting only A5 still failed
5/5 (A5 innocent), reverting A5+A6 was green 3/3 (A6 the cause).  The guard therefore lives **at the end only**: the
install probes past the last byte once the leader says the file is complete, and if anything is there it discards
the file and returns an error.  Measured after that change: `kserver_test` 40/40, `kserver_cluster_fuzz` seed 1 green
5/5, `REGRESS|quick|pass=10 fail=0`, `REGRESS|fuzz|pass=13 fail=0`.  The unit case
`test_install_snapshot_discards_a_stale_longer_file` asserts the honest behaviour (refused and the old bytes gone)
rather than a silent trim; with the guard neutered it fails.

### A7 `[audit]` MEDIUM/LOW: the forced-high base decodes the wrong record (that fallback can never work); the verify helper creates the file it verifies; `skipped_prefix` is dead state

`:1802` `if(n_base<prev_base) n_base=prev_base;` but the decode at `:1822` uses `pick_base`'s segment/offset - the
*newest* record, carrying the lower base - so `:1847` refuses: fail-stop where the comment promises a successful
recovery.  `:1547` `file=vfs_open(path);` in the verify helper creates a missing `.snap.<index>`.  `:1592`
`k_u64 skipped_prefix;` is incremented and never read.

## B. Threading and lifetime

### B1 `[me]` **[FIXED]** HIGH: `k_wal_files_close` is idempotent only sequentially, and the main thread calls it after a bounded wait

`code/kserver.h:1959-1961` `if(worker->seg_file){ vfs_close(worker->seg_file); worker->seg_file=0;`
`code/kserver.h:5257-5260` `/* … k_wal_files_close is idempotent, so this is safe for both. */` then
`k_wal_files_close(&server->wal_worker);` - executed after `runtime_wait_workers_exit`, which only *warns* when a
worker did not exit within `RUNTIME_WAIT_MS` (30 s).  Two concurrent callers both see a non-NULL handle: double
`vfs_close` → double `VFS_FREE` and a double `--n->refcount`.  "Idempotent" is the unproven premise.

**Fix.** `wait_exit` now reports whether every worker is gone (the vtable, both backends and `runtime_wait_workers_exit`
return it; the sync backend answers 1 by construction), and release closes the WAL handles only when it did.  A worker
that outlived the 30 s wait leaves the handles alone and says so - a leaked handle while the node is going down beats
a double close.  Defensive fix, honestly labelled: the suite has no way to make a worker outlive that wait, so there
is no red case for it; the evidence is the code path plus the gates.

### B2 `[audit]` **[FIXED]** HIGH: `thread_destroy` frees the runtime even when a thread did not join

`code/runtime.h:417` warn-and-break, then `:426-429` queue free and `:437` `RUNTIME_FREE(t)`, while the straggler
keeps using `rt`.  Same 30 s-timeout class as B1.

**Fix.** `thread_destroy` tracks the joins; on a join timeout it no longer closes that handle nor breaks out of the
loop, and if any worker is still alive it returns before freeing the runtime, its queues and its thread array - the
only safe option while that thread can still touch them.  `wait_exit`'s new return value (above) carries the same
information to the caller.  Defensive fix, same labelling as B1: the join bound exists on the Windows path (POSIX
joins are unbounded), and no suite can drive a straggler.

### B3 `[audit]` HIGH: the event loop writes an inbound snapshot while the snapshot worker may write the same path

`:2505` `k_server_write_inbound_snapshot(server,&decoded.message.install_snapshot)!=0` runs in the event loop and
writes `.snap.<index>` unlocked (`:2430-2432`); the worker writes the same *kind* of path at `:4542`.  The read path
is guarded (`:4770` `!server->snapshot_inflight`), the write path is not.  A collision needs the two indices to
coincide (deterministic policies on two nodes make it plausible - **[audit] ASSUMED**).  The mem backend's premise
is per-*handle*, so two handles to one inode from two threads is outside it.

### B4 `[me]` **[FIXED]** HIGH (latent): the rx buffer is freed inline while the frame reader is inside it

`code/kserver.h:2383-2386` `if(!conn->close_pending){ conn->close_pending=1; k_rx_free(&conn->rx);` with the
comment `/* the reader is done with this buffer before any reap */` - false at that moment: cemon calls
`k_conn_closed` **inline** from close, i.e. from inside a frame handler that `k_rx_feed` is in the middle of
(`kproto.h:212-215`: the handler returns, then `rx->len>total ? memmove : …; rx->len-=total;`).  `k_rx_free`
memsets the struct, so `rx->len` underflows and the next turn reads `rx->data==0`.  Safe today only because every
in-handler close happens to propagate a nonzero return; nothing states or checks that.

**Fix, with a red case.** `k_rx_feed` now checks, after every handler call, that the buffer is still the one it was
parsing (`rx->data!=0 && rx->len>=total`) and stops otherwise, instead of subtracting from a length a handler may
just have zeroed.  `test_rx_feed_stops_when_the_handler_frees_the_buffer` (kclient_test, now 19 cases) drives it
directly with a handler that frees the buffer and returns 0 - the shape that used to depend on no handler ever doing
this.  Measured: with the guard replaced by `/* NEUTERED */` the suite **segfaults (rc=139)** inside that very case,
with the guard restored it passes and the build is warning-free.

### B5 `[audit]` MEDIUM (OOM only): a failed snapshot-result post wedges the stop path

`:4619-4622` keeps the task on the main thread's pointer, but `:4630` gates `on_stop` on `server->snapshot_task`
being null, so the stop callback never fires and no snapshot is retried - where the WAL side makes the same event
fail-stop (`:4479-4481`).  `runtime_stop` is reached only from release.

### B6 `[me]` LOW: the mem backend's `open`/`unlink` still read the table through a caller-supplied `be`

`vfs.h:340` `vfs_mem_ctx *mem=(vfs_mem_ctx *)be;` (and `:372`) - the same class as the bug fixed in `62790f5`,
benign today because the wrapper forwards `&vfs_mem.base`.  The lock no longer has this problem.

## C. Request/protocol and membership visibility

### C1 `[me]` **[FIXED]** HIGH: the membership note is still server-global, so it is misdelivered or silently killed

`code/kserver.h:575` `char membership_note[128];` · set at `:3424-3434` · consumed at `:4176`
`if(request->type==K_REQ_MEMBER&&request->conn&&server->membership_note_len>0){` - by *whichever* MEMBER commits
next.  Three shapes: an auto-replace accept in the same round clears it (`:3424` runs for `conn==0` too) before the
client's response is delivered; a note set for one request is answered on another client's commit; and the
`!request->conn` / `CATCHUP_FAILED` terminal paths (`:4108-4137`, `:4149-4157`) never clear it.  The third-round
write-up says this was fixed by putting the note on the request - it was not, and that text must be corrected.

**Fix.** The note now lives on the `k_request` that submitted the change (`char member_note[128]` +
`member_note_len`), is rendered there at submit time, and is attached to that request's own reply; the server-wide
`membership_note`/`membership_note_len` fields are gone, so there is no path by which one submitter's text can
reach another client, no overwrite race between two submitters, and no leftover text waiting to be printed with a
later reply.  **The previous round's document claimed this had already been moved onto the request; that claim was
false** - the field was still on the server, set in `k_server_submit_member` and read in the result handler.
`test_membership_note_is_bound_to_its_request` drives two submitters and asserts each note sits on its own request;
it is a guard for the new contract rather than a red case for the old bug (the old shape has no per-request field
to assert on), and it is labelled that way.

### C2 `[me]` **[FIXED]** MEDIUM: the note's worst case does not fit its buffer (three auditors, same arithmetic)

Fixed text 96 chars + node id + ms + label, with `:3340` `return source==1?"submitted by auto-replace":(source==2?"client request":"submitted elsewhere");`
fed by `:3429` from `server->pending_source[0]`.  With `"submitted by auto-replace"` (25) and a 2-digit id the
rendering is 128 > the 127 `k_snprintf(dst,128,…)` can write, so the sentence the feature exists to deliver is cut
mid-word.  The comment at `:3426-3428` ("Measured worst case … = 118 bytes") budgets 14 for the label.

**Fix.** The render now checks its own result and, when the text would not fit, ships an EMPTY note and prints why
(`the note for the change submitted here no longer fits its 128-byte buffer (N bytes) - widen it`) instead of
handing the operator half a sentence.  The comment carries the arithmetic over the reachable inputs.  The test
asserts the note fits and that the stored length excludes the terminator; it exercises the longest label
("submitted by auto-replace", 26 bytes) but with a short node id and a 4-digit age, so the absolute worst case
(11-digit id + 20-digit age) is bounded by the arithmetic in the comment rather than measured.

### C3 `[audit]` MEDIUM-HIGH: a refused change resets the *whole* wait clock

`:3398-3404` `pending_before=server->pending_count;` … `server->membership_pending_ms=0;`
`server->membership_pending_notice_ms=0; server->membership_notice_count=0;` - unconditional, while a second own
pending entry may be alive (layering is explicitly supported).  After 20 s waiting on change A, a refused change B
resets A's reported age to 0 and re-arms the notice budget.  The count rollback itself is correct.

### C4 `[audit]` MEDIUM: `age_ms` is one server-global accumulator reported per entry, and `pending[]` is a bag

`:3065-3067` prints `server->membership_pending_ms` for every entry; the array is swapped-with-last on removal
(`:1330-1332`), so a young entry can be reported with an old entry's age.  The unit test pins the accumulator
semantics, so this is report-vs-behaviour, not a test gap.

### C5 `[audit]` MEDIUM: `membership_change_started`/`_completed` do not count the same thing

`:1329` increments `completed` inside the per-entry loop (one change adding N voters counts N) while `:3420`
increments `started` once per accepted change; a follower applying a change increments `completed` with
`started==0`.  `STATS` presents them as a pair.

### C6 `[audit]` MEDIUM-LOW: TOPOLOGY is exempt from the admission gate but served through a barrier ⇒ a redirect ring

`:3595` exempts TOPOLOGY like INFO/STATS/HELP, but only those three are answered locally (`:3745`, `:3756-3774`);
TOPOLOGY goes through the barrier (`:3216-3239`) and, refused with `leader_id` still naming this node, answers
REDIRECT to the client's own endpoint, which the client follows with no self-check and no retry bound
(`kclient.h:247-264`).  MEMBERS, on the identical path, gets a hard "server stopping" - inconsistent.

### C7 `[audit]` MEDIUM-LOW: a write queued behind a failed FCALL is stranded with no answer

`:3148-3153` frees the failed FCALL and `return`s, after `:3143` already cleared `gate_closed`, so the rest of
`gate_head` is neither answered nor redirected until another FCALL, leadership loss or release.

### C8 `[audit]` LOW: a refused change leaves a phantom pending entry labelled "not ours"; `SHUTDOWN` depends on its ack

`:3381` submits the ADDR before the reconfig is attempted; after a refusal the ADDR applies later and re-adds the
target with `pending_source=0`, hidden from every report (`:3057`, `:3336`) while `k_server_reconnect` keeps dialing
it.  `:3778-3779` sends the SHUTDOWN ack first and only then stops, so a failed send answers the request zero times
and performs no shutdown.

## D. Client and CLI

### D1 `[me]` **[FIXED]** HIGH: script mode still hangs when the host is unreachable, and always exits 0

`code/kdbctl.c:744` `if(!cli.tty&&!k_client_busy(&app)&&g_cli_out_count>0){` - `g_cli_out_count` counts only
`app->output` (`:568`), which a client that never reaches a server never touches, so stdin is never read, EOF is
never seen, and the process spins.  The same loop assigns `rc` only for an event-loop failure (`:761-762`), so a
script whose commands time out still exits 0.  Both are the class the one-shot path already fixed.

**Fix, two halves.** Script mode now bounds its opening wait with the same budget the one-shot handshake uses
(`K_CLI_SETTLE_BUDGET_US`): with no output ever arriving it prints `no output from the server within 6000ms;
script mode needs a working connection` and exits non-zero instead of spinning.  And after the loop the exit
status reflects what happened inside the script - `app.error_count` (the client's own count of responses that were
neither OK, NOT_FOUND nor REDIRECT) makes any failed command fail the run.  Smoke: `printf 'GET k\n' | kdbctl
127.0.0.1:1` must exit 1 and must not hang (the check's `timeout 30` would turn a hang into 124).

### D2 `[me]` **[FIXED]** HIGH: one-shot mode reports success for a request that was only redirected (and for a failed CAS)

`code/kdbctl.c:714` `if(cmd_id&&app.last_done_id==cmd_id&&!k_client_busy(&app)&&app.last_done_status!=K_STATUS_ERROR) one_shot_ok=1;`
`last_done_id/status` are set *before* redirect handling (`kclient.h:239-240`), so a swept or dropped redirect
leaves status REDIRECT (3) ≠ ERROR (2) ⇒ `one_shot_ok=1`, no error line, exit 0.  A compare failure is CONFLICT
(4), so a `CAS` that did not swap also exits 0.

**Fix.** The success test now reads the response STATUS rather than "anything but ERROR": a final REDIRECT means no
leader ever accepted the command, so it prints why and fails; a CONFLICT (the command ran, its condition was false)
gets its own exit status, 2, so a script can tell it from both success and failure; OK and NOT_FOUND stay 0, since
an absent key is a legitimate answer to a read.  A usage error or a locally refused command stays 1.

### D3 `[me]` **[FIXED]** MEDIUM: PIPE's latency percentiles are the 5th/9th/9.9th

`code/kdbctl.c:465-468` `pct=(i==0)?50u:…` with `sorted[(pct*(n-1))/1000u]` - the divisor suits `p99.9`/`max`
only, so `p50` is `sorted[n/20]`.  Every reported percentile except the tail is ~10× too good.

**Fix.** The table is per mille and the percent labels were written as percent: `p50` indexed `50*(n-1)/1000`.  The
values are now 500/900/990/999/1000.  Honest limit on the evidence: wrong percentiles of a monotone distribution
still satisfy `min<=p50<=p90<=p99<=max`, so the smoke check can only pin the labels and that ordering - the
arithmetic rests on the one-line table plus inspection.

### D4 `[me]` **[FIXED]** MEDIUM: PIPE can exit 0 while requests were dropped, and its rate numerator counts redirects

`code/kdbctl.c:549` `return app->done_count?(g_pipe_status_other?1:0):-1;` - counters move only in the completion
hook (`:428-442`), which requests dropped by the client's deadline sweep or give-up paths never reach; and `:545`
feeds `ops_per_s` the redirect-inclusive `app->done_count` while `ok/not_found/other` exclude redirects.

**Fix, two halves.** The run now exits non-zero unless it actually completed: `end=complete`, every request queued,
no redirect-excluded failures, no `other` statuses, and at least one completion - `end=` was already printed but
nothing acted on it.  And the rate numerator is a new `g_pipe_done` incremented in the pipe's own completion hook
after the redirect early-return, because the client's `done_count` counts redirect responses too, so the old figure
charged the run for work it did not do.

### D5 `[audit]` MEDIUM: deadlines are armed at queue time and never re-armed on (re)send

`kclient.h:364` sets `deadline_us` when the request is queued; `k_client_send_pending` refreshes `sent_us` only.  A
request queued while disconnected - or re-sent after a redirect 4.9 s in - is reported "outcome unknown" although
it was never transmitted, or was transmitted with no time left.  This contradicts the file's own contract
(`kclient.h:14-15`, "an ALREADY-SENT request").

### D6 `[audit]` MEDIUM: give-up paths drop requests silently and can leave `discovering` set forever

`kclient.h:466-469` (and `:500-503`) free the whole pending list with no counter, no `on_done`, and no
`app->discovering=0`; a retained discovery MEMBERS then suppresses every later MEMBERS reply, so a manual
`members` prints nothing for the rest of the session.

### D7 `[audit]` MEDIUM/LOW: locally-refused one-shot commands burn the 6 s budget; one-shot argv re-joining splits arguments; the initial connect is never retried

`:703-708` waits on `g_cli_out_count>0`, which a locally-refused command never sets ⇒ every usage error costs 6 s;
`:785-792` re-joins argv with single spaces and re-tokenizes, so `SET k "a b"` silently writes a different key set;
`:645` does not arm a reconnect while `k_client_queue` keeps accepting and arming deadlines, so a hostname that
does not resolve yields "may or may not have been applied" for requests never sent.

### D8 `[audit]` LOW: `EXIT` in one-shot reports failure; no redirect loop bound; `host_index` can leave range; the RGET limit guard is dead on 32-bit `unsigned long`; dead signal state

`:727-732` treats `EXIT` as "no request was sent" ⇒ exit 1; `kclient.h:247-263` has no loop bound and no
same-target check; `kclient.h:184` shrinks `host_count` without clamping `host_index`; `kdbctl.c:285`
`parsed>4294967295ul` is constant-false where `unsigned long` is 32-bit (MSVC 6/Win32), so an overflowing limit is
clamped to `ULONG_MAX` and accepted; `g_cli_out_truncated`, `cli->print_truncated`, `g_cli_last` are written and
never read, and `PIPE` is advertised (`:490`) but absent from the command table.

## E. Tests, gates, harnesses

### E1 `[me]` **[FIXED]** HIGH: the gate has no per-layer timeout, so a hang is neither a pass nor a failure

`grep -c timeout build.sh` = 0, while `build.sh:330` states the contract - every layer must produce a verdict line,
no verdict is a failure - which can never be evaluated if the layer never returns.  With E2 this turns a bug into a
stalled gate.

**Fix, with a red device.** `reg_gate` now runs each layer in the background under a bash-native watchdog
(`REG_LAYER_TIMEOUT_S`, default 1800 s; per layer: `REG_LAYER_TIMEOUT_S=3600 reg_gate ...`).  A bash built-in is
used deliberately: `timeout(1)` is not present on every platform this script must run on (macOS).  Evidence: the
`reg_report`/`reg_gate` bodies extracted verbatim from `build.sh` and fed a layer that sleeps 30 s with a 3 s limit
produce `GATE|hang_probe|FAIL|rc=124 [layer timeout 3s] verdict=<no verdict line>|6s` in ~7 s, where the old code
would still be waiting.  Both gates then ran green with all 10 and 13 layers under the watchdog.

### E2 `[audit]` HIGH: `tests/cemon_test.c:242` is a tautology

`TEST_ASSERT(1,"destroy returned");` in `test_destroy_bounded_drain`; the property is only observable as a hang, so
reverting the fix produces a stall (E1), not a red layer.

### E3 `[me]` **[FIXED]** MEDIUM: the new "git-derived lists are non-empty" rule reports before the tests list exists

`tools/check-principles.py:202` `report('git-derived file lists are non-empty …', GIT_LIST_PROBLEMS)` while `:255`
`TESTS_C = git_files('tests/*.c','tests/*.h')` builds that list afterwards - an empty tests list still leaves the
C99 ratchet green.

**Fix, and a correction of the finding's wording.** One rule now reports emptiness after EVERY `git_files` call and
prints how many files it saw.  Precise position, verified: `git_files` is called at `:156` (`*.sh`) and `:254`
(`TESTS_C`), and the previous report sat at `:202` - between them - so the `tests/*` list was indeed the one whose
emptiness was invisible, exactly as the finding said.  Running the checker in a copy of the tree with no `.git`
shows the failure and the count (`FAIL ... (tests/ has 0 C/H files)`, `PRINCIPLES|FAIL|rules=19 fail=1`); that run
would have failed before this change too, because the `*.sh` list is built before the old report, so it is not a
differential test for this line - the differential evidence is the call order above plus the merged rule.

### E4 `[audit]` MEDIUM: `is_char_literal` cannot lex escaped literals; two demonstrable false negatives in the `//` rule

Executed by the auditor: `is_char_literal("'\n'")` is False, so escaped literals are treated as code; and
`rule('x=1;//')` / `rule('case 3://note')` are False (the `[^/]` and `[^:]` guards).  No live occurrences today.

### E5 `[audit]` MEDIUM: `watch_counters.sh` greps field names the server never emits; several harnesses advertise gates they do not enforce

`watch_counters.sh:22` greps `client_requests|request_count|request_bytes` while the emitted names are
`wal_inflight=`, `client_connections=`, `pending_requests=`, `pending_request_bytes=` - so the in-flight leak
signal the harness exists for is never printed.  `pipe_verify.sh`, `pipe_frontier.sh`, `perf_matrix.sh`,
`cpu_probe.sh`, `crash_hunt.sh`, `pageheap_hunt.sh` print reports and end with `echo done`: no comparison, no
readiness certification, so a failed build can read as a crash.  `regress_selftest.sh` is never invoked by the
gate; nothing checks the layer count; `bench_persist.c` counts responses as accepted writes; `raft_fuzz`'s
`done: N iterations` counts requested iterations, not ones that ran.

### E6 `[audit]` MEDIUM: `bench_rate`'s `unique` workload flag is unreachable from every harness that passes it

`bench_rate.c:52` `int unique=argc>6?atoi(argv[6]):0;` while callers pass the string `unique`
(`rate_profile.sh:90,101`, `perf_matrix.sh:53-57`, `cpu_probe.sh`, `burst.sh`) ⇒ `atoi` yields 0, so every
"unique" run is single-key and the CASE line still labels it `mode=unique`.

### E7 `[audit]` LOW: the `sprintf` pattern cannot match column 0; the `raft_inspect` rule counts sites but never where they are; `git_out` ignores return codes; `cli_smoke.sh`'s piped assertions use 1-char needles (`contain … "a"`) that the failure text itself satisfies

## F. Documentation

### F1 `[me]` - the ledger's own claim about A1 is false

`doc/gaps-audit.md:1128` states that with the metadata present "no records found" cannot happen.  A1 shows it can,
and silently.  §N's ceiling description must also be corrected: the residual it records as "64 segments of 64 MiB"
is implemented as **2** segments when the metadata file is absent (`K_WAL_SCAN_EMPTY_PREFIX_MAX_UNUSED_SLOT`, `:66`).

### F2 `[audit]` - wrong or stale statements found by checking the docs against the code

`doc/gaps-audit.md:1201-1210` (§R): the `vfs.h:308/310/324/326` references do not resolve and "read/write 的拷贝
也在锁内" contradicts both the code and §T.  `:1230` (§S): the tty-gated reader is `cli.h:731`, not `:736`.
`:1256` and `doc/code-review-2026-09c.md:306`: `cli_smoke.sh` has 22 assertions, not 19.  `:1079` and
`doc/principles.md:47`: `k_open_fail` is at `kserver.h:5324`, not `:5163`.  `doc/principles.md:27`: "CI has a
separate LF step" is false (the step was deleted; the gate's `principles` layer owns the rule).  `:94,97`: the
`vfs.h` and `treap.h` 64-bit references point at the wrong lines (`vfs.h:13-17`, `treap.h:13-19`/`:115-119`).
`doc/testing.md:8` 16 files not 15; `:23` 11 executables not 10; `:37-39` omits the `vfs-fault-test` targets;
`:311-312` "four unit suites" is five; `:330-331` is a stale paragraph.  `doc/gaps-audit.md:282`/`:187`:
`kserver_test`'s first case is `[1/37]`, not `[1/33]`; `:411`: `EV_MAX` is 8192, not 1024.  The status table in
`code-review-2026-09c.md` is right where it counts fixes, except the "19 checks" figure and the claim that the
membership note was moved onto the request (C1).

## G. Already closed before the audits returned

**The mem lock reached through `file->be`** - found by investigating two pushes with a red linux-gate job
(`cd4252e`, `1887455`), fixed in `62790f5` (a file-scope `static vfs_spin vfs_mem_lock`), confirmed by that job
turning `success` and by both jobs being green on the fix.  Evidence: in the `regress-logs-linux` artifact every
other layer is green (`cli_smoke: PASS`, `kserver 37/37`, `raft 221/221`, `selftest: PASS`, `PRINCIPLES|OK|rules=19
fail=0`) while `vfs_fault_test.log` stops after `BEGIN [1/5]` - that layer hung.  `1887455` touches one markdown
file, so the failure could not come from its diff; the local full gate was green *through the same code*, which is
the substance of the finding.  Lessons in the skill reference: a lock must be reached through the object's own
instance, and a green local gate is not a green gate - read the pushed run and let the artifacts name the layer.

## H. Fix order, and what each fix has to prove

1. **A1** - the ceiling must never return success while the metadata names an acknowledged record; continue the
   scan near `next.segment` before giving up, and give up loudly.  Evidence: a store with a released prefix beyond
   the ceiling must refuse to start, and the existing small-store tests must stay green.
2. **A2/A3** - **done**: refuted in the reachable forms by experiment, the misleading comment rewritten, the property pinned by `test_wal_recovery_refuses_a_middle_segment_cut`.
3. **A5/A6** - base-0 acceptance and install-without-truncate: silent-corruption paths with cheap guards.
4. **B1/B4** - the double close after a bounded wait, and the rx buffer freed under the reader: one line each, plus
   an invariant the code states rather than assumes.
5. **C1/C2** - the note belongs to a request, and must fit.
6. **D1-D4** - the CLI's exits and statistics: the operator's and the agent's view of what happened.
7. **E1-E5** - the gate's own blind spots: the fixes above need a gate that can fail, time out and be checked.
8. **F1/F2** - the docs last, so the correction is the final state and not a guess.
