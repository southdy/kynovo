#!/usr/bin/env python3
"""Aggregate gcov .gcov reports across test drivers into per-file line+branch
coverage with uncovered-branch localization.  Pure stdlib, no third-party.

Usage:  python tools/cov_report.py build/cov/gcov [max_uncovered]

The build/cov/gcov/<driver>/ directory holds one .gcov set per driver (each
driver compiled with --coverage, run, then `gcov -b -c`).  Drivers that
#include the same single-header library (raft.h, kserver.h, ...) each emit a
<header>.gcov; this script takes the UNION across drivers: a line/branch is
"covered" if ANY driver executed it, matching how coverage is defined.

Line coverage  = covered executable lines / total executable lines.
Branch coverage = branches taken at least once / total branches (gcov's
"Taken at least once", the strict branch metric).  A branch is identified by
(owning source line, relative index within that line), which is stable across
drivers (the raw gcov arc number is per-compilation-unit and NOT comparable).
"""
import re
import sys
import os
import glob
import collections

SRC_RE = re.compile(r'^\s*(\S+)\s*:\s*(\d+):(.*)$')
# gcov reports a branch three ways: "taken N", "taken 0 (fallthrough)", and
# "never executed" (the owning line never ran).  The last form must be counted in
# the denominator too: ignoring it dropped every never-executed branch from both
# numerator and denominator and systematically OVERSTATED branch coverage
# (13.8k such branch lines in a full run).
BR_RE = re.compile(r'^branch\s+\d+\s+(?:taken\s+(\d+)|never executed)')


def parse_gcov(path):
    cov_lines = set()
    tot_lines = set()
    cov_br = set()
    tot_br = set()
    br_src = {}          # (line, rel_idx) -> owning source text (uncovered only)
    src_of_line = {}     # line -> source text
    cur_line = None
    cur_src = ''
    rel_idx = {}
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.rstrip('\n').rstrip('\r')
            m = SRC_RE.match(line)
            if m:
                count = m.group(1).rstrip('*')
                lineno = int(m.group(2))
                cur_line = lineno
                cur_src = m.group(3)
                rel_idx.setdefault(lineno, 0)
                src_of_line[lineno] = cur_src
                if count != '-':
                    tot_lines.add(lineno)
                    if not count.startswith('#'):
                        cov_lines.add(lineno)
                continue
            m = BR_RE.match(line)
            if m and cur_line is not None:
                rel = rel_idx[cur_line]
                rel_idx[cur_line] += 1
                tot_br.add((cur_line, rel))
                taken = m.group(1)
                if taken is not None and int(taken) > 0:
                    cov_br.add((cur_line, rel))
                else:
                    br_src[(cur_line, rel)] = cur_src
    return cov_lines, tot_lines, cov_br, tot_br, br_src, src_of_line


def aggregate(base_dir, basename):
    """Union the per-driver <basename>.gcov reports.  Returns dict or None."""
    cov_lines = set()
    tot_lines = set()
    cov_br = set()
    tot_br = set()
    br_src = {}
    src_of_line = {}
    found = 0
    for path in glob.glob(os.path.join(base_dir, '*', basename + '.gcov')):
        cl, tl, cb, tb, bs, sl = parse_gcov(path)
        cov_lines |= cl
        tot_lines |= tl
        cov_br |= cb
        tot_br |= tb
        br_src.update(bs)
        src_of_line.update(sl)
        found += 1
    if not found:
        return None
    return {
        'drivers': found,
        'cov_lines': cov_lines, 'tot_lines': tot_lines,
        'cov_br': cov_br, 'tot_br': tot_br,
        'br_src': br_src, 'src_of_line': src_of_line,
    }


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def report_file(base_dir, basename, max_uncovered):
    d = aggregate(base_dir, basename)
    if d is None:
        return
    lc, lt = len(d['cov_lines']), len(d['tot_lines'])
    bc, bt = len(d['cov_br']), len(d['tot_br'])
    print('%s  (union of %d driver%s)' % (basename, d['drivers'],
                                          '' if d['drivers'] == 1 else 's'))
    print('  lines   : %5.2f%%  (%d/%d)' % (pct(lc, lt), lc, lt))
    print('  branches: %5.2f%%  taken-at-least-once (%d/%d)'
          % (pct(bc, bt), bc, bt))
    uncovered = [(ln, rel) for (ln, rel) in d['tot_br']
                 if (ln, rel) not in d['cov_br']]
    uncovered.sort(key=lambda t: t[0])
    if uncovered and max_uncovered > 0:
        print('  uncovered branches (%d):' % len(uncovered))
        for ln, rel in uncovered[:max_uncovered]:
            src = d['br_src'].get((ln, rel), d['src_of_line'].get(ln, ''))
            src = src.strip()
            if len(src) > 78:
                src = src[:78] + '...'
            print('    %s:%d  %s' % (basename, ln, src))


def main():
    if len(sys.argv) < 2:
        sys.stderr.write('usage: cov_report.py <gcov_dir> [max_uncovered]\n')
        return 2
    base_dir = sys.argv[1]
    max_uncovered = int(sys.argv[2]) if len(sys.argv) > 2 else 15
    if not os.path.isdir(base_dir):
        sys.stderr.write('no such dir: %s\n' % base_dir)
        return 2
    # basenames present across any driver, sorted with the raft.h core first
    names = set()
    for path in glob.glob(os.path.join(base_dir, '*', '*.gcov')):
        names.add(os.path.basename(path)[:-5])  # strip .gcov
    order = ['raft.h', 'kserver.h', 'kbase.h', 'kproto.h', 'kclient.h',
             'runtime.h', 'treap.h', 'vfs.h', 'cemon.h', 'cli.h', 'kdb.c']
    ordered = [n for n in order if n in names]
    rest = sorted(n for n in names if n not in order)
    for basename in ordered + rest:
        report_file(base_dir, basename, max_uncovered)
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
