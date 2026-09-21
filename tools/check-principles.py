#!/usr/bin/env python3
"""Mechanical enforcement of this project's principles.

These rules used to live only in memory, which means they could be broken silently.  A rule is
enforced here only if violating it produces a loud failure.

Design notes (each one was learned by getting it wrong first):
  * COMMENTS ARE STRIPPED WITH A REAL SCANNER, not with grep.  A naive "skip lines starting with
    *" filter reported prose like "the worker inline" and a trailing "/* sync: run entry inline */"
    as violations; a check that fires on comments is a check people learn to ignore.
  * `long long` is NOT banned: the tree deliberately uses `unsigned long long` typedefs.  Whether
    that satisfies the MSVC 6.0 claim is a separate, open question (doc/gaps-audit.md).
  * ULL literals are forbidden in code/ (the shipped library) and allowed in tests/ and tools/,
    which are gcc-only and never compiled by MSVC.
  * KNOWN deficits use a BUDGET (ratchet): the site count may shrink, never grow.  Where a
    violation is already a backlog card, the card is named in the message.
  * Records under doc/measurements/ may contain CRLF - they are captured output, and reformatting
    archived evidence would destroy it.  Only the repository index and the scripts are enforced.
"""
import os, re, shutil, subprocess, sys

def find_git():
    """Resolve git to an ABSOLUTE path.  A bare 'git' in subprocess fails on Windows whenever the
    interpreter cannot resolve the MSYS-provided entry (CreateProcess error 2) - the same trap as
    spawning 'grep' from Python here.  A missing git is reported loudly, never skipped silently."""
    cand = [shutil.which('git'), shutil.which('git.exe'),
            'C:/Program Files/Git/cmd/git.exe', 'C:/Program Files/Git/bin/git.exe',
            'C:/Program Files/Git/mingw64/bin/git.exe', 'C:/Program Files (x86)/Git/cmd/git.exe']
    for c in cand:
        if c and os.path.isfile(c): return c
    return None

GIT = find_git()
GIT_LIST_PROBLEMS = []
GIT_FAILURES = []

def git_files(*args):
    """A file list derived from git.  An EMPTY list is not a passing state: every rule that consumes one of
    these silently becomes green when git is unavailable or when the pattern matches nothing (the review
    found that an empty list also made scan() fall back to the whole library, so the rule then inspected
    files it was never written for).  Record the emptiness so a rule can report it."""
    listing = (git_out('ls-files', *args) or '').split()
    if not listing:
        GIT_LIST_PROBLEMS.append('git ls-files %s matched no file' % ' '.join(args))
    return listing

def git_out(*args):
    """git with its return code CHECKED.  The old version returned stdout and dropped rc, so a failing git looked
    like an empty list - and an empty list is what makes a rule green for want of files (review 4th round E7)."""
    if not GIT: return None
    p = subprocess.run([GIT] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if p.returncode != 0:
        GIT_FAILURES.append('git %s -> rc=%d %s' % (' '.join(args), p.returncode, (p.stderr or '').strip()[:70]))
    return p.stdout

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
LIB = ['code/' + f for f in sorted(os.listdir('code')) if f.endswith(('.h', '.c'))]
SRC = LIB + ['tests/' + f for f in sorted(os.listdir('tests')) if f.endswith(('.h', '.c'))]

def strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":                      # string / char literal
            q = c; out.append(c); i += 1
            while i < n:
                out.append(text[i])
                if text[i] == '\\': 
                    i += 1
                    if i < n: out.append(text[i])
                elif text[i] == q:
                    i += 1; break
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i+1] == '*':   # block comment
            j = text.find('*/', i + 2)
            seg = text[i:] if j < 0 else text[i:j+2]
            i = n if j < 0 else j + 2
            out.append('\n' * seg.count('\n'))            # keep line numbering intact
            continue
        if c == '/' and i + 1 < n and text[i+1] == '/':   # line comment
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        out.append(c); i += 1
    return ''.join(out)

def is_char_literal(text, i):
    """True when the quote at text[i] opens a C CHARACTER literal.  A comment full of prose ("don't",
    "node's") used to be treated as the start of a literal that ran on until the next apostrophe - possibly
    lines later - and everything in between, including real // comments, disappeared from the rule's view
    (review 6.3: raft.h alone had 111 // occurrences invisible that way).  A char literal is a quote, then
    one character or an escape, then a closing quote (a wide/multi-char literal still starts that way)."""
    if i + 1 >= len(text):
        return False
    if text[i+1] == '\\':                       # '\n', '\x41', '\012', '\'' ...
        j = i + 2
        if j < len(text) and text[j] in 'xX':    # hex escape: \x41
            j += 1
            while j < len(text) and (text[j].isdigit() or text[j] in 'abcdefABCDEF'):
                j += 1
        elif j < len(text) and text[j].isdigit():  # octal escape: \0, \012
            j += 1
            while j < len(text) and text[j].isdigit():
                j += 1
        else:                                    # a simple escape is exactly one character: \n \t \\ \'
            j += 1
        return j < len(text) and text[j] == "'"
    return i + 2 < len(text) and text[i+2] == "'"


def strip_literals(text):
    """Blank out string and char literal CONTENTS (keeping length and newlines) without touching
    comments.  Used by the \"// comments\" rule: scanning comment-stripped text can never find a // comment,
    and scanning raw text would flag every \"disk://...\" string literal.  Stripping literals only is what
    makes that rule able to fail."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or (c == "'" and is_char_literal(text, i)):
            quote = c
            out.append(' ')
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\' and i + 1 < n:
                    out.append(' ')
                    i += 1
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            if i < n:
                out.append(' ')
                i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def code_text(path):
    with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh:
        return strip_comments(fh.read())

def scan_raw(pattern, files=None, allow=None):
    """Like scan(), but strips literals instead of comments - for rules that are ABOUT comments.  `allow(line, match)`
    returning True excuses a match: a rule about comments needs to be able to say "not this one" without weakening
    its pattern for everyone (the // rule and URI schemes)."""
    rx = re.compile(pattern)
    hits = []
    for path in (SRC if files is None else files):
        with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh:
            txt = strip_literals(fh.read())
        for ln, line in enumerate(txt.split('\n'), 1):
            for mt in rx.finditer(line):
                if allow and allow(line, mt):
                    continue
                hits.append('%s:%d: %s' % (path, ln, line.strip()[:110]))
                break
    return hits


def scan(pattern, files=None, label=None):
    rx = re.compile(pattern)
    hits = []
    for path in (SRC if files is None else files):
        for ln, line in enumerate(code_text(path).split('\n'), 1):
            if rx.search(line):
                hits.append('%s:%d: %s' % (path, ln, line.strip()[:110]))
    return hits

def sh_files():
    return git_files('*.sh')

fails = 0; rules = 0
def report(name, hits, detail=None):
    global fails, rules
    rules += 1
    if hits: fails += 1
    print('  %-4s %s' % ('FAIL' if hits else 'ok', name))
    for h in (hits or [])[:8]: print('        | ' + h if isinstance(h, str) else h)

print('== principles ==')
if not GIT:
    print('  FAIL  cannot locate git (absolute path); git-dependent rules cannot be judged')
    print('PRINCIPLES|FAIL|rules=0 fail=1')
    sys.exit(1)

# 1. line endings
eol = (git_out('ls-files', '--eol') or '').split('\n')
# doc/measurements/ is captured console output and .gitattributes marks it `-text` on purpose: those
# bytes ARE the observation (a few records legitimately hold CRLF, one holds bare CR).  The exemption
# is named in the rule itself so nobody has to infer it from the failing output.
report('repository stores LF (index CRLF = 0; doc/measurements/ excepted as captured evidence)',
       [l for l in eol if 'i/crlf' in l and 'doc/measurements/' not in l])
report('no CRLF in shell scripts', [l for l in eol if 'w/crlf' in l and l.split()[-1].endswith(('.sh', '.bash'))])

# 2. build/ purity
tracked = (git_out('ls-files', 'build/') or '').split()   # emptiness here is the GOOD state
report('build/ holds no tracked file (pure output)', tracked)

# 3. no machine-specific paths in tracked scripts (URI prefixes like disk:// excluded by the boundary)
bad = []
for f in sh_files():
    with open(f, 'r', encoding='utf-8', errors='replace', newline='') as fh:
        for ln, line in enumerate(fh.read().split('\n'), 1):
            s = line.strip()
            if s.startswith('#') or '://' in line: continue
            if re.search(r'(^|[^A-Za-z0-9_])[A-Za-z]:[\\/]', s): bad.append('%s:%d: %s' % (f, ln, s[:110]))
report('no drive-letter paths in tracked shell scripts', bad)

# 4. C89 / MSVC-6 language constraints
report('no <stdint.h>', scan(r'#\s*include\s*<stdint\.h>'))
report("no 'inline' keyword", scan(r'\binline\b'))
report('no ULL literals in the shipped library (code/)', scan(r'[0-9]+ULL', LIB))
# Scans text with LITERALS stripped (not comments stripped): a real // comment is a violation, a
# "disk://x" URI inside a string is not.  Previously this rule ran on comment-stripped text and therefore
# could never match anything - a permanently green rule.

def uri_scheme(line, mt):
    """True for the `//` of a URI scheme (`mem://`, `disk://`...), which is not a comment.  The old rule excluded
    every `://` with a `[^:]` guard, which had two demonstrable false negatives (fourth review round E4):
    `case 3://note` was invisible, and a `//` with nothing after it (`x=1;//` at end of file) was too.  A scheme
    starts with a LETTER, so `3://` is not one - the guard is narrowed instead of dropped.  Block comments keep
    their `mem://` prose: strip_literals() blanks string contents but not comments."""
    i = mt.end() - 2
    if i < 1 or line[i-1] != ':':
        return False
    k = i - 2
    while k >= 0 and (line[k].isalnum() or line[k] in '+.-'):
        k -= 1
    scheme = line[k+1:i-1]
    return len(scheme) > 0 and scheme[0].isalpha()


report('no // comments in C sources', scan_raw(r'(^|[^/])//', allow=uri_scheme))

# 5. Windows XP+ only
report('no post-XP Windows APIs', scan(r'GetQueuedCompletionStatusEx|GetTickCount64|CreateFile2|'
                                       r'GetFileInformationByHandleEx|SetFileInformationByHandle|'
                                       r'GetSystemTimePreciseAsFileTime|InitializeCriticalSectionEx|WSAPoll\s*\('))

# 6. no temporary instrumentation left behind
report('no temporary instrumentation tags', scan(r'\b(TEMP-(INSTR|AB|PROBE)|INSTR-[A-Z0-9]+|XXX-|HACK-)'))

# 6b. 64-bit types: every header that defines one must carry the MSVC branch, and 64-bit printf
# conversions must go through the header's format macro - a literal %lld is a C99 conversion that
# MSVC 6.0's runtime does not accept (see the comment in raft.h).
C64 = ['code/kbase.h', 'code/vfs.h', 'code/cemon.h', 'code/raft.h', 'code/treap.h']
problems = []
for path in C64:
    with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh: txt = fh.read()
    for need, why in (('_MSC_VER', 'no _MSC_VER branch'), ('__int64', 'no __int64 form'), ('long long', 'no long long form')):
        if need not in txt: problems.append('%s: %s' % (path, why))
report('every 64-bit header has __int64 and long long forms under _MSC_VER', problems)
report('no literal %lld/%llu in code/ (use the header format macro)', scan(r'%ll[du]', LIB + ['code/kdbctl.c', 'code/kdbsvr.c']))

# 7. layering ratchets
# The application must not read raft_ctx's configuration fields for its own policy (card t_972b67e8,
# issue #14/C6): raft.h reports the facts on the ready bundle and the application caches them.  The pattern
# targets raft-internal reads (`raft->config_*`), NOT the sanctioned `ready->config_joint` the application
# copies the fact from.  Budget is ZERO now: the three original sites are gone, and this may never grow.
sites = scan(r'raft->config_(new|joint|learners)', ['code/kserver.h', 'code/kdbsvr.c'])
report('app layer reads no raft config fields (budget 0, card t_972b67e8)', sites)
sites = scan(r'raft_inspect', ['code/kserver.h', 'code/kdbsvr.c', 'tests/raft_cluster_fuzz.c', 'tests/raft_test.c'])
# Name the sanctioned site too: the rule used to report a count, so a reader could not tell WHICH call is the
# one allowed (review 4th round E7).
report('raft_inspect only in the diagnostics path (budget 1): allowed=%s' % (sites[:1] or ['<none>']), sites[1:] or [])

# 8. unbounded formatting ratchet.  The INFO/STATS line used to be three unbounded sprintf calls that
# appended through a `text+len` pointer into a fixed buffer, so one more field could overflow it.  It is
# now built with k_text_append (bounded, always NUL-terminated, marks and counts truncation), and this
# rule stops new bare sprintf calls into fixed buffers from creeping back in.  The budget is the site
# count measured when the INFO/STATS line was converted; it may shrink, never grow.
sites = scan(r'(^|[^_a-zA-Z])sprintf\s*\(', LIB + ['code/kdbctl.c', 'code/kdbsvr.c'])   # (^|..): a bare
# sprintf at column 0 was invisible to the old pattern (review 4th round E7).
BUDGET_SPRINTF = 10   # measured with THIS checker on the tree that added the rule: 10 sites in code/
report('no new bare sprintf in code/ (measured %d, budget %d, use k_text_append/k_snprintf)'
       % (len(sites), BUDGET_SPRINTF), sites[BUDGET_SPRINTF:] or [])

# 9. C99 64-bit spellings in the test tree.  MSVC 6.0 - the declared toolchain - has no `long long`,
# no LL/ULL literals, no %llu and neither strtoull nor _strtoui64; MinGW-w64 accepts all of them, which
# is why this class reached a real compiler only when the XP guest was used.  Derived types must come
# locally, the way tests/raft_fuzz.c and tests/test.h each do for themselves; no shared header, because
# a test program that only needs the types would then carry the harness state and warn as unused.
# Budget is the site count measured with THIS checker when the rule was added: it may shrink as files
# are converted, never grow.
# 157 sites measured with THIS checker's own comment-stripping (a raw grep says 236 - it counts
# comments, and a budget taken from that number would leave ~80 sites of slack for new violations).
TESTS_C = git_files('tests/*.c', 'tests/*.h')
# The budget lives in ONE place and the message is derived from it: the old form kept the number in the
# message text and a different number in the slice, so the message walked 161->7 while the threshold stayed
# 162 and ~155 new violations passed silently.  The measured count is printed too, so a budget that no
# longer matches the tree is visible in the ok line instead of only on the day someone injects a test.
BUDGET_C99_TESTS = 7
c99_sites = scan(r'\blong long\b|(?:0[xX][0-9a-fA-F]+|[0-9]+)(?:ULL|ull|LL|ll)\b|%ll[du]', TESTS_C)
report('no new C99 64-bit spellings in tests/ (measured %d, budget %d = the sanctioned per-compiler blocks, use the project macros or a local layer)'
       % (len(c99_sites), BUDGET_C99_TESTS), c99_sites[BUDGET_C99_TESTS:] or [])

# 8. contract files: changing them must be deliberate
dirty = (git_out('status', '--porcelain', 'code/raft.h', 'code/treap.h') or '').strip()
last = git_out('diff', '--name-only', 'HEAD~1', 'HEAD') or ''
if dirty or re.search(r'^code/(raft|treap)\.h$', last, re.M):
    rules += 1
    print('  NOTE raft.h/treap.h changed: cite the semantic paragraph (doc/dissertation.md) in the message')
else:
    report('raft.h/treap.h untouched in this change', [])

# 9. every request type is accounted for in the places that must know it.
# A new K_REQ_* is a multi-file change (declaration, server dispatch/classification, the client's
# response handling, the CLI), and forgetting one file is SILENT: a response body that no branch prints
# is swallowed and the client answers a bare "ok".  This rule cannot guess which places a new command
# needs, so it forces the author to say so - a declared type that is not listed below is a failure, and
# every place listed for a type must really contain it.
REQ_FILES = {'kproto.h': 'code/kproto.h', 'kserver.h': 'code/kserver.h',
             'kclient.h': 'code/kclient.h', 'kdbctl.c': 'code/kdbctl.c'}
REQ_PLACES = {
    # response bodies: every place that must mention the type, frozen from the tree
    'K_REQ_GET':      ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_MGET':     ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_MSET':     ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_RSET':     ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_MDEL':     ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_RDEL':     ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_COUNT':    ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_MIN':      ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_MAX':      ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_CAS':      ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_FCALL':    ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_INFO':     ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_STATS':    ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_MEMBERS':  ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_HELP':     ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_SHUTDOWN': ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_TOPOLOGY': ['kproto.h', 'kserver.h', 'kclient.h', 'kdbctl.c'],
    'K_REQ_RGET':     ['kproto.h', 'kserver.h', 'kclient.h'],   # reached through a kclient helper
    'K_REQ_MEMBER':   ['kproto.h', 'kserver.h', 'kclient.h'],   # the CLI issues it via k_client_queue_member, and its
    'K_REQ_SET':      ['kproto.h', 'kserver.h', 'kdbctl.c'],    # response body is a bare status: no print branch
    'K_REQ_DEL':      ['kproto.h', 'kserver.h', 'kdbctl.c'],
}
def req_text(path):
    with open(path, 'r') as fh:
        return fh.read()
req_declared = set(re.findall(r'#define\s+(K_REQ_[A-Z0-9_]+)\s+[0-9]+u', req_text(REQ_FILES['kproto.h'])))
req_problems = ['declared in kproto.h but not accounted for here: %s (add it to REQ_PLACES and wire every'
                ' place it needs)' % c for c in sorted(req_declared - set(REQ_PLACES))]
req_problems += ['listed in REQ_PLACES but not declared in kproto.h: %s' % c
                 for c in sorted(set(REQ_PLACES) - req_declared)]
for cmd in sorted(REQ_PLACES):
    for place in REQ_PLACES[cmd]:
        if not re.search(r'\b' + cmd + r'\b', req_text(REQ_FILES[place])):
            req_problems.append('%s is expected in %s but does not appear there' % (cmd, REQ_FILES[place]))
report('every request type is accounted for in the files that must know it (%d types)' % len(REQ_PLACES),
       req_problems)

# A rule fed by a git-derived file list is only as good as that list.  `git ls-files tests/*.c` matching nothing
# (git missing, run from the wrong directory, a pattern typo) left every such rule green - including the C99
# budget one, whose "measured 0" would then read as a clean tree (fourth-round review E3).  The emptiness is
# recorded when the list is built; this is where it becomes a failure, and the count is printed so a list that
# shrank to near-nothing is visible in the ok line too.
report('every git-derived file list is non-empty, so no rule is green for want of a file to look at (tests/ has %d C/H files)' % len(TESTS_C), GIT_LIST_PROBLEMS)
report('every git command the rules rely on exited 0 (a failed git used to look like an empty list)', GIT_FAILURES)

print('PRINCIPLES|%s|rules=%d fail=%d' % ('OK' if not fails else 'FAIL', rules, fails))
sys.exit(1 if fails else 0)
