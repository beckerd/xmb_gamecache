# Changelog

## 1.7 — first public release

Everything below was developed and tested incrementally on real
hardware (PSP-1000, ARK-4, 128 GB card); this is the first version
published.

- **Persistent XMB game cache** — unlimited ISOs cached in
  `PSP/SYSTEM/XMBGC.BIN` (metadata + icons), validated per boot from
  the directory listing alone; lazy per-game build on first use;
  automatic detection of added/removed/changed ISOs; self-compacting.
- **Recently-played-first ordering** — ISO, homebrew, and PS1/POPS
  launches recorded in `PSP/SYSTEM/XMBMRU.BIN`; recent games surface
  to the top of the date-sorted game list.
- **Instant free-space display** — the slow whole-FAT free-cluster
  count runs once per boot; later queries are served from a snapshot.
  Below 1 GB free the real count is always used.
- **RAM mirror** — titles and icons mirrored in RAM (2 MB cap) so the
  game list reopens in about a second after waking from sleep.
- Game Categories Lite detected and deferred to; fails inert on
  non-ARK-4 firmware or version mismatches.
