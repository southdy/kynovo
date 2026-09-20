# kynovo full-project review — 2026-09c

Method: seven **read-only** module audits (raft / recovery+WAL+snapshot / request+protocol+membership+shutdown /
client+CLI / platform layer / tests+gates / docs+hygiene) run in parallel over the tree at `33b3eaa`, then
**every high-severity claim was checked against the code by hand** — that step is not optional here: previous
rounds produced confident sub-agent findings that the code refuted, so each item below carries a status:

- **VERIFIED** — I read the cited code and the mechanism holds as described;
- **REPORTED** — plausible and specific, not yet re-read by me; the quote is the auditor's.

Baseline evidence for this review (independent, `./build.sh regress full` on this machine):

```
GATE|build|ok|rc=0 diagnostics=0 binaries=yes [pinned, strict]|183s
GATE|principles|ok|PRINCIPLES|OK|rules=18 fail=0|7s
raft 221/221 · kserver 34/34 · kclient 16/16 · cemon 5/5 · vfs_fault 4/4
selftest PASS · selftest_cleanup 0 · cli_smoke PASS
raft_fuzz 20000 · raft_cluster_fuzz 2000 · kserver_cluster_fuzz 40 histories / 2509 ops, 0 inconclusive
soak_release SOAK|PASS|rounds_run=24 fails=0 liveness=1
REGRESS|full|pass=14 fail=0 duration=330s
```

Nothing below contradicts that: the gate is green and **the defects are in what the gate does not look at**
(an uncompiled branch, a memory-safety rule the deterministic transport cannot exhibit, a ratchet with slack,
and documents that drifted).

---

## 1. Memory safety and correctness

**1.1 [high, VERIFIED] A failed response send frees the connection while the frame reader is still using it**
`code/kserver.h:2309-2313` (in `k_server_send_response`, called from the frame dispatcher):
```c
    if(rc!=0){
      void *dead=conn->sock;
      conn->sock=0;                      /* clear BEFORE closing: the handle is invalid either way */
      server->transport->close(dead);
```
`code/cemon.h:2655` `if(notify) cemon_emit_terminal(sock,CEMON_CLOSED,status,0,0,0);` runs **inline** inside
`cemon_close`, the app's handler `code/kdbsvr.c:135-137` calls `k_conn_closed(conn)` (`kserver.h:2253`), and
that handler **frees the `k_conn`** — the file says so itself at `kserver.h:3705-3706` and `:3714`:
*"the handler frees this k_conn, so touching conn->sock afterwards writes into freed memory"*.

The caller is the frame path: `kserver.h:3710-3712`
```c
  rc=k_rx_feed(&conn->rx,K_CLIENT_MAGIC,data,size,k_server_client_frame,conn);
  if(conn->rx.len>=old_len) server->rx_buffer_bytes+=(k_u64)(conn->rx.len-old_len);
  else server->rx_buffer_bytes-=(k_u64)(old_len-conn->rx.len);
```
`k_rx_feed` (`kproto.h:212-214`) itself touches `rx->data`/`rx->len` after the handler returns. Trigger is
ordinary: `cemon_send` fails on send-queue backpressure (`cemon.h:2816`) — no exotic condition.
*Fix*: the frame handler must not free its own `conn`; defer the close to `k_server_client_received`
(a `closed` flag the reader checks), or make `k_conn_closed` only mark the connection dead and free it later.

**1.2 [high, VERIFIED] The same event double-subtracts `rx_buffer_bytes`, which wraps and then stalls every client**
`kserver.h:2287-2288` already subtracts on `k_conn_closed`; `kserver.h:3712` subtracts again from a **freed**
`conn->rx.len` with no clamp (`k_u64` ⇒ ~1.8e19 when it wraps). Then `rx_buffer_bytes>=K_RX_BYTES_MAX` is
permanently true: `kserver.h:3691` pauses every new client, `:3704` closes any connection that sends a byte,
`:4761` never re-arms. Same fix as 1.1.

## 2. Recovery and durability

**2.1 [high, VERIFIED — and half of it is my own change] "Metadata unusable + the prefix already released by a snapshot" recovers as an EMPTY store**
`kserver.h:1592-1604` and `:1704-1705`:
```c
  for(seg=0;seg<=last_seg||meta_absent;seg++){
    file=vfs_open(path);                       /* OPEN_ALWAYS: this CREATES the file */
    if(meta_absent){
      k_u8 probe_byte;
      if(vfs_read(file,0,&probe_byte,1u)!=0){ vfs_close(file); break; }
    }
...
  if(!records){
    if(meta_absent) return 1;                  /* no metadata and no parsable record: a never-written store */
```
A store that has snapshotted at least once has **no segment 0** (cleanup deletes `segment<keep_segment`). So with
a damaged metadata slot: `vfs_open` creates an empty segment 0, the 1-byte probe fails, the scan breaks with
`records==0`, and `return 1` reports "never written" — the entire history in segments k.. is orphaned, the server
starts empty and then writes a fresh WAL under the same names. `doc/crash-contract.md` already concedes a
segment file can be missing while its fsynced records were acked on POSIX, so this is acknowledged-data-loss
shaped, not theoretical.

This is the **unfinished half of the D10 fix I landed earlier today** (`09e6a3e`): the metadata-lost path does
scan — but only while segment 0 exists. And note the honest complication I hit while fixing it: with the current
vfs interface *"fresh store"* and *"damaged store whose prefix was released"* are **indistinguishable**
(`vfs_open` creates; `vfs_unlink` is destructive and also fails on absence; there is no read-only existence
test and no size query). Recommended: add the smallest read-only primitive (`vfs_exists`) and then fail-stop
loudly when the metadata is unusable and segment 0 is absent **but a snapshot exists**; a genuinely fresh store
never reaches the scan (it returns earlier via `meta_rc<=0`).

**2.2 [high, REPORTED] An absent or unopenable SEGMENT — including the newest — looks like a clean end of the chain**
Same loop: `if(!file){ if(meta_absent) break; continue; }` and the "clean end" probe at `:1618-1620` treat a
missing file exactly like a legitimate boundary, and the loader never compares what it scanned against
`meta->record` (only `meta->next.segment` is used, `:1585`). For the **newest** segment there is no later
segment to raise the cross-segment generation check, so its records are dropped and `meta->next` is rewound to
an older position — the next batch then writes over live positions.

**2.3 [high, REPORTED] An empty `.wal.meta` short-circuits recovery before any segment is read**
`kserver.h:1573-1574` `meta_rc=k_wal_meta_load(base,meta); if(meta_rc<=0) return meta_rc;` — `meta_rc==0` means
"the file exists (vfs_open created it) and holds zero bytes" (deleted/truncated slot), and the loader returns
before touching a single segment; the caller then re-initialises the metadata and appends at `{0,0}`
(`kserver.h:5272`). Three metadata-damage shapes behave inconsistently: one slot valid → scan; both slots
CRC-invalid → fail-stop; both slots zeroed → scan; file empty → empty store.

**2.4 [medium, REPORTED] Snapshot cleanup can delete a segment the ROLLBACK target still needs**
`kserver.h:4731` `server->snapshot_wal_segment=bundle->wal_result.next.segment;` (the position **after** the
base-carrying record) feeds `k_server_schedule_cleanup(...,snapshot_prev_wal_segment)` (`:4520`) which unlinks
`segment<keep_segment` (`:4442-4444`). When the base-carrying record ends exactly on a segment boundary the
segment holding it is deleted. After a restart the value is looser still (`:5262` seeds it with the startup
append position), so the first post-restart snapshot deletes everything written before the restart. Records are
deltas (`raft.h:3346-3356`), so a rollback then cannot be replayed — availability, not corruption (the loader
refuses), but the retention comment's promise is not kept.

**2.5 [low, REPORTED] A torn tail disables the NEXT segment's generation check entirely**
`kserver.h:1642` gates the cross-segment check on `prev_seg_clean`, which a torn tail clears (`:1702`), so a
following segment's first record is accepted with **no** relation to `prev_gen`. Tolerant rule should be
`gen==prev_gen+1 || gen==prev_gen+2` (the skipped generation being the torn record's).

**2.6 [low, REPORTED] Recovery CREATES files while scanning and leaves them behind when it refuses**, and the
snapshot "retry" branch is unreachable (`:4927` requires `task->serialized`, which only a finished worker sets)
while a failed result post leaves `snapshot_task`/`snapshot_inflight` set forever — so `k_server_maybe_finish_stop`
(`:4473`) can never fire.

## 3. The membership-visibility feature I landed today — two of its defects are mine

**3.1 [high, VERIFIED] Step 3 never reaches the operator: the client swallows the `MEMBER` body**
`code/kclient.h:269-270`:
```c
...||pending->type==K_REQ_TOPOLOGY||pending->type==K_REQ_SHUTDOWN) app->output(app->output_ud,"%.*s",...);
    else app->output(app->output_ud,"ok");
```
`K_REQ_MEMBER` is **not** in that whitelist, so the note the server attaches (`kserver.h:4019-4022`) is printed as
a bare `ok`. The gate rule added for exactly this class certifies the swallow as accounted for, with a comment
that is factually wrong (`tools/check-principles.py:270` "the discovery handshake, not the CLI" — the CLI does
issue it, via `k_client_queue_member`). *Fix*: add `K_REQ_MEMBER` to the whitelist, add a regression test shaped
like the existing SHUTDOWN one, correct the rule's comment.

**3.2 [medium, VERIFIED by arithmetic] The note is longer than its buffer on every delivery**
`kserver.h:551` `char membership_note[128];` vs the format at `:3290-3294`. Minimum rendering is 131 characters
(`"note: node 1 is still catching up (0ms, link=dialing); this change was applied on top of a config change that has not committed yet"`),
typical ≈139. `k_snprintf` truncates safely (`kbase.h:376` returns `strlen`), so the sentence the feature exists
to deliver always ends mid-word. *Fix*: shorten the text or size the buffer to `K_MEMBERSHIP_NOTE_MAX`.

**3.3 [medium, REPORTED — and it is a consequence of the newer ordering] A refused change leaves a phantom "catch-up wait" forever**
`kserver.h:3257` submits the ADDR entry **before** the reconfig is attempted, and the ADDR apply is what fills
`pending[]` (`:2657`). When the reconfig is refused (`reconfig rejected`, `:3270-3273`) nothing drains pending:
`k_membership_update` only drops ids that graduate (`:1301-1308`), and `pending_count=0` exists only on
CATCHUP_FAILED (`:3999`) and leadership loss (`:4829`). So `k_server_membership_tick` prints "waiting for node N"
one line per 10 s **forever**, `STATS` reports `membership_pending=1` with an ever-growing age, `TOPOLOGY`
reports a pending node, and `k_server_reconnect` dials a node that was never added. My new visibility work
therefore reports a change that does not exist. *Fix*: clear pending for the change that was refused, mirroring
the CATCHUP_FAILED branch.

**3.4 [medium, VERIFIED] The synchronous rejection path bypasses the new backoff**
`kserver.h:3348-3350` gates on `auto_replace_fail_streak`, which is incremented **only** in the result handler
(`:3963`). The synchronous failures (`:3368-3372`, `:3377-3381`) reset the phase and print but never touch the
streak — and the likeliest synchronous rejection is `raft.h:2193` "deferred reconfig in progress". So the
measured storm (16 attempts/36 s, each writing an ADDR + CONFIG entry) is only half-covered. *Fix*: count those
failures too.

**3.5 [medium, VERIFIED] The one-shot note can be cleared by someone else, and `membership_change_source` is global**
`kserver.h:3288` clears `membership_note_len` on **every** submit, including auto-replace's internal one
(`conn==0`), and `k_server_drive` runs `k_server_auto_replace` (`:4832`) **before** client results are consumed
(`:4629`) — so a same-round internal submit destroys the note the client was about to receive. Likewise
`membership_change_source` (`:3269`) is overwritten by any submit, so a tick can attribute a client's wait to
"submitted by auto-replace". *Fix*: put the note on the `k_request`, not on the server.

**3.6 [VERIFIED, checked and clean] No leak of the note into an unrelated response**: `membership_note_len=0`
runs at the head of every submit and no other terminal path consults it, so 3.5's clearing is a loss, not a leak.

## 4. Platform layer, and the code no gate can see

**4.1 [medium, VERIFIED] A real C89 violation sits in the POSIX branch, where no gate can go red on it**
`code/cemon.h:2584` (in `cemon_unix_recvfrom`):
```c
    if(rc>=0){
      sock->recv_armed=0;
      addr.len=(int)len;
      int emit_rc=cemon_emit_blockable(sock,CEMON_DATA,0,sock->udp_recv_buf,rc,&addr);
```
The sibling function at `:2547-2552` declares `int emit_rc;` at block top — the fix was applied there and not
here. MSVC 6.0 (the hard-constraint compiler) rejects mixed declarations outright; the 0-warning contract is
`PLATFORM=windows`-only (`build.sh:360-368`), so this line can never turn a gate red. *Fix*: hoist the
declaration, and consider making the Linux job fail on `warning:` in the POSIX branches.

**4.2 [high, REPORTED] The mem backend's inode table is a process-global singleton with no lock, and worker threads use it**
`code/vfs.h:414-422` (one `static vfs_mem_ctx vfs_mem`), with unsynchronised bucket walk/insert and
`refcount++/--` at `:293-326`, reached from the WAL worker (`kserver.h:1890/1898`) and the snapshot worker
(`:4440/4443`). The threaded stress path exists (`tests/kserver_test.c:1420-1425`, `--apply-stress … thread` with
`runtime_backend=0` ⇒ real threads over `mem://`). The disk backend is incidentally safe; the mem one is not.

**4.3 [medium, REPORTED] `vfs_open` never truncates and no `vfs_truncate` exists, so an InstallSnapshot receiver
writes into whatever was already at that path** (`vfs.h:96` `OPEN_ALWAYS`; `kserver.h:2324-2331` writes at
`snapshot_offset` with no unlink/truncate, and the loader's trailer check never probes past the trailer —
unlike the save path, which does at `:4419-4424`).

**4.4 [medium, REPORTED] `runtime.h`'s documented bound on worker handshake/exit waits does not exist on POSIX**
(`runtime.h:60-62` promises a bound; the Windows branch uses `WaitForSingleObject(...,RUNTIME_WAIT_MS)`, the
POSIX branches `pthread_cond_wait`/`pthread_join` with no timeout ⇒ one misbehaving worker hangs the process).
The kqueue branch is compiled by nothing; its fatal note names `epoll_wait` (`cemon.h:3303` inside the kqueue
`cemon_poll_once`) — the auditor extracted that branch into a stub and it *is* syntactically clean.

## 5. Client, CLI

**5.1 [medium, VERIFIED] Piped stdin is ignored and the process never exits** — `cli.h:477` requires a tty,
`kdbctl.c:697-708` has no EOF path; `printf 'GET k\n' | timeout 12 ./build/kdbctl.exe 127.0.0.1:1` → rc=124 with
zero bytes of output, even against a dead port (pre-documented as E1; still present, and now measured again).

**5.2 [medium, REPORTED] One-shot reports "the command was not executed" for a command that may be committed**
(`kdbctl.c:691` vs `kclient.h:481` "outcome unknown"), because the one-shot budget (3 s, `kdbctl.c:663/678`) is
**shorter** than the client's own request timeout (5 s, `kclient.h:18`).

**5.3 [medium, REPORTED] PIPE counts REDIRECT responses as completions** (`kdbctl.c:412-419`: `on_done` fires
before the redirect branch, so a run seeded on a follower reports `other>0` and **exits 1** although every op
committed; the published pipe-verify gates only ever seed a single node).

**5.4 [medium, REPORTED] PIPE `avg_us` divides a full sum by a capped sample count** (`kdbctl.c:415-416/447`,
`K_PIPE_SAMPLES`), so `count=100000` prints a mean ~12× too large next to correctly capped percentiles.

**5.5 [low, REPORTED] Smaller client items**: `RGET … limit abc` silently means "no limit" (`kdbctl.c:273-276`);
a REDIRECT to an unknown endpoint overwrites the endpoint being abandoned when the host list is full
(`kclient.h:253`, contradicting its own comment); PIPE truncates a value at 255 bytes (`kdbctl.c:462`);
`cli_print` emits `\r\n` on top of text-mode stdout ⇒ `\r\r\n` (`cli.h:791`), which is the trap the harnesses
have already been bitten by; `K_CLIENT_ARG_MAX 16` really yields 15 (`cli.h:627`); CLI caps node ids at 65535
while the protocol allows 2^31-1 (`kdbctl.c:112/119`).

## 6. Gates that do not enforce what they claim

**6.1 [high, VERIFIED] `scan()` re-scopes to the whole library when its file list is empty**
`tools/check-principles.py:109` and `:121` — `for path in (files or SRC):`. `TESTS_C` is git-derived (`:223`), so
in any tree where `git ls-files` returns nothing (no `.git`, a half-clone, a copy) the C99 rule scans **all of
`code/`**, and the four other git-derived predicates go **vacuously green on zero data** — the "0 failures must
not mean 0 data" failure the doctrine forbids. *Fix*: `files if files is not None else SRC`, and make an empty
git-derived list a hard FAIL.

**6.2 [high, VERIFIED] The primary CI job still carries the naive LF check that was deleted from its sibling as "a wrong one"**
`.github/workflows/ci.yml:47-55` `git ls-files --eol | grep -Ei '(i|w)/crlf'` — without the project's documented
exemption it flags **138** tracked files locally (all `doc/measurements/*` `attr/-text`), and in CI it checks 0
files. I removed the duplicate from `linux-gate` earlier today but left this one. *Fix*: delete it, or route the
check through `python tools/check-principles.py`.

**6.3 [high, VERIFIED] The `no // comments` rule is blind in 23 of the 26 files it scans**
`check-principles.py:174` scans `strip_literals()` output, whose literal detector fires on an **apostrophe inside
a comment** and blanks everything to the next apostrophe (`:82`). `code/raft.h` has 111 such apostrophes, so
large regions are invisible to the rule whose docstring records "a permanently green rule" as its reason to
exist. *Fix*: strip comments with a real state machine and keep literals.

**6.4 [low, VERIFIED] The sprintf ratchet has three free violations**
`check-principles.py:212` says `budget 13` with slice `sites[13:]`, and the tree has **10** hits — the message
number is hardcoded, not derived (unlike `BUDGET_C99_TESTS`). *Fix*: derive it and set it to the measurement.

**6.5 [low, REPORTED] Other gate scope holes**: `no 'inline'` does not match `__inline` (MSVC's own spelling);
both LF rules ignore git's `mixed` EOL class (10 such files exist); the 0-warning assertion is unenforced on
every runner and its regex cannot see linker warnings; `raft->config_*` / `raft_inspect` scopes are narrower
than their messages; the `raft.h`/`treap.h` "rule" only prints a NOTE yet counts as a rule; the fuzz layers'
`done: N iterations` line is the **argument**, not a measurement, so a run that executed 1 of 2000 iterations
reads identically; unit-suite layers accept any positive count.

## 7. Documentation drift (all REPORTED, spot-checked)

- **`doc/crash-contract.md:14-17` cites four code sites that do not contain the described code** (the record
  write is `kserver.h:1971`, the slot write+sync `:1924`, the snapshot sync `:2326`, the trailer read-back
  `:4423-4424`). This is the document that certifies durability.
- **`doc/testing.md:32`** still documents the L0 rule the gate deliberately abandoned (`build/build.log` +
  loose `grep ' error|warning'`) — the declared authoritative spec teaches the false-positive rule removed in
  `b9c64b2`.
- **`doc/gaps-audit.md:826-827`** lists `D13-D19`, `F2`, `A9/A10` and `C4` as still open; all were fixed
  (`152786a`, `1b3f813`, `9962d09`) and `doc/principles.md:47` still cites C4 as the open justification.
- **Layer/case counts are stale in every live doc that states them**: `AGENTS.md:21` says fuzz `pass=12` (13);
  `README.md:48/54` and `doc/testing.md:195/203` say 4 unit suites and `pass=9` (5 suites, 10); `doc/testing.md:48`
  says `kserver_test 33/33` (34/34, and the same file says 34 elsewhere); `doc/testing.md:265` says "22 sources"
  where the recorded XP run pulled 23.
- **Hygiene is clean**: no tracked build artifact, CRLF only under the exempted `doc/measurements/`, no stray or
  hand-written file in `build/`, `.gitattributes` ordering correct (the exemption wins).

---

## Fix order I recommend

1. **1.1 + 1.2** (one root cause): stop freeing the `k_conn` from inside the frame handler. Memory corruption on
   an ordinary path beats everything else on this list.
2. **3.1** — one line in `kclient.h` plus a regression test; without it the membership work I landed today has no
   operator-facing effect, and the gate rule that exists for this class is certifying the swallow.
3. **2.1 + 2.2 + 2.3** as one change: anchor recovery to the durable slot (`meta->record`) and fail-stop when the
   metadata is unusable and the prefix is gone; add the read-only `vfs_exists` primitive this needs. Silent
   empty-store recovery is the worst failure shape in the project.
4. **6.1 + 6.2 + 6.3** — three gate holes that all produce false greens, which is how the previous rounds' fixes
   got believed.
5. **3.3 + 3.4 + 3.5** — make the visibility feature tell the truth (clear pending on refusal, back off on
   synchronous rejections too, hang the note off the request).
6. **4.1** (one-line C89 fix) and **4.2** (lock the mem inode table or refuse the threaded mem combination), then
   the documentation corrections in §7.

## Status after the first fix round

| item | state | evidence |
|---|---|---|
| 1.1 + 1.2 connection freed inside a frame handler | fixed, `server: never free a connection from inside a frame handler` | new deterministic test; with the inline free restored the process dies inside it |
| 3.1 MEMBER body swallowed into `ok` | fixed, `client: print the MEMBER response body` | test reports `expected "note: node 4 is still catching up" / actual "ok"` without the whitelist entry |
| 4.1 declaration after a statement (POSIX branch) | fixed, plus the correction commit after the first hoist landed in the wrong function | linux-gate job: red then green (the job ④ added caught it twice) |
| 6.2 naive LF check in the primary CI job | fixed (removed; the principles layer owns the rule with its exemption) | YAML valid, `gate` steps = 8 |
| 2.1 / 2.2 / 2.3 recovery | fixed, `server: recover past a released prefix and refuse a missing newest segment` | probes + a deterministic fail-stop test; see doc/gaps-audit.md §N, including the residual that is deliberately kept |
| 3.3 / 3.4 / 3.5 membership visibility truthfulness | open | planned: per-entry source instead of a server-global, clear the pending report on a refusal, count a synchronous rejection in the auto-replace backoff |
| 4.2 mem backend inode table and the WAL/snapshot worker threads | open | |
| 6.1 / 6.3 gate holes (empty file list widens the scan; `//` rule blinded by apostrophes) | open | |
| 7 documentation drift (crash-contract line refs, testing.md's retired L0 rule) | partly fixed (counts and verdict examples in this round) | crash-contract refs still to do |
