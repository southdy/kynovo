## What changed, and why

<!-- One or two sentences.  If the change follows a paragraph of the semantic baseline (Ongaro's
dissertation) or a gap in doc/gaps-audit.md, say which one. -->

## Gates run (paste the verdict lines, do not summarise them)

- [ ] `./build.sh regress quick` -> `REGRESS|quick|pass=7 fail=0 duration=...s`
- [ ] `./build.sh regress fuzz` (required when the change touches raft, the store, the transport or snapshots)
- [ ] The specific gate that used to reproduce the bug (when fixing one)

```
GATE|...
REGRESS|...
```

## Evidence

<!-- Log paths, verdict lines, exit codes.  "Should be fine" is not evidence: this repository has
three recorded instances of a conclusion drawn from a stale binary. -->

## Risk, rollback and follow-up

- Risk: <!-- e.g. touches raft contract semantics: needs two independent lines of evidence -->
- Rollback: <!-- revert this commit / the change is behind no flag -->
- `doc/gaps-audit.md` updated: <!-- yes: which entry, or n/a -->

## Checklist

- [ ] C89 / MSVC 6.0 compatible / Windows XP+ (no `inline`, VLAs, `stdint.h`, `long long` literals)
- [ ] LF only (no CRLF introduced - CI checks this)
- [ ] No hand-written file added under `build/`
- [ ] No temporary instrumentation left behind (grep for the tags you used)
