# Security Policy

## Reporting a vulnerability

Please report security issues privately, **not** as a public GitHub issue. Use
GitHub's [private vulnerability reporting](https://github.com/bnelair/mef3io/security/advisories/new)
(Security → Report a vulnerability), or contact the maintainers directly.

Useful details: affected version, platform, a minimal reproduction, and the
impact you observed. We aim to acknowledge reports within a week.

## Scope

mef3io parses untrusted, possibly-corrupted binary files (including from
network filesystems and foreign writers). Parsing must never crash the host
process or read out of bounds — it validates structure, CRCs, and header
counters and raises a typed error instead. Memory-safety issues in the read
path (segfaults, out-of-bounds reads, unbounded allocations from crafted
input) are in scope and treated as security bugs.

Note: MEF's built-in AES metadata encryption is a format feature for access
control, not a warranty of confidentiality against a determined attacker; see
the [encryption model](https://bnelair.github.io/mef3io/encryption_model/).

## Supported versions

Fixes land on the latest release line. Please upgrade to the newest version
before reporting.
