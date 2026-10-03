# Contributing to EVONIX

Use the matching enforcing branch from
[BRANCHES.md](Documentation/evonix/BRANCHES.md).
Shared fixes should be checked on all four variants without enabling root in
Normal builds or changing the permissive branch incidentally.

## Patch requirements

1. Keep changes focused, explain their runtime impact and preserve published history.
2. Use kernel-style commit subjects and certify the DCO with `git commit -s`.
3. Do not commit generated output, flashable ZIPs, logs, credentials or signing keys.
4. Document changed interfaces, permissions, defaults and compatibility limitations.
5. Preserve upstream attribution and license notices.

## Validation

Run `git diff --check` and the relevant `scripts/checkpatch.pl` checks.
Build with the pinned workflow in
[BUILDING.md](Documentation/evonix/BUILDING.md), inspect the resulting
configuration and verify KSU versus Normal isolation.

Record exact build commits, checksums and test evidence. Runtime changes to
charging, scheduling, thermal behavior, storage or vendor interfaces require
targeted device testing. Compilation alone does not establish safety,
performance improvement or cross-ROM compatibility.

Never rewrite published branch or tag history. Submit a focused pull request
describing the problem, design, security trade-offs and exact validation.
