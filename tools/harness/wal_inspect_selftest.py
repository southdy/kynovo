#!/usr/bin/env python
"""wal_inspect_selftest -- prove wal_inspect.py can fail.

The inspector is only worth having if it reports the problems it exists to find.
This script takes a store the SERVER wrote (so the bytes, the CRC and the
metadata slots are real, not synthesised by the same code under test), copies it
once per case, damages one thing in each copy, and asserts the exit code and the
line the tool must print.  A run that cannot find a real store does not pass: it
exits 2 and says what to create, because "no store" must never read as "clean".

Requires a store with at least two WAL segments.  Create one with a small
segment size and a handful of writes:

  d:/MinW64-15.2.0/bin/kdbsvr.exe init disk://<dir>/n1 --wal-seg-size 4096
  kdbsvr.exe server 1 <cport> <pport> disk://<dir>/n1 1@127.0.0.1:<cport>:<pport>
  kdbctl.exe 127.0.0.1:<cport> PIPE 8 120 SET kkk      # then SHUTDOWN

Usage:
  python tools/harness/wal_inspect_selftest.py <store-base>

Exit codes: 0 all checks passed, 1 a check failed, 2 no usable store to test.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "wal_inspect.py")
SEG_WIDTH = 20


def segment_path(base, number):
    return "{}.wal.{:0{w}d}".format(base, number, w=SEG_WIDTH)


def segments_of(base):
    directory = os.path.dirname(base) or "."
    name = os.path.basename(base)
    found = []
    for entry in os.listdir(directory):
        if entry.startswith(name + ".wal.") and entry[len(name) + 5:].isdigit():
            found.append(int(entry[len(name) + 5:]))
    return sorted(found)


def copy_store(source_base, destination_dir):
    """Copy every file of the store; return the new base."""
    destination_base = os.path.join(destination_dir, os.path.basename(source_base))
    for entry in os.listdir(os.path.dirname(source_base) or "."):
        if entry.startswith(os.path.basename(source_base) + "."):
            shutil.copyfile(os.path.join(os.path.dirname(source_base) or ".", entry),
                            os.path.join(destination_dir, entry))
    return destination_base


def first_record_header(base, number):
    with open(segment_path(base, number), "rb") as fh:
        return fh.read(24)


def run_tool(base):
    result = subprocess.run([sys.executable, TOOL, base],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result.returncode, result.stdout.decode("utf-8", "replace")


def bump_generation(base, number, delta):
    path = segment_path(base, number)
    with open(path, "rb") as fh:
        header = bytearray(fh.read(24))
    generation = struct.unpack("<Q", bytes(header[8:16]))[0]
    struct.pack_into("<Q", header, 8, generation + delta)
    with open(path, "r+b") as fh:
        fh.write(bytes(header))
    return generation, generation + delta


def flip_payload_byte(base, number, offset):
    path = segment_path(base, number)
    with open(path, "r+b") as fh:
        fh.seek(offset)
        byte = fh.read(1)
        fh.seek(offset)
        fh.write(bytes([byte[0] ^ 0xFF]))


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: wal_inspect_selftest.py <store-base>\n")
        return 2
    source = argv[1].rstrip("/\\")
    numbers = segments_of(source)
    if len(numbers) < 2:
        sys.stderr.write(
            "wal_inspect_selftest: {} holds {} WAL segment(s); at least two are needed so a\n"
            "cross-segment forgery exists.  See this file's header for the commands that build\n"
            "one.\n".format(source, len(numbers)))
        return 2

    work = tempfile.mkdtemp(prefix="wal_inspect_selftest-")
    failures = []
    checks = 0
    newest = numbers[-1]
    previous = numbers[-2]

    def check(name, ok, detail):
        nonlocal checks
        checks += 1
        if ok:
            print("PASS  {}".format(name))
        else:
            failures.append(name)
            print("FAIL  {}\n        {}".format(name, detail.replace("\n", "\n        ")))

    # 1. baseline: the untouched copy is consistent, and the tool actually read something
    baseline_dir = os.path.join(work, "baseline")
    os.makedirs(baseline_dir)
    baseline = copy_store(source, baseline_dir)
    code, out = run_tool(baseline)
    segments_seen = out.count("segment ") - out.count("starts at generation")
    records_seen = 0
    for line in out.splitlines():
        if "record(s), generation" in line:
            records_seen += int(line.split(" byte(s), ")[1].split(" record(s)")[0])
    check("baseline recovers as consistent", code == 0 and "verdict: consistent" in out,
          "exit={}\n{}".format(code, out))
    check("baseline inspected every segment and at least one record",
          segments_seen >= 2 and records_seen >= 2,
          "segments={} records={}\n{}".format(segments_seen, records_seen, out))

    # 2. a cross-segment generation discontinuity (the header carries no CRC, so only the rule finds it)
    gap_dir = os.path.join(work, "generation-gap")
    os.makedirs(gap_dir)
    gap = copy_store(source, gap_dir)
    old, new = bump_generation(gap, newest, 2)
    code, out = run_tool(gap)
    check("a cross-segment generation gap is named and refused",
          code == 1 and "verdict: NOT RECOVERABLE" in out
          and "segment {} starts at generation {}".format(newest, new) in out
          and "refusing to recover" in out,
          "segment {} generation {} -> {}; exit={}\n{}".format(newest, old, new, code, out))

    # 3. a corrupted payload byte: the CRC is the only thing that can see it
    crc_dir = os.path.join(work, "crc")
    os.makedirs(crc_dir)
    crc = copy_store(source, crc_dir)
    # the LAST record's payload: flipping there leaves every header intact
    tail_path = segment_path(crc, newest)
    size = os.path.getsize(tail_path)
    flip_payload_byte(crc, newest, size - 1)
    code, out = run_tool(crc)
    check("a corrupted record fails its CRC check",
          code == 1 and "fails its CRC check" in out and "refusing to recover" in out,
          "exit={}\n{}".format(code, out))

    # 4. a torn tail is a crash artefact and must NOT be reported as corruption
    torn_dir = os.path.join(work, "torn")
    os.makedirs(torn_dir)
    torn = copy_store(source, torn_dir)
    torn_path = segment_path(torn, newest)
    with open(torn_path, "r+b") as fh:
        fh.truncate(os.path.getsize(torn_path) - 7)
    code, out = run_tool(torn)
    check("a torn tail is tolerated, not called corruption",
          code == 0 and "verdict: consistent" in out and "crash artefact" in out,
          "exit={}\n{}".format(code, out))

    # 5. a missing newest segment
    lost_dir = os.path.join(work, "lost")
    os.makedirs(lost_dir)
    lost = copy_store(source, lost_dir)
    os.remove(segment_path(lost, newest))
    code, out = run_tool(lost)
    check("a missing newest segment is reported",
          code == 1 and "verdict: NOT RECOVERABLE" in out,
          "exit={}\n{}".format(code, out))

    # 6. nothing to inspect must not read as clean
    empty_dir = os.path.join(work, "empty")
    os.makedirs(os.path.join(empty_dir, "nothing"))
    code, out = run_tool(os.path.join(empty_dir, "nothing"))
    check("a store with no segment is not a clean run", code == 2, "exit={}\n{}".format(code, out))

    print("wal_inspect_selftest: {} check(s), {} failure(s)".format(checks, len(failures)))
    if failures:
        print("wal_inspect_selftest: fixtures kept at {}".format(work))
        print("wal_inspect_selftest: FAIL {}".format(", ".join(failures)))
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("wal_inspect_selftest: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
