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
does not). **Now covered**: the store-level snapshot harness (`drive_store`) drives one, and the case
`server WAL recovery keeps a snapshot base behind a released prefix past the scan ceiling` asserts the recovered
state through `treap_get`; its red proof (the jump neutralised) fails at `rc==0` / `it recovers`.

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

### A4 **[FIXED]** HIGH: after a torn tail, cross-segment generation continuity is not enforced, and term/vote can regress

`:1687` gates `:1712` `if(!have&&prev_seg_clean&&prev_seg_gen&&gen!=prev_seg_gen+1u){` on `prev_seg_clean`, so a
segment following a torn tail is accepted with no relation to the previous generation; `:1757` adopts term/vote from
the *last record read* (segment order), not the highest generation.

**Outcome: fixed, with a red case.**  Two changes, because the two halves had different answers.

*Continuity.*  A segment that merely follows a torn tail must still be **newer** than it, and how much more can be
required depends on where the tear is - the thing the old code got wrong in both directions.  A tear in the
PAYLOAD leaves the torn record's header (and therefore its generation) intact, so the next record continues at
exactly `+1`; a tear in the HEADER does not - a record was started, so at least one generation was consumed, but a
rewrite may have consumed more that is invisible now - so only `>` can be required there.  ">" alone is not enough
after a payload tear either: it would still accept a segment from a different history.  The claim in the old
comment, that a torn tail "may legitimately skip" and therefore justifies dropping the check entirely, is gone.

*Term/vote.*  `k_state_decode_record` assigned `restore->persist.term` unconditionally, so whichever record the
walk reached last won; a stale segment accepted after a torn tail could hand back an OLDER term with that older
term's vote attached.  A node that comes back believing an older term can grant a vote it already granted
elsewhere, or unseat a legitimate leader - which is why this is the round's HIGH item (Ongaro Sec. 5.1:
`currentTerm` "must never decrease").  The merge is by term now, with `voted_for` taken from the very record the
term came from; an equal term takes the later record's vote.

*Evidence.*  `test_wal_recovery_continues_a_torn_tail_by_generation` builds a store that really rotates (tiny
segment size), tears the last segment's tail, and continues in the next one: payload tear `+1` accepted, payload
tear `+2` refused, header tear `+2` accepted, header tear `+0` refused - and it asserts the rotation happened
rather than assuming it, because a continuation the walk never reaches would make every case pass for the wrong
reason (the first version of this case did exactly that: it wrote segment 1 while the walk stopped at the write
position, so all four cases were vacuous).  Red case: with the rule reverted to the old "no relation after a torn
tail", the case fails on `header tear + 0` - the stale segment is accepted and its state taken as the newest.  The
term merge itself has no red case of its own: with the continuity rule in place a stale segment no longer gets
that far, so it is defence in depth, and it is labelled as such rather than claimed as covered.

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
**Case (added with the store-level harness).** `forge_last_record_base` rewrites the last complete record of a segment so it names a base whose snapshot file was never written (payload CRC recomputed and stored back), and `server WAL recovery refuses a rollback to base 0 (it never certified a state)` requires the load to refuse. Red proof: removing the `prev_base>0` guard makes the store OPEN (48/49, at `rc!=0`) - the silent empty start this guard exists to stop.


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

### A7 **[FIXED; both reachable parts now red-proven, one third moot]** MEDIUM/LOW: the forced-high base decodes the wrong record; the verify helper creates the file it verifies; `skipped_prefix` is dead state

`:1802` `if(n_base<prev_base) n_base=prev_base;` but the decode at `:1822` uses `pick_base`'s segment/offset - the
*newest* record, carrying the lower base - so `:1847` refuses: fail-stop where the comment promises a successful
recovery.  `:1547` `file=vfs_open(path);` in the verify helper creates a missing `.snap.<index>`.  `:1592`
`k_u64 skipped_prefix;` is incremented and never read.

**Outcome.**

- *The forced-high base.*  The metadata was decoded from `bseg/boff/bsize`, and `bseg` is overwritten by **every**
  record, so it names the newest one - which, when a higher base is forced, carries the OLDER base and not the one
  being decoded.  The decode then failed and recovery refused, exactly where the rollback message it had just
  printed promised a successful recovery.  The first record that carried the base in use is now remembered
  (`base_rec_*`) and decoded from; the older-base path (`vseg/voff/vsize`) was already correct.  No case: staging it
  needs a store with two bases and a forced-high base on top - and no forging: a store whose newest record carries
  base 0 while an earlier one carried the real base is the observed restart shape, so `drive_store` builds it.
  **Covered** by `server WAL recovery decodes the first record that carried the base, not the newest`; red proof:
  using the newest record again fails it and A1's case together (48/50).
- *The verify helper.*  `k_snapshot_verify_file` opened the path with `vfs_open`, which CREATES a missing file, so
  asking whether `.snap.<index>` verifies left a zero-length file behind - a question that manufactured its own
  answer.  It now probes one byte and unlinks what it would have created, the same rule the WAL scan applies to the
  segments it probes.  **No case, deliberately**: without a vfs existence probe, "absent" and "present but empty"
  are indistinguishable from inside the process, so any assertion this suite could make would pass either way - a
  test that cannot fail is worse than no test.  A first version of the case was written, found not to discriminate
  (it passed with the fix reverted), and removed.
- *`skipped_prefix`.*  **Moot** - the variable no longer exists; it went with an earlier round's rewrite of the
  scan.
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

### C3 **[FIXED]** MEDIUM-HIGH: a refused change resets the *whole* wait clock

`:3398-3404` `pending_before=server->pending_count;` … `server->membership_pending_ms=0;`
`server->membership_pending_notice_ms=0; server->membership_notice_count=0;` - unconditional, while a second own
pending entry may be alive (layering is explicitly supported).  After 20 s waiting on change A, a refused change B
resets A's reported age to 0 and re-arms the notice budget.  The count rollback itself is correct.

**Outcome: fixed, and now red-proven.**  (It was recorded for a while as "defensive, no red proof" - that
sentence is retracted by the case below, not quietly dropped.)  The three resets are now guarded
by `k_server_membership_own_pending(server)==0`: only a refusal that leaves no wait of this node's own standing may
clear the clock.  The red proof could not be obtained because the only refusal shape the unit harness can stage is
the *address-log* submit failing before the reconfig is even attempted (a stale leadership view), where the clock is
untouched by construction; the reconfig refusal itself needs a real in-flight change, and the harness cannot keep a
catch-up target from graduating into the desired config.  The case that pins the stale-view shape is kept for what it
proves - the refusal answers an error and leaves the running wait's clock and notice budget alone - but it cannot go
red, and with the guard removed it still passes: it is recorded as such so nobody reads it as coverage of this fix.
What DOES cover it is `membership: a refusal by Raft leaves the clock of the change that is in flight`: node 2 is
pre-listed in the address book but is not a voter, the capture transport delivers nothing to it, so its change is
accepted and deferred (the cluster stays healthy on its one voter) - a change genuinely in flight - and a second
change is then refused by Raft itself (§4.1: at most one uncommitted config), which is the only refusal shape that
reaches the audited lines.  Red proof: with the clear unconditional again, the still-running wait's clock reads 100
ms instead of 1100 (50/51).  The election timeout is raised before the change, because at the default 250 ms the
leader would lose quorum contact with node 2 and step down, and a step-down refuses at the app layer - again before
the clock is read.

### C4 **[FIXED]** MEDIUM: `age_ms` is one server-global accumulator reported per entry, and `pending[]` is a bag

`:3065-3067` prints `server->membership_pending_ms` for every entry; the array is swapped-with-last on removal
(`:1330-1332`), so a young entry can be reported with an old entry's age.  The unit test pins the accumulator
semantics, so this is report-vs-behaviour, not a test gap.

**Outcome: fixed, with a red case.**  Each pending entry carries `pending_since_ms`, stamped from the server's
internal clock (`elapsed_total_ms`, the only clock a pure function may use), and TOPOLOGY prints that entry's own
age.  `membership_pending_ms` stays what it is - the server-level *wait* clock that drives the notices - so the two
quantities now have two names and two meanings instead of one being reported as the other.  New case: two targets
added 10 s apart report `age_ms=15000` and `age_ms=5000`; printing the global accumulator fails it.

### C5 **[FIXED]** MEDIUM: `membership_change_started`/`_completed` do not count the same thing

`:1329` increments `completed` inside the per-entry loop (one change adding N voters counts N) while `:3420`
increments `started` once per accepted change; a follower applying a change increments `completed` with
`started==0`.  `STATS` presents them as a pair.

**Outcome: fixed by naming both correctly, plus a real defect found while doing it.**  The counter is
`membership_targets_graduated`: it counts catch-up *targets* that graduated into the config, and a follower
applying a change graduates targets with `change_started==0`, which is coherent under that name.  While renaming,
the `STATS` format string turned out to print the label `membership_targets_gompleted` - a misspelling that would
have made any external scraper of that field miss it.  Fixed, and `doc/`, `tools/` and `tests/` contain no other
reference to either old name.

### C6 **[FIXED]** MEDIUM-LOW: TOPOLOGY is exempt from the admission gate but served through a barrier ⇒ a redirect ring

`:3595` exempts TOPOLOGY like INFO/STATS/HELP, but only those three are answered locally (`:3745`, `:3756-3774`);
TOPOLOGY goes through the barrier (`:3216-3239`) and, refused with `leader_id` still naming this node, answers
REDIRECT to the client's own endpoint, which the client follows with no self-check and no retry bound
(`kclient.h:247-264`).  MEMBERS, on the identical path, gets a hard "server stopping" - inconsistent.

**Outcome: fixed, with a red case.**  TOPOLOGY is now answered locally in the same diagnostics block as INFO and
STATS - it reads only application state, so it needs no barrier - and no longer enters the read barrier at all.  It
shares the single `raft_inspect` site, so the gate's budget-1 rule for that call is untouched.  New case: with the
server stopping (which refuses reads through the barrier) a TOPOLOGY request must be answered `OK` carrying
`role=`; pre-fix it comes back as a `REDIRECT` - naming the very node that received it - and the case fails.

### C7 **[REFUTED]** MEDIUM-LOW: a write queued behind a failed FCALL is stranded with no answer

`:3148-3153` frees the failed FCALL and `return`s, after `:3143` already cleared `gate_closed`, so the rest of
`gate_head` is neither answered nor redirected until another FCALL, leadership loss or release.

**Outcome: refuted against the current code - the guard exists.**  Freeing a request whose type is FCALL while the
gate is closed calls `k_server_open_gate` (`code/kserver.h:2204`, with a comment naming this exact deadlock), which
re-opens a fresh gate window over the queued writes so they are drained rather than stranded; and leadership loss
goes through `k_server_clear_gate`, which drains and redirects the whole queue by design (its comment names
"strand the tail of `gate_head`" as the thing it prevents).  The line numbers in the audit point at
`k_server_build_write_command` in this revision - they had drifted; the code they claim to describe does not.

### C8 **[FIXED]** LOW: a refused change leaves a phantom pending entry labelled "not ours"; `SHUTDOWN` depends on its ack

`:3381` submits the ADDR before the reconfig is attempted; after a refusal the ADDR applies later and re-adds the
target with `pending_source=0`, hidden from every report (`:3057`, `:3336`) while `k_server_reconnect` keeps dialing
it.  `:3778-3779` sends the SHUTDOWN ack first and only then stops, so a failed send answers the request zero times
and performs no shutdown.

**Outcome: both halves fixed.**  (a) `SHUTDOWN` now stops the server whether or not the ack could be sent: the ack
is a courtesy, the stop is the operator's intent, and a dropped connection used to turn the request into one that
answered nothing *and* shut nothing down.  (b) The targets of a change this node submitted are remembered when Raft
refuses it (and forgotten when a change is accepted), so the later ADDR apply - submitted before the reconfig on
purpose, because followers need the address first - marks their pending entries `source=-1`; TOPOLOGY reports them
as "left over from a refused change" instead of hiding them, and `k_server_membership_own_pending` counts only
sources 1 and 2, so a leftover is never mistaken for this node's own wait.  Red case: with the mark neutered, the
new case fails on `pending_source[0]==-1` (expected -1, got 0).

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

### E2 `[FIXED]`: `tests/cemon_test.c:242` was a tautology

`TEST_ASSERT(1,"destroy returned");` in `test_destroy_bounded_drain` - an assertion that cannot fail, so the case's
only evidence was the *absence* of a hang, and a hang would have wedged `cemon_test` instead of failing it.

Fixed by asserting what the case actually claims: the call returns inside a bound.  `t0=_test_now_us()` before
`cemon_destroy(loop)`, and `TEST_ASSERT(elapsed<CEMON_DESTROY_BOUND_US, ...)` after, where the bound is 10 s - 5x the
measured drain (`PASS ... 2000151 us`, i.e. 2.0 s) so a finite-but-absurd "just wait longer" regression fails it.
The comment says where the remaining case is caught, so the bound is not mistaken for the whole guard: an INFINITE
wait never returns, and *that* is the gate's per-layer watchdog (E1), which reports a layer that outlives its budget
as `FAIL rc=124` rather than letting the gate block forever.

**Red proof.** With the bound set to `0ul`: `SUMMARY: 4/5 passed` and
`-- FAIL cemon_destroy returns with a never-completing socket ref (bounded drain)` - the assertion really can fail,
which is exactly what could not be said of the line it replaced.  Restored to 10 s: `5/5`, 0 warnings,
`REGRESS|quick|pass=10 fail=0 duration=192s`.

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

### E4 `[FIXED]`: `is_char_literal` could not lex escapes; two demonstrable false negatives in the `//` rule

Both halves were confirmed by execution and are now fixed.

**The lexer.** `is_char_literal("'\n'")` was False: after a backslash the old code expected the closing quote
*immediately*, so every two-character escape was treated as code.  It now consumes the escape it actually finds -
hex (`\x41`), octal (`\0`, `\012`) or a single character - and then expects the quote.  `'\n'`, `'\x41'`,
`'\012'` and `'\0'` went from False to True; `'a'`, `'\'` are unchanged and prose like `don't` is still not a
literal.

**The comment rule.** `scan_raw(r'(^|[^:])//[^/]')` had two false negatives: `[^:]` hid `case 3://note`, and the
trailing `[^/]` hid a `//` with nothing after it (`int a;//` at end of file).  Dropping the `:` guard outright was
tried first and *failed loudly* - the checker went red on `mem://`/`disk://` prose inside block comments, which are
not `//` comments at all, because `strip_literals` blanks string contents but leaves comments.  The guard is
therefore narrowed, not removed: `scan_raw` gained an `allow(line, match)` predicate, and the rule excuses only a
`//` that belongs to a URI scheme (a letter-initial `<scheme>://`).  A scheme starts with a letter, so `3://` in
`case 3://note` is not one.

Measured truth table, five cases, old pattern vs new rule:

| case | old | new | wanted |
|---|---|---|---|
| `int a;//` (nothing after it) | missed | caught | caught |
| `switch(c){case 3://note` | missed | caught | caught |
| `/* mem:// runs like disk:// */` | not caught | not caught | not caught |
| `const char *u="disk://x";` | not caught | not caught | not caught |
| `x=1; // hello` | caught | caught | caught |

`PRINCIPLES|OK|rules=20 fail=0` on the tree (no live occurrences, as the audit said), and
`REGRESS|quick|pass=10 fail=0 duration=269s`.

### E5 `[FIXED]`, with one half refuted: harnesses that could not report what they claimed

**The counters harness grep'd field names the server has never emitted.**  `watch_counters.sh` filtered
`client_requests|wal_inflight_count|client_connection_count|request_count|request_bytes`; the STATS line
(`code/kserver.h:3830`) emits `wal_inflight=`, `client_connections=`, `pending_requests=`,
`pending_request_bytes=`.  So the harness whose stated purpose is to catch an admission leak printed an empty field
set and left the judgement to the reader.  It now greps the real names, **certifies that it measured something**
(a run where the counters are absent prints `STATS-FIELDS-MISSING ...` and the script exits 1 instead of looking
like a clean report), compares `pending_requests` before and after the runs - the leak it exists for - and ends in
`WATCH|PASS|...` or `WATCH|FAIL|...` with the corresponding exit code.

**Eleven harnesses ended in `echo done` and printed no verdict.**  None of them is in the gate chain (the only
harness `build.sh` calls is `soak_release.sh`), so the risk is a human or an agent reading `done` as "passed".
Each now carries a header line saying it is a report, not a gate, and that its numbers are not compared.

**`regress_selftest.sh` tested a lookalike, and was never run.**  It redefined `reg_report`/`reg_gate` inline - a
copy free to drift from the functions it claims to test - and no gate invoked it.  It now extracts the real
functions from `build.sh` by name and `eval`s them, and it runs **as a gate layer** (`regress_selftest`), so a
broken detector fails the gate it is part of.  It checks four cases: silent-but-exit-0, crash with no verdict,
a verdict line, and a layer that hangs (3 s budget -> `rc=124`).

That last case is the E1 proof, now permanent - and it immediately found a real defect in the gate itself: every
FAIL line read `[layer timeout 1800s]`, because `${timed_out:+ ...}` expands whenever the variable is *set*, and it
is set to `0` on the normal path.  A layer that exited 0 with no verdict line was reported as a timeout.  Fixed in
`reg_gate` (`to_note` only when `timed_out = 1`), and the selftest's four cases now pass:
`SELFTEST|PASS|the detector FAILs a silent layer, a crashing layer and a hanging one and passes a verdict line (4/4)`,
with `REGRESS|quick|pass=11 fail=0 duration=307s` (the layer count went 10/13/14 -> 11/14/15).

**`bench_persist.c` counted responses, not accepted writes.**  Its summary called every completed request a SET;
the label now says what was measured (`n=... errors=... (every request that completed, whatever its status)`),
backed by the client's own `error_count`.

**Refuted:** "`raft_fuzz`'s `done: N iterations` counts requested iterations, not ones that ran".  The loop
(`tests/raft_fuzz.c:541`) is a plain `for(i=0;i<count;i++)` with no early exit - the `break`s the audit may have
seen are in a helper above it - and it prints `done` only after the loop, so the count IS the number of iterations
that ran.  Left as it is, with this note so the claim is not re-raised.

### E6 `[audit]` MEDIUM: `bench_rate`'s `unique` workload flag is unreachable from every harness that passes it

`bench_rate.c:52` `int unique=argc>6?atoi(argv[6]):0;` while callers pass the string `unique`
(`rate_profile.sh:90,101`, `perf_matrix.sh:53-57`, `cpu_probe.sh`, `burst.sh`) ⇒ `atoi` yields 0, so every
"unique" run is single-key and the CASE line still labels it `mode=unique`.

### E7 `[FIXED]`: four blind spots in the checks themselves

1. **The `sprintf` ratchet could not see column 0.**  `scan(r'[^_a-zA-Z]sprintf\s*\(')` requires a character
   before the name, so a bare `sprintf(` at the start of a line was invisible.  Now `(^|[^_a-zA-Z])sprintf\s*\(`.
   The measured count is unchanged (10, budget 10) - this tree has no column-0 call - so the fix is about the rule
   being able to fail, not about a current violation.
2. **The `raft_inspect` rule reported a count but never the sites.**  With budget 1, a reader could not tell *which*
   call is the sanctioned one.  The ok line now carries it:
   `raft_inspect only in the diagnostics path (budget 1): allowed=['code/kserver.h:3825: ...']`.
3. **`git_out` ignored the return code.**  A failing git produced empty stdout, which is exactly what makes a rule
   green for want of files (E3's problem from the other side).  `git_out` now records non-zero exits and a new rule
   reports them.  That is rule 20 - `PRINCIPLES|OK|rules=20 fail=0`.

   **The first version of that rule was wrong, and CI said so.**  It recorded *every* git call, and the rules also
   run `git diff --name-only HEAD~1 HEAD` - which exits 128 on CI, because CI checks out a **shallow clone** where
   `HEAD~1` does not exist.  Both CI jobs went red on the commit that added the rule while the tree was fine, which
   is exactly what a new rule is for and the honest way to find out.  The rule now records only the list-producing
   calls (`git ls-files ...`), whose emptiness is the state that makes rules green; a shallow clone's missing
   `HEAD~1` is a property of the checkout, not of the tree.  Reproduced locally in a clone made the same way
   (`git clone --depth 1`): before the narrowing, `FAIL every git command the rules rely on exited 0` plus
   `PRINCIPLES|FAIL|rules=20 fail=1`; after it, `PRINCIPLES|OK|rules=20 fail=0`.  The renamed rule says what it
   checks: `every file list the rules read came from a git that exited 0`
4. **`cli_smoke.sh`'s piped assertions accepted one-character needles.**  `contain "piped script ran the first
   command" "$out" "a"` was satisfied by any failure message containing an "a".  The script now stores
   `VAL_ONE`/`VAL_TWO` and asserts on those, so the check can only pass if the values actually came back.

### One observation from this round's gate runs: a single false RED

While the E7 tree was being verified, `./build.sh regress quick` reported
`REGRESS|quick|pass=10 fail=1 duration=418s`, with `GATE|cli_smoke|FAIL|rc=0 verdict=<no verdict line>|43s` - and the
layer's own log (`build/regress/cli_smoke.log`) contradicted it: 863 bytes, 32 `PASS`, 0 `FAIL`, ending in
`cli_smoke: PASS`.  The gate's own grep, run afterwards against that same file, matches.

What was checked rather than assumed: the same layer, under the same `reg_gate` extracted from `build.sh`, three
times in a row - `ok` 3/3, each log identical in size and content.  `tests/cli_smoke.sh`'s `say()` is a plain
`printf` with no pipe or background writer, so a late-flushing writer inside the layer is not the mechanism.  The
one external fact that lines up is that the shell running the gate was killed by the tool harness at 420 s while the
gate itself reported 418 s - **that is a suspicion, not a finding**, and it is written down as one.

`reg_gate` now re-reads the log up to three times (0.3 s apart) before declaring a verdict line absent.  That does
not explain the observation; it only means a verdict line that lands a moment late cannot be reported as missing.
The gate re-ran clean immediately after: `REGRESS|quick|pass=11 fail=0 duration=224s`.

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

## Closing state

Every item in this round is closed one way or the other - fixed, or refuted with the experiment that refuted it:

| item | outcome |
|---|---|
| A1 | fixed (`1393b75`) - the scan ceiling no longer reports a released prefix as an empty store |
| A2, A3 | **refuted** - a middle-segment cut is caught by the acknowledged-position check, measured; the misleading slot comment was rewritten and the property is now pinned by a test |
| A5, A6 | fixed (`a0ae3ae`) - base 0 is no longer a "verified snapshot", and an install may not inherit a stale tail; the fuzz refuted the first version of the second fix 5/5 |
| B1, B2 | fixed (`fc0f366`) - a worker that outlives its wait is left alone instead of being freed under; defensive, no red case, labelled as such |
| B4 | fixed (`fc0f366`) with a red case - the feed stops when a handler frees its buffer; the suite segfaults inside that case without the guard |
| C1, C2 | fixed (`c6a8026`) - the membership note travels with its request; a note that would not fit says so |
| D1-D4 | fixed (`f5caeef`) - the CLI's exit status stops reporting refusals, redirects, conflicts and truncated runs as success |
| E1, E3 | fixed (`d1031bb`) - every gate layer runs under a watchdog, and the git-list emptiness report sits after every list is built |
| F (doc numbers) | fixed (`c018ed3`), with two of the flagged numbers refuted rather than changed |
| E2 | fixed - the tautological assert now measures the bounded return, and fails when the bound is tightened |
| E4 | fixed - the char-literal lexer understands escapes, and the comment rule catches `case 3://note` and a trailing `//` without firing on `mem://` prose (5-case truth table) |
| E5 | fixed - the counters harness greps the fields the server emits and certifies it measured something; `regress_selftest` extracts the real detector and runs as a gate layer (it found a false "layer timeout" claim in `reg_gate`); 11 report-only harnesses now say so |
| E7 | fixed - the `sprintf` ratchet sees column 0, the `raft_inspect` rule names its sanctioned site, `git_out` records non-zero exits (rule 20), and `cli_smoke`'s piped needles are the values it stored |
| C3 | fixed - the wait clock is only cleared by a refusal that leaves no wait of this node's own; **red-proven** by `membership: a refusal by Raft leaves the clock of the change that is in flight` (unconditional clear leaves 100 ms instead of 1100) |
| C4 | fixed - every pending entry carries its own age from the server's internal clock (red case: 15 s vs 5 s) |
| C5 | fixed - the counter is named for what it counts; the `STATS` label it was printed under was misspelled and is fixed |
| C6 | fixed - TOPOLOGY is answered locally instead of through the barrier it is exempt from (red case: pre-fix it answers REDIRECT to itself) |
| C7 | **refuted** - freeing a failed FCALL already re-opens the gate, and leadership loss drains the queue by design |
| C8 | fixed - SHUTDOWN stops regardless of its ack; a refused change's leftover pending entry is marked and reported (red case: `pending_source[0]==-1`) |
| A4 | fixed - a segment continuing a torn tail must still be newer (exactly `+1` after a payload tear, `>` after a header tear), and term/vote merge by term so a term cannot regress (red case: the stale continuation is accepted with the rule reverted) |
| A7 | fixed - the forced-high base decodes the record that first carried it; the verify helper no longer creates the file it verifies (unred-proofed, see the section); `skipped_prefix` was already gone |

Evidence for the round as a whole: `REGRESS|full|pass=14 fail=0 duration=267s`, CI green on every push, and the XP
guest run recorded in `doc/testing.md` section 7 (`cemon 5/5`, `kclient 19/19`, `raft 221/221`, `kserver 41/41`,
`vfs_fault 5/5`, `error C`/`LNK` = 0, fuzz and the crash contract included).  The one thing the guest cannot cover
is `cli_smoke.sh`, which needs a shell - said out loud there rather than implied.
