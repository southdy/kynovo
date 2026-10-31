#!/usr/bin/env python
"""wal_inspect -- read-only forensics for a kynovo store's WAL.

Recovery refuses a store by printing one line and exiting; that line names the
segment, the offset and the generations involved, but it cannot say whether the
neighbouring segment is the damaged one, whether the bytes are corrupt or merely
out of order, or where the write position the metadata slot recorded actually
points.  This tool answers those from the files on disk, without opening the
store and without writing anything.

It walks exactly what k_server_open's scan walks (code/kserver.h, "recovery" in
doc/crash-contract.md) and applies the same rules:

  * a record header is magic/version/generation(u64)/payload_size(u32)/crc(u32);
  * the CRC covers the PAYLOAD only, so a wrong generation is invisible to it -
    which is why the generation is checked structurally, not by checksum;
  * inside a segment each record's generation is the previous one + 1;
  * the first complete record of a segment continues the previous EXISTING
    segment: exactly +1 when that segment ended at a record boundary (a clean end
    or a torn payload, whose header is complete), strictly greater when it ended
    inside a header (a rewrite may have consumed generations that are gone);
  * a torn tail at the END of the log is a crash artefact and is tolerated;
    anything damaged in the middle of history is not.

Refusal lines use the server's own wording, so a tool run can be diffed line by
line against the server's log.

Usage:
  python tools/harness/wal_inspect.py <store-base> [--records]

<store-base> is the same prefix handed to `kdbsvr init` / `kdbsvr server`
(e.g.  D:/kynovo-store/n1 ).  The files it reads are
<store-base>.wal.<20-digit segment> and <store-base>.wal.meta.

Exit codes (a run that inspected nothing is not a clean run):
  0  every record verified, generations continuous
  1  a problem was found and named below (corruption, a gap, a lost record)
  2  nothing was inspected (no segment under that base holds a record)
"""

import os
import re
import sys
import zlib

K_WAL_MAGIC = 0x314C574B
K_WAL_VERSION = 1
K_WAL_HEADER_SIZE = 24
K_STATE_MAX = 1024 * 1024 * 1024  # the upper bound the server accepts for payload_size

K_WAL_META_MAGIC = 0x314D574B
K_WAL_META_VERSION = 2
K_WAL_META_SLOT_SIZE = 128
K_WAL_META_SLOT_GAP = 4096
K_WAL_META_CRC_LEN = 56

SEG_RE = re.compile(r"^\.wal\.(\d{20})$")


def read_u32(b, off):
    return int.from_bytes(b[off:off + 4], "little")


def read_u64(b, off):
    return int.from_bytes(b[off:off + 8], "little")


def read_i64(b, off):
    return int.from_bytes(b[off:off + 8], "little", signed=True)


def decode_meta_slot(blob):
    """Return (slot, None) or (None, reason)."""
    if len(blob) < K_WAL_META_SLOT_SIZE:
        return None, "short ({} byte(s))".format(len(blob))
    magic = read_u32(blob, 0)
    version = read_u32(blob, 4)
    if magic != K_WAL_META_MAGIC:
        return None, "bad magic 0x{:08x}".format(magic)
    if version != K_WAL_META_VERSION:
        return None, "version {} (expected {})".format(version, K_WAL_META_VERSION)
    if (zlib.crc32(blob[:K_WAL_META_CRC_LEN]) & 0xFFFFFFFF) != read_u32(blob, K_WAL_META_CRC_LEN):
        return None, "CRC mismatch"
    return {
        "generation": read_u64(blob, 8),
        "record_segment": read_u64(blob, 16),
        "record_offset": read_u64(blob, 24),
        "record_size": read_u64(blob, 32),
        "next_segment": read_u64(blob, 40),
        "next_offset": read_u64(blob, 48),
    }, None


def inspect_meta(base):
    """Return (newest decodable slot or None, notes)."""
    path = base + ".wal.meta"
    if not os.path.exists(path):
        return None, ["meta: {} does not exist (never written, or the file is gone)".format(path)]
    notes = []
    with open(path, "rb") as fh:
        blob = fh.read()
    slots = []
    for index, off in ((0, 0), (1, K_WAL_META_SLOT_GAP)):
        slot, reason = decode_meta_slot(blob[off:off + K_WAL_META_SLOT_SIZE])
        if slot is None:
            notes.append("meta: slot {} (offset {}) does not decode: {}".format(
                "AB"[index], off, reason))
            continue
        slots.append((index, slot))
        notes.append(
            "meta: slot {} generation={} record=seg{}@{} size={} next=seg{}@{}".format(
                "AB"[index], slot["generation"], slot["record_segment"],
                slot["record_offset"], slot["record_size"],
                slot["next_segment"], slot["next_offset"]))
    if not slots:
        return None, notes + ["meta: no usable slot - recovery would fall back to the segment scan"]
    # k_wal_meta_load takes the newest generation, ties broken by the further write position
    order = sorted(slots, key=lambda item: (item[1]["generation"], item[1]["next_segment"],
                                            item[1]["next_offset"]))
    best = order[-1]
    if len(order) > 1 and order[0][1]["generation"] != best[1]["generation"]:
        notes.append("meta: the slots disagree (slot {} generation={} is older than slot {} "
                     "generation={}); the newest wins".format(
                         "AB"[order[0][0]], order[0][1]["generation"],
                         "AB"[best[0]], best[1]["generation"]))
    return best[1], notes


def list_segments(base):
    directory = os.path.dirname(base) or "."
    name = os.path.basename(base)
    if not os.path.isdir(directory):
        return []
    found = []
    for entry in os.listdir(directory):
        if not entry.startswith(name):
            continue
        match = SEG_RE.match(entry[len(name):])
        if match:
            found.append((int(match.group(1)), os.path.join(directory, entry)))
    found.sort()
    return found


def walk_segment(number, path):
    """Return (records, tail) for one segment.

    tail is a dict: kind is clean / torn-header / torn-payload / damaged, text is a
    one-line description for the segment line, refusal is the message the server
    prints when the tail is a refusal (None for the tolerated ones), and message is
    the tolerated-torn-tail line the server prints as a diagnostic.
    """
    with open(path, "rb") as fh:
        data = fh.read()
    size = len(data)
    records = []
    off = 0
    while True:
        if off >= size:
            return records, {"kind": "clean", "refusal": None,
                             "text": "ends at a record boundary ({} byte(s))".format(size)}
        if size - off < K_WAL_HEADER_SIZE:
            return records, {
                "kind": "torn-header", "refusal": None,
                "text": "{} byte(s) short of a header at offset {} (generation unknowable)".format(
                    K_WAL_HEADER_SIZE - (size - off), off),
                "message": "wal: segment {} ends with an incomplete tail at offset {} "
                           "(that record was never completed, so it was never acked)".format(
                               number, off)}
        header = data[off:off + K_WAL_HEADER_SIZE]
        magic = read_u32(header, 0)
        version = read_u32(header, 4)
        generation = read_u64(header, 8)
        payload_size = read_u32(header, 16)
        crc_want = read_u32(header, 20)
        if magic != K_WAL_MAGIC or version != K_WAL_VERSION:
            return records, {
                "kind": "damaged",
                "text": "offset {} holds magic 0x{:08x} version {} - neither a record header "
                        "nor a torn tail".format(off, magic, version),
                "refusal": "wal: segment {} offset {} holds neither a record header nor a torn "
                           "tail (magic/version mismatch): refusing to recover".format(number, off)}
        if payload_size == 0 or payload_size > K_STATE_MAX or generation == 0:
            return records, {
                "kind": "damaged",
                "text": "offset {} has an illegal header (payload_size={} generation={})".format(
                    off, payload_size, generation),
                "refusal": "wal: segment {} offset {} has an illegal record header "
                           "(payload_size={} generation={}): refusing to recover".format(
                               number, off, payload_size, generation)}
        if off + K_WAL_HEADER_SIZE + payload_size > size:
            return records, {
                "kind": "torn-payload", "refusal": None,
                "text": "offset {}: header complete (generation {}), {} of {} payload byte(s) "
                        "present".format(off, generation,
                                         max(0, size - off - K_WAL_HEADER_SIZE), payload_size),
                "message": "wal: segment {} ends with a torn payload at offset {} "
                           "(the record was never completed, so it was never acked)".format(
                               number, off)}
        payload = data[off + K_WAL_HEADER_SIZE:off + K_WAL_HEADER_SIZE + payload_size]
        records.append({
            "segment": number,
            "offset": off,
            "generation": generation,
            "payload_size": payload_size,
            "crc_ok": (zlib.crc32(payload) & 0xFFFFFFFF) == crc_want,
            # Only the leading fields are decoded (the rest needs k_state_decode_record): the term
            # and the vote decide which record a recovered node would believe.
            "term": read_i64(payload, 0) if payload_size >= 8 else None,
            "voted_for": read_u32(payload, 8) if payload_size >= 12 else None,
        })
        off += K_WAL_HEADER_SIZE + payload_size


def main(argv):
    words = [a for a in argv[1:] if not a.startswith("--")]
    show_records = "--records" in argv[1:]
    if len(words) != 1:
        sys.stderr.write("usage: wal_inspect.py <store-base> [--records]\n")
        return 2
    base = words[0].rstrip("/\\")

    problems = []
    meta, meta_notes = inspect_meta(base)

    segments = list_segments(base)
    if not segments:
        print("wal_inspect: no {}.wal.<segment> file under {}".format(os.path.basename(base), base))
        return 2

    sizes = {}
    for number, path in segments:
        sizes[number] = os.path.getsize(path)

    prev_end_gen = 0        # last complete record of the previous EXISTING segment
    prev_seg = None
    prev_known = False      # is prev_end_gen+1 the only acceptable next generation?
    total = 0
    newest = None
    walked_cleanly = True   # did the walk reach the end of the chain with no damage?

    for number, path in segments:
        records, tail = walk_segment(number, path)
        first = records[0]["generation"] if records else None
        last = records[-1]["generation"] if records else None
        print("segment {}: {} byte(s), {} record(s), generation {}..{}, tail: {}".format(
            number, sizes[number], len(records),
            first if first is not None else "-", last if last is not None else "-", tail["text"]))
        if show_records:
            for record in records:
                print("    offset {:>8}  generation {:>6}  payload {:>7}  crc {}  term {}  vote {}"
                      .format(record["offset"], record["generation"], record["payload_size"],
                              "ok" if record["crc_ok"] else "BAD",
                              record["term"] if record["term"] is not None else "-",
                              record["voted_for"] if record["voted_for"] is not None else "-"))

        if not records and tail["kind"] == "clean":
            # The server walks past a segment holding no byte (a released prefix) without ending
            # the chain, so an empty segment takes no part in continuity either.
            continue

        if records:
            if prev_seg is not None and (first <= prev_end_gen or
                                         (prev_known and first != prev_end_gen + 1)):
                problems.append(
                    "wal: segment {} starts at generation {} but segment {} ended at {} ({}): "
                    "refusing to recover".format(
                        number, first, prev_seg, prev_end_gen,
                        "it ended at a record boundary, so generations must continue"
                        if prev_known else
                        "it was torn mid-header, so this segment must still be newer"))
            for index, record in enumerate(records):
                if index and record["generation"] != records[index - 1]["generation"] + 1:
                    problems.append(
                        "wal: segment {} jumps from generation {} to {} (a record is missing "
                        "inside the segment): refusing to recover".format(
                            number, records[index - 1]["generation"], record["generation"]))
                if not record["crc_ok"]:
                    problems.append(
                        "wal: segment {} offset {} fails its CRC check (generation {}): "
                        "refusing to recover".format(number, record["offset"],
                                                     record["generation"]))
            total += len(records)
            newest = records[-1]

        if tail.get("refusal"):
            problems.append(tail["refusal"])
        if tail["kind"] == "damaged":
            walked_cleanly = False
            break
        if tail["kind"] in ("torn-header", "torn-payload"):
            print(tail["message"])
            print("note: a torn tail at the end of the log is a crash artefact and is tolerated")
            walked_cleanly = False      # the scan stops here; later segments are never reached
            break

        prev_seg = number
        prev_end_gen = last if last is not None else prev_end_gen
        prev_known = True

    for note in meta_notes:
        print(note)

    if meta is not None and newest is not None:
        if meta["generation"] > newest["generation"]:
            line = ("the metadata slot records generation {} at seg{}@{} while the newest "
                    "complete record reached is generation {} (seg{}@{}): records the slot "
                    "counted are missing from the segments".format(
                        meta["generation"], meta["record_segment"], meta["record_offset"],
                        newest["generation"], newest["segment"], newest["offset"]))
            if walked_cleanly:
                problems.append(line)
            else:
                print("note: " + line + " (the walk stopped early, so this is not evidence on "
                      "its own)")
        elif meta["generation"] < newest["generation"]:
            print("note: the metadata slot is behind the log (slot generation {} < newest record "
                  "{}); it is written every 64 records, so this is normal inside that window"
                  .format(meta["generation"], newest["generation"]))
        if meta["next_segment"] not in sizes:
            problems.append(
                "the metadata slot's write position is seg{}@{} but no segment {} exists: the "
                "records it counts as written are not there".format(
                    meta["next_segment"], meta["next_offset"], meta["next_segment"]))
        elif meta["next_offset"] > sizes[meta["next_segment"]]:
            problems.append(
                "the metadata slot's write position is seg{}@{} but segment {} holds only {} "
                "byte(s): the records it counts as written are not there".format(
                    meta["next_segment"], meta["next_offset"], meta["next_segment"],
                    sizes[meta["next_segment"]]))

    if not total:
        print("wal_inspect: {} segment file(s) exist but none holds a record - nothing was "
              "inspected".format(len(segments)))
        return 2

    print("inspected {} segment(s), {} record(s); newest generation {}".format(
        len(segments), total, newest["generation"] if newest else "-"))
    if problems:
        print("verdict: NOT RECOVERABLE")
        for problem in problems:
            print("  " + problem)
        return 1
    print("verdict: consistent - every record verified and the generations are continuous")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
