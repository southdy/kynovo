# The crash contract

What survives a crash, on each platform, and why.  Written because the POSIX side was only ever
*compiled* before (issue #12); this is the part that is about semantics rather than buildability
(issue #18).  Every claim below names the code site or the measurement that establishes it.

## The sequence, and what survives at each point

A durable write is always: **write → `vfs_sync` → (later, optionally) `vfs_close`**, and the sync happens
in the same call that performs the write.

| point in the sequence | crash here loses | established by |
|---|---|---|
| bytes written, `vfs_sync` not yet returned | that record | `k_wal_append_bundle`: `vfs_write(header)` + `vfs_write(payload)` + `vfs_sync(file)` in one `if` (`code/kserver.h:2061`) |
| WAL metadata slot write | nothing acked after it; the slot is written only together with its own `vfs_sync` (`k_wal_meta_store`, `code/kserver.h:2014`) | two slots 4 KiB apart, newest-generation wins (`k_wal_meta_load`) |
| snapshot chunk written | the snapshot, which is then simply not there | chunks are written as they stream; the final `vfs_sync` happens only when `snapshot_done` (`k_server_write_inbound_snapshot`, `code/kserver.h:2432`) |
| snapshot complete: `vfs_sync` returned, read-back verification passed | nothing — the file is trusted from here | trailer written, `vfs_sync`, then the trailer is **read back** and the file is probed for bytes past the end (`k_snapshot_worker_save`, `code/kserver.h:4571-4581`) |
| `vfs_close` | nothing | everything durable was already synced; see the platform note below |

Each code reference above names the enclosing function as well as the line: the four line numbers this table
carried before were all wrong (they pointed at unrelated code, e.g. `kserver.h:1914` sits in the payload
encoder while the record write is `k_wal_append_bundle`), and a bare line number cannot be checked without
reading the file anyway.  Every one was re-verified against the tree when this was corrected.

**The claim that matters, measured rather than argued**: on the CentOS 7.9 guest, 2000 writes were
acknowledged (`pipe mode=SET k=64 n=2000 ... ok=2000 not_found=0 end=complete`), the server was killed
with `kill -9` (no cleanup, no flush-on-exit), and after restart the same 2000 keys read back with
`ok=2000 not_found=0` while a never-written prefix read `ok=0 not_found=2000`.  Recovery reported
`snapshot: verified 1042 (14542 bytes)` and no corruption.  So: **every acknowledged write survives a hard
crash**, and the check that says so is able to fail (the control prefix).

## Platform difference, named rather than assumed

* **`CloseHandle` flushes; POSIX `close()` does not.**  On Windows, closing a file handle pushes its
  buffered data to the device, so close is incidentally a durability point.  On POSIX, `close()` only
  releases the descriptor - the data is in the page cache and a crash before writeback loses it.
* **This difference is not relied upon anywhere.**  Both platforms end every durable write in `vfs_sync`
  in the same call (`code/vfs.h:173` = `FlushFileBuffers`, `code/vfs.h:175` = `fsync`), and no path writes
  and then closes without having synced.  The Windows flush-on-close is therefore a convenience, not a
  contract: a crash between the last `vfs_sync` and `vfs_close` loses nothing on either platform.

## Directory entries: deliberately not synced, and what that costs

POSIX makes a newly created file's **name** durable only after the containing directory is synced; Windows
has no equivalent step.  This project **does not implement a directory-sync primitive** - a deliberate
decision, not an omission - so the contract has to say what the consequence is:

* After a crash, a file created shortly before it may be **absent**; it is never *corrupt*.  The data file
  is `vfs_sync`'d and read back before anything trusts it, so a file that is present has a verified
  trailer (snapshots) or CRC-covered records (WAL).
* Recovery is written for absence: the snapshot retention is 2-deep (`snapshot_prev_index`,
  `snapshot_older_index`), the newest file is verified before use, and an unverifiable or missing one falls
  back to the previous verified version.  A missing WAL segment is skipped by the scan
  (`if(!file) continue;` - "released by snapshot cleanup"), and a hole *inside* the retained range is
  fail-stop rather than silently accepted.
* So the honest statement of the POSIX contract is: **acknowledged data is never lost, and recovery never
  has to guess** - but a crash can cost the *newest* snapshot or segment file, which self-heals by being
  rebuilt from the WAL on the next save.

## When recovery refuses: collecting the evidence

Recovery's refusal line is deliberately specific (each site is in `k_server_open`'s scan, `code/kserver.h`):

```
wal: segment 1 starts at generation 13 but segment 0 ended at 10 (it ended at a record boundary,
     so generations must continue): refusing to recover
```

It names the segment, the generation it starts at, the previous existing segment, the generation that segment
ended at, and which relation was required.  What it cannot say is *which side is wrong*: whether the bytes are
damaged, whether the segment is stale or foreign, or whether the write position the metadata slot recorded
still matches what the segments hold.  That is what `tools/harness/wal_inspect.py` answers, read-only:

```
python tools/harness/wal_inspect.py <store-base> [--records]
```

It walks the same records with the same rules (magic/version, `payload_size`, `generation`, CRC over the
payload only, `+1` inside a segment, `+1` or `>` across segments depending on where the previous segment was
torn) and prints, per segment, the byte count, the record count, the generation range and one of: clean end,
torn header, torn payload, or damage with its offset.  `--records` adds one line per record with offset,
generation, payload size, CRC status, term and vote - the two fields that decide what a recovered node would
believe.  Refusal lines use the server's wording verbatim, so a tool run can be diffed against the server's
log; on a deliberately forged store the two lines are identical:

| verdict | meaning | next step |
|---|---|---|
| `verdict: consistent` (exit 0) | every record verified, generations continuous | the refusal, if any, is elsewhere (config, snapshot, metadata) |
| `starts at generation N` / `jumps from generation A to B` (exit 1) | a segment does not continue the previous one, or a record is missing inside one | compare the two generations against the metadata slot line: the segment whose first generation is *not* `previous + 1` is the foreign or restored one |
| `fails its CRC check` (exit 1) | bytes are damaged, not merely mis-ordered | a single flipped byte is enough; the record's payload is unrecoverable, so the store must be restored from a peer or a snapshot |
| `holds neither a record header nor a torn tail` (exit 1) | mid-history damage (the crash-tolerated case is only the *final* record) | same as above |
| `records the slot counted are missing from the segments` (exit 1) | the metadata slot names a newer generation than the log holds | the WAL lost an acknowledged record; treat as data loss, not as a clean restart |
| `a torn tail ... tolerated` | the last record was never completed, so it was never acked | nothing to do; recovery accepts it |

Two limits, stated rather than implied.  It reads a **static** store: run it after the server has stopped, or
accept that a live store's tail may be one append ahead of the walk.  And it is for a `disk://` store - `mem://`
backing lives in the process, so there is nothing on disk to read.

**Certification.**  `python tools/harness/wal_inspect_selftest.py <store-base>` takes a store the *server*
wrote (real bytes, real CRC, real slots - not synthesised by the code under test), copies it six times, and
damages one thing in each copy: a generation bump on the newest segment (the header carries no CRC, so only the
structural rule can find it), a flipped payload byte, a torn tail, a deleted newest segment, an empty
directory, and the untouched baseline.  It asserts the exit code *and* the line for each, and a run without a
usable store exits 2 instead of passing.  Verified here against a 2-segment / 17-record store that
`kdbsvr init --wal-seg-size 4096` + 120 pipelined SETs produced: the tool reported 17 records, newest
generation 17 and a write position of seg1@2958, which are the server's own `wal_records=17`,
`persist_generation=17` and `wal_next_offset=2958` from `STATS`; every CRC read `ok`, which is what proves
`zlib.crc32` and `k_crc32` agree on real records.  Both directions of the self-test were exercised: with the
cross-segment rule disabled in the tool, exactly the generation-gap check fails; with the torn tail made fatal,
exactly the torn-tail check fails; restored, `7 check(s), 0 failure(s)`.

## Leaked assumptions: audited, none found

* The application layer is platform-neutral by construction: **`code/kserver.h` contains zero `_WIN32` /
  `_MSC_VER` conditionals.**  The shared headers that do carry them are mechanism-level only - `kbase.h`
  (17: types, 64-bit formats, the allocator, timing), `treap.h` (2), `raft.h` (1) - and every one of them
  is the platform *seam* rather than a behaviour assumed by the layer above.
* A scan of the shared headers for Windows-only API leaks (`CreateFile`, `GetLastError`, `stricmp`,
  `MAX_PATH`, `_open`, backslash paths) finds no genuine hits; the apparent matches are string-literal
  escapes (`\t`, `\n`) and identifiers that merely end in `_open`/`_close` (`vfs_open`, `vfs_close`).
* The two behaviours that *are* platform-different are both named above rather than hidden: close's
  flush-on-Windows, and directory-entry durability.  Neither is assumed by the code.
