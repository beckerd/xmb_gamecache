# Changelog

## 1.8 — cache file capped at a card-scaled budget

The cache could previously grow without bound on an ever-growing
library: only dead bytes counted against the old ~48 MB rebuild
trigger, so live data slipped past it. The budget now covers the
whole file — 0.5% of card capacity, clamped to 8–48 MB, derived from
the free-space snapshot already collected (never a fresh devctl).

- **Reclamation runs at list-build time, cheapest step first:** trim
  the append point back over trailing free space, evict bulky media
  (ICON1/PIC0/PIC1/SND0) from the least recently played games — every
  game keeps its title and icon — then compact survivors into a fresh
  file.
- **Compaction preserves built state** (nothing goes cold, no ISO is
  re-scanned) and is crash-safe by ordering: the index is deleted
  before the data-file swap, so an interruption at any point lands on
  the normal rebuild path. The old wipe-and-rebuild behavior remains
  only as its failure fallback.
- While over budget mid-session, reads stream from the ISO instead of
  growing the file.

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
