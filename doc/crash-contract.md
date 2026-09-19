# The crash contract

What survives a crash, on each platform, and why.  Written because the POSIX side was only ever
*compiled* before (issue #12); this is the part that is about semantics rather than buildability
(issue #18).  Every claim below names the code site or the measurement that establishes it.

## The sequence, and what survives at each point

A durable write is always: **write → `vfs_sync` → (later, optionally) `vfs_close`**, and the sync happens
in the same call that performs the write.

| point in the sequence | crash here loses | established by |
|---|---|---|
| bytes written, `vfs_sync` not yet returned | that record | `k_server_flush_writes` → `k_wal_write_record`: `vfs_write(header)` + `vfs_write(payload)` + `vfs_sync(file)` in one `if` (`code/kserver.h:1914`) |
| WAL metadata slot write | nothing acked after it; the slot is written only together with its own `vfs_sync` (`code/kserver.h:1867`) | two slots 4 KiB apart, newest-generation wins (`k_wal_meta_load`) |
| snapshot chunk written | the snapshot, which is then simply not there | chunks are written as they stream; the final `vfs_sync` happens only when `snapshot_done` (`code/kserver.h:2269`) |
| snapshot complete: `vfs_sync` returned, read-back verification passed | nothing — the file is trusted from here | trailer written, `vfs_sync`, then the trailer is **read back** and the file is probed for bytes past the end (`code/kserver.h:4179-4194`) |
| `vfs_close` | nothing | everything durable was already synced; see the platform note below |

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
