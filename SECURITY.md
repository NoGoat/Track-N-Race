# Security Policy

## Supported Versions

Only the latest release gets security fixes. If you're on an older version, update first and check whether the issue is still there.

## Reporting a Vulnerability

Please **don't** report security issues in public issues, pull requests, or public Discord channels.

Report them privately through GitHub instead:

**[Report a vulnerability](https://github.com/NoGoat/Track-N-Race/security/advisories/new)**

This opens a private report that only you and I can see. You need a GitHub account to use it.

If you can't use GitHub, message me on [Discord](https://discord.gg/UMVRrN3RbR) and ask for a private way to send the details. Don't post the details themselves in a public channel.

### What to include

- which app (desktop or Android) and version
- what the issue is and what an attacker could do with it
- steps to reproduce, or a proof of concept
- any files needed to trigger it, such as a crafted `.tnrd` recording

### What to expect

Track N Race is a solo project, so I can't promise fixed response times. I'll acknowledge your report as soon as I can, keep you updated in the report, and credit you in the fix unless you'd rather stay anonymous.

## Scope

Things I'm especially interested in:

- crashes or code execution from crafted UDP telemetry packets or `.tnrd` files
- problems with desktop–Android pairing, such as connecting without approval or exposing data to other devices on the network
- the Electron app loading or running content it shouldn't
