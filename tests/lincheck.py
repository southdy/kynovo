#!/usr/bin/env python3
"""lincheck.py -- register linearizability checker (kynovo client-consistency oracle)

Closes the gap the convergence oracle (state_hash equality) leaves open:
convergence proves replicas agree, but NOT that the client-visible history of
operations is linearizable.  A deposed leader serving one stale read, or a
read returning a value that was never committed, is invisible to a "all nodes
converged" check -- and visible to this one.

Model: a single register (one key).  An op is a 4-tuple (op, val, inv, resp):
  op   : 'r' (read) or 'w' (write); a DEL is a write of the absent value
  val  : for a write, the value written; for a read, the value returned
         (None == "not found" / absent)
  inv  : logical invocation timestamp
  resp : logical response timestamp (resp >= inv)

linearizable(ops, init) returns True iff there exists a total order of the ops
such that (a) it respects real-time order (j precedes i whenever
j.resp < i.inv) and (b) it is a valid register history (each read returns the
value of the most recent preceding write, or init if none).

Algorithm: backtracking search over the real-time partial order.  Exponential
in the worst case, but histories here are short and the real-time constraints
prune hard.  Pure stdlib; self-tests run under `python3 lincheck.py`.
"""


def linearizable(ops, init):
    n = len(ops)

    # j must precede i in ANY linearization iff ops[j].resp < ops[i].inv.
    # Precompute a predecessor bitmask for cheap feasibility pruning.
    pred = [0] * n
    for i in range(n):
        m = 0
        for j in range(n):
            if j != i and ops[j][3] < ops[i][2]:
                m |= (1 << j)
        pred[i] = m

    used = 0

    def search(cur_val, count):
        nonlocal used
        if count == n:
            return True
        for i in range(n):
            if used & (1 << i):
                continue
            # every real-time predecessor of i must already be placed
            if pred[i] & ~used:
                continue
            op, val, _inv, _resp = ops[i]
            if op == 'w':
                used |= (1 << i)
                if search(val, count + 1):
                    return True
                used &= ~(1 << i)
            else:  # read: must equal the current register value
                if cur_val == val:
                    used |= (1 << i)
                    if search(cur_val, count + 1):
                        return True
                    used &= ~(1 << i)
        return False

    return search(init, 0)


# ---- history builder (the format the C harness will export) ---------------
def mk_read(val, inv, resp):
    return ('r', val, inv, resp)


def mk_write(val, inv, resp):
    return ('w', val, inv, resp)


def _self_test():
    failures = 0

    def expect(name, ops, init, want):
        nonlocal failures
        got = linearizable(ops, init)
        status = "ok" if got == want else "FAIL"
        if got != want:
            failures += 1
        print(f"  [{status}] {name}: got={got} want={want}")

    print("lincheck self-test:")

    # -- linearizable --
    # sequential write then read
    expect("seq write->read", [mk_write("v", 0, 1), mk_read("v", 2, 3)], None, True)
    # read before any write returns absent
    expect("read absent", [mk_read(None, 0, 1), mk_write("v", 2, 3)], None, True)
    # concurrent read/write: read may linearize before the write (returns absent)
    expect("concurrent read-old", [mk_write("v", 0, 5), mk_read(None, 1, 2)], None, True)
    # concurrent read/write: read may linearize after the write (returns new)
    expect("concurrent read-new", [mk_write("v", 0, 5), mk_read("v", 1, 2)], None, True)
    # two overlapping writes, read sees the later one
    expect("two writers", [mk_write(1, 0, 3), mk_write(2, 0, 3), mk_read(2, 4, 5)], None, True)
    # delete then read = absent
    expect("del->read", [mk_write(None, 0, 1), mk_read(None, 2, 3)], "v", True)

    # -- non-linearizable (each must be REJECTED) --
    # stale read: write fully completes, then read returns the OLD value
    expect("stale read", [mk_write("v", 0, 1), mk_read(None, 2, 3)], None, False)
    # phantom value: read returns a value that was never written
    expect("phantom value", [mk_write("a", 0, 1), mk_read("b", 2, 3)], None, False)
    # read-after-delete returns the deleted value
    expect("read after delete", [mk_write(None, 0, 1), mk_read("v", 2, 3)], "v", False)

    print(f"self-test: {'PASS' if failures == 0 else str(failures) + ' FAILURES'}")
    return failures


if __name__ == '__main__':
    import sys
    sys.exit(1 if _self_test() else 0)
