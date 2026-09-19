#!/usr/bin/env python3
"""Create one issue per "## <title>" block in ops/backlog-issues.md, skipping titles that already
exist (open or closed), so the file can be re-posted safely after an edit.

Runs in the cloud with the workflow's GITHUB_TOKEN.  --dry-run prints the parsed titles only, which
is how the parser is verified locally (a dev machine has no token and no gh).
"""
import subprocess, sys

FILE = 'ops/backlog-issues.md'

def parse(path):
    issues, cur, buf = [], None, []
    for line in open(path, encoding='utf-8'):
        if line.startswith('## '):
            if cur: issues.append((cur, ''.join(buf).strip()))
            cur, buf = line[3:].strip(), []
        elif cur is not None:
            buf.append(line)
    if cur: issues.append((cur, ''.join(buf).strip()))
    return issues

def main():
    dry = '--dry-run' in sys.argv
    items = parse(FILE)
    if not items:
        print('OPS|FAIL|no "## title" blocks found in %s' % FILE); return 1
    print('OPS|parsed|%d issues' % len(items))
    if dry:
        for t, _ in items: print('OPS|would-create|%s' % t)
        print('OPS|post-backlog-as-issues|count=%d dry_run=1' % len(items)); return 0
    existing = subprocess.run(['gh', 'issue', 'list', '--state', 'all', '--limit', '200',
                               '--json', 'title', '--jq', '.[].title'],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True).stdout.split('\n')
    have = set(x.strip() for x in existing if x.strip())
    made = 0
    for title, body in items:
        if title in have:
            print('OPS|skip-existing|%s' % title); continue
        r = subprocess.run(['gh', 'issue', 'create', '--title', title, '--body', body],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        if r.returncode == 0:
            made += 1; print('OPS|created|%s' % title)
        else:
            print('OPS|FAIL|%s: %s' % (title, r.stderr.strip()[:200]))
    print('OPS|post-backlog-as-issues|count=%d created=%d' % (len(items), made))
    return 0

if __name__ == '__main__':
    sys.exit(main())
