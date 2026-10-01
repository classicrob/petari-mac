# Native compilation status

Audited on 2026-09-28 against the working native-port branch. These are Clang syntax checks, not link or gameplay results.

| Source group | Passing | Checked |
| --- | ---: | ---: |
| Game C++ | 1605 | 1605 |
| JSystem / nw4r C++ | 200 | 200 |

The latest library audit includes the native resource additions and the
DSP-facing units. Native mailbox handles now carry host addresses without
truncating them. These counts describe that audit snapshot; subsequent additions
need their own build validation.

The complete arm64 `petari` application and `petari_link_check` now link. The
application reaches original game initialization, where runtime debugging is
ongoing; title-screen and gameplay behavior have not yet been verified. Resource,
platform, and focused Metal tests provide separate evidence, summarized in
[README.md](README.md) and the subsystem reports.
