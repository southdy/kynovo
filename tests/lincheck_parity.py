#!/usr/bin/env python3
"""lincheck_parity.py -- make the "the C checker agrees with the reference" claim machine-checked.

`tests/lincheck.h` is the checker the gate actually runs (inside `kserver_cluster_fuzz`), and its header
calls itself an in-harness port of `tests/lincheck.py`, whose `linearizable()` is the reference
implementation.  Until this script existed that agreement was prose - nothing executed the Python side at
all, so the two could drift apart and both still look right.

What it does:

  1. generates history families from a FIXED seed (a different sample every run would make a failure
     unreproducible): narrow random, wide random, and a family that is non-linearizable BY CONSTRUCTION,
     so the comparison cannot pass by accepting every history;
  2. hands them to build/lincheck_check.exe, which runs lincheck.h's memoized checker AND its plain
     backtracking oracle over each history;
  3. decides every history three way - the reference `linearizable()`, the C checker, and (for short
     histories) an exhaustive permutation search that shares no code with either - and compares.

One line is the verdict:

  lincheck parity: <N> histories accept=<a> reject=<r> wide=<w> py_mismatch=0 exhaustive_mismatch=0
                   oracle_mismatch=0 exhaustive=<m>

Exit status is 0 only when the run decided something: >0 histories, at least one accepted AND at least
one rejected history (a checker that answers everything the same way proves nothing), the wide
out-of-mask history answered INCONCLUSIVE, and zero mismatches.  "0 mismatches" must never be able to
mean "0 comparisons".

`wide` is the documented divergence, not a defect: lincheck.h packs the op mask in 64 bits and answers
LIN_INCONCLUSIVE above that, while the reference implementation has no limit.  The C answer must be
INCONCLUSIVE (-1) there - reporting an unchecked history as a violation would fabricate a safety bug.
"""

import itertools
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import lincheck  # the reference implementation under test

ABSENT = -1        # lincheck.h's LIN_ABSENT ("key absent")
LIN_MAX = 64       # lincheck.h's op-mask width
SEED = 20260921    # fixed on purpose: reproducible families, reproducible failure


def pyval(val):
    """lincheck.h spells "key absent" as -1; the reference implementation spells it None."""
    return None if val == ABSENT else val


def to_reference(ops):
    return [(kind, pyval(val), inv, resp) for (kind, val, inv, resp) in ops]


def exhaustive(ops, init):
    """Third opinion: every permutation, keeping those that respect real-time order and the register."""
    n = len(ops)
    for order in itertools.permutations(range(n)):
        ok = True
        for pos in range(n):
            for prev in range(pos):
                if ops[order[pos]][3] < ops[order[prev]][2]:   # this op answered before that one started
                    ok = False
                    break
            if not ok:
                break
        if not ok:
            continue
        cur = init
        for i in order:
            op, val, inv, resp = ops[i]
            if op == 'w':
                cur = val
            elif cur != val:
                ok = False
                break
        if ok:
            return True
    return False


def gen_random(rng, n):
    """Same shape as lincheck.h's randomized cross-check: values -1..8 so values congruent mod 4 collide."""
    init = rng.randint(-1, 3)
    ops = []
    for _ in range(n):
        inv = rng.randint(0, 8)
        resp = inv + rng.randint(0, 9 - inv)
        if rng.randint(0, 1):
            ops.append(('w', rng.randint(-1, 8), inv, resp))
        else:
            ops.append(('r', rng.randint(-1, 8), inv, resp))
    return ops, init


def gen_concurrent_valid(rng):
    """A serial history whose intervals are widened so neighbouring ops overlap.

    Linearizable by construction (the serial order still linearizes it), and the only family that puts
    CONCURRENT orders on the accept side: a checker that mishandles overlapping intervals - or one that
    rejects everything - cannot pass it.
    """
    vals = list(range(-1, 8))
    n = rng.randint(1, 7)
    cur = rng.choice(vals)
    ops = []
    for t in range(n):
        if t == 0 or rng.randint(0, 1):
            cur = rng.choice([v for v in vals if v != cur])
            ops.append(('w', cur, 2 * t, 2 * t + 2))
        else:
            ops.append(('r', cur, 2 * t, 2 * t + 2))
    return ops, ABSENT


def gen_corrupt_serial(rng):
    """A serial history (disjoint intervals, so its linearization is unique) with one read corrupted.

    The real-time order pins every read to the value the register held at its slot; changing that value
    therefore makes the history non-linearizable by construction - no reliance on luck for the reject side.
    """
    vals = list(range(-1, 8))
    n = rng.randint(2, 6)
    cur = rng.choice(vals)
    ops = [('w', cur, 0, 0)]
    for t in range(1, n):
        if rng.randint(0, 1):
            cur = rng.choice([v for v in vals if v != cur])
            ops.append(('w', cur, t, t))
        else:
            ops.append(('r', cur, t, t))
    reads = [i for i, o in enumerate(ops) if o[0] == 'r']
    if reads:
        i = rng.choice(reads)
    else:
        i = len(ops) - 1
        ops[i] = ('r', ops[i][1], ops[i][2], ops[i][3])
    op, val, inv, resp = ops[i]
    ops[i] = ('r', rng.choice([v for v in vals if v != val]), inv, resp)
    return ops, ABSENT


def gen_wide_serial(pairs):
    """A serial history longer than the op mask: the C checker must say INCONCLUSIVE, and must be fast."""
    ops = []
    for t in range(pairs):
        val = t % 7
        ops.append(('w', val, 2 * t, 2 * t))
        ops.append(('r', val, 2 * t + 1, 2 * t + 1))
    return ops, ABSENT


def write_histories(path, families):
    with open(path, 'w', newline='\n') as fh:
        for ops, init in families:
            fh.write('H %d %d\n' % (len(ops), init))
            for kind, val, inv, resp in ops:
                fh.write('O %s %d %d %d\n' % (kind, val, inv, resp))


def main(argv):
    if len(argv) != 3:
        sys.stderr.write('usage: lincheck_parity.py <lincheck_check-binary> <work-dir>\n')
        return 2
    binary, work = argv[1], argv[2]

    rng = random.Random(SEED)
    narrow, wide, valid, corrupt = [], [], [], []
    for _ in range(1000):
        narrow.append(gen_random(rng, rng.randint(1, 7)))
    for _ in range(100):
        wide.append(gen_random(rng, rng.randint(8, 9)))
    for _ in range(200):
        valid.append(gen_concurrent_valid(rng))
    for _ in range(200):
        corrupt.append(gen_corrupt_serial(rng))
    families = narrow + wide + valid + corrupt + [gen_wide_serial((LIN_MAX // 2) + 1)]

    if not os.path.isdir(work):
        os.makedirs(work)
    hist_path = os.path.join(work, 'lincheck-histories.txt')
    write_histories(hist_path, families)

    t0 = time.time()
    try:
        proc = subprocess.Popen([binary, hist_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                universal_newlines=True)
        out, err = proc.communicate()
        rc = proc.returncode
    except OSError as exc:
        sys.stderr.write('lincheck_parity: cannot run %s: %s\n' % (binary, exc))
        return 1
    elapsed = time.time() - t0

    verdicts = []
    read = None
    for line in out.splitlines():
        if line.startswith('V '):
            parts = line.split()
            if len(parts) != 3:
                sys.stderr.write('lincheck_parity: malformed verdict line %r\n' % line)
                return 1
            verdicts.append((int(parts[1]), int(parts[2])))
        elif line.startswith('read: '):
            read = int(line.split()[1])

    problems = []
    if rc != 0:
        problems.append('driver exit %d: %s' % (rc, ' / '.join(err.strip().splitlines()[:4])))
    if read != len(families) or len(verdicts) != len(families):
        problems.append('driver read %s and printed %d verdicts for %d histories'
                        % (read, len(verdicts), len(families)))
    if problems:
        for p in problems:
            sys.stderr.write('lincheck_parity: FAIL %s\n' % p)
        return 1

    accept = reject = wide_n = py_mis = exh_mis = ora_mis = exh_checked = 0
    for idx in range(len(families)):
        ops, init = families[idx]
        memo, oracle = verdicts[idx]
        if memo != oracle:
            ora_mis += 1
            problems.append('history %d (n=%d init=%d): memo=%d plain-oracle=%d' % (idx, len(ops), init, memo, oracle))
        ref = lincheck.linearizable(to_reference(ops), pyval(init))
        if ref:
            accept += 1
        else:
            reject += 1
        if len(ops) > LIN_MAX:
            wide_n += 1
            if memo != -1:
                problems.append('history %d (n=%d): the op mask cannot hold this history, so the answer must be'
                                ' INCONCLUSIVE (-1), got %d' % (idx, len(ops), memo))
            continue
        if memo != (1 if ref else 0):
            py_mis += 1
            problems.append('history %d (n=%d init=%d): C=%d reference=%d :: %r'
                            % (idx, len(ops), init, memo, 1 if ref else 0, ops))
        if len(ops) <= 6:
            exh_checked += 1
            if exhaustive(ops, init) != ref:
                exh_mis += 1
                problems.append('history %d (n=%d init=%d): exhaustive says %r, reference says %r :: %r'
                                % (idx, len(ops), init, not ref, ref, ops))

    if accept == 0:
        problems.append('no history was accepted - a checker that rejects everything is not being tested')
    if reject == 0:
        problems.append('no history was rejected - a checker that accepts everything is not being tested')
    if wide_n != 1:
        problems.append('expected exactly one out-of-mask history, saw %d' % wide_n)

    print('lincheck parity: %d histories accept=%d reject=%d wide=%d py_mismatch=%d exhaustive_mismatch=%d '
          'oracle_mismatch=%d exhaustive=%d families=narrow%d/wide%d/valid%d/corrupt%d/oversized%d driver_ms=%d'
          % (len(families), accept, reject, wide_n, py_mis, exh_mis, ora_mis, exh_checked,
             len(narrow), len(wide), len(valid), len(corrupt), 1, int(elapsed * 1000)))
    for p in problems:
        sys.stderr.write('lincheck_parity: FAIL %s\n' % p)
    if problems:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
