# Security policy

## Supported release branches

Security work targets the four enforcing HyperOS and ColorOS branches listed
in [BRANCHES.md](Documentation/evonix/BRANCHES.md). The permissive branch
is not part of V4.0 and is not covered by its validation.

## Security boundaries

- Release configurations enforce SELinux and disable its permissive boot option.
- Inherited vendor-module version and symbol-version mismatch relaxation remains.
  SELinux enforcement does not make that relaxation safe.
- ColorOS compatibility interfaces include broadly writable Android-facing nodes.
  Their handlers require input-validation and privilege-boundary review.
- KernelSU variants intentionally provide privileged functionality.
- Baseband-guard filters covered root-origin block-device writes. It is not a
  comprehensive defense against every ioctl, mapping or already-privileged attack.
  Boot, recovery and update-related partitions remain available by design.
- OEM charge-pause observations do not prove electrical battery isolation.

These compatibility choices reduce stock GKI security guarantees. Do not describe
the release as a hardened or universally compatible kernel.

## Reporting

Report unexpected privilege bypasses, memory-safety defects, information leaks
and unsafe interface interactions privately through a
[GitHub security advisory](https://github.com/NEESCHAL-3/EVONIX-Kernel/security/advisories/new).
Include the exact branch, commit, configuration, impact and reproduction steps.
Do not include credentials, signing keys, proprietary material or personal data.
There is no guaranteed response timeline; allow reasonable time before disclosure.
