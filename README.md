# xmb_gamecache — fast XMB Game menu for ARK-4 on big memory cards

https://github.com/user-attachments/assets/0ac02283-8778-44a1-bbc5-bf59080ceb3e

The load time to show items in my 128GB memory card was so frustrating!
This plugin caches the data that slowed down you accessing your Game > Memory card.

This plugin also re-arranges items based on "last used" so most recently accessed
items in your memory card are first.

Written by Fable 5, reviewed by me. Tested on my PSP1000 with ARK4. 
Use at your own risk of course. I can't guarantee this won't conflict with other
plugins (this is my only plugin).


A VSH kernel plugin for PSPs running **ARK-4 CFW** that makes the XMB
Game menu load near-instantly, no matter how many ISOs are on the
card — including after waking from sleep.

## The problems it solves

ARK-4 shows your ISOs in the XMB by presenting each one as a fake game
folder. Every time the PSP boots back to the XMB (which happens every
time you exit a game), ARK re-opens every ISO on the card and extracts
its title, PARAM.SFO and icon again. ARK does have a built-in cache
(`ISOCACHE.BIN`) but it is hard-capped at **32 games**, and even cached
games get their icons re-read out of the ISO every session. With a
128 GB card full of games, that's a long grind on every single reboot.

Separately, every time the XMB shows the Memory Stick's free space it
re-counts every free cluster on the card from scratch — the firmware
caches nothing — which stalls the UI for several seconds on a big
card, most visibly after waking from sleep.

## What it does

- **Persistent game cache.** Each game's synthesized metadata and icon
  bytes are kept in `ms0:/PSP/SYSTEM/XMBGC.BIN` for an **unlimited**
  number of ISOs. On a warm boot the game list is validated purely
  from the directory listing (file size + timestamp — no per-game file
  opens) and served straight from the cache.
- **RAM mirror for sleep/wake.** Game titles and icons are additionally
  mirrored in RAM (up to 2 MB) during the first Game-menu visit of a
  session. RAM survives sleep, so re-opening the game list after
  waking needs almost no card IO and appears in about a second.
- **Instant free-space display.** The slow free-cluster count runs
  once per boot (folded into the boot you already wait for); every
  later query — including after sleep — is answered instantly from
  that snapshot. Safety: once free space drops below 1 GB the real
  count is always used, so a nearly-full card is never misreported.
  (Trade-off: deleting/adding games mid-session can leave the figure
  slightly stale until the next boot — and any game launch reboots
  the XMB, so it corrects itself in normal use.)
- **Recently-played-first ordering.** Launches (ISO, homebrew, and
  PS1/POPS) are recorded, and recently played games sort to the top
  of the Game menu. This works by serving fake "newest" folder dates
  to the XMB, so it only applies when the XMB sorts by date (the
  default for the Memory Stick game list).
- Adding/removing/replacing ISOs is detected automatically; only new
  or changed ISOs are re-scanned.
- Backgrounds (PIC1), animated icons (ICON1) and sounds (SND0) are
  cached the first time the XMB actually shows them.
- First Game-menu visit after install: about as slow as stock (it
  builds the cache once). Every visit after that: near-instant.

## Install

1. Copy `xmb_gamecache.prx` to `ms0:/SEPLUGINS/`.
2. Add this line to `ms0:/SEPLUGINS/PLUGINS.TXT` (create it if needed):

   ```
   vsh, ms0:/SEPLUGINS/xmb_gamecache.prx, on
   ```

3. Reboot the PSP. Open the Game menu once to build the cache (this
   first visit is slow — let it finish). After that, enjoy.

On a PSP Go using internal storage, use `ef0:/SEPLUGINS/` and the
`ef0:` path in the line above; the cache goes to
`ef0:/PSP/SYSTEM/XMBGC.BIN` automatically.

Optional extra speed: also enable ARK's Memory Stick cache by adding
`vsh, mscache, on` to the same PLUGINS.TXT.

## Tested on

- PSP-1000 (32 MB), ARK-4, 128 GB Memory Stick (SD adapter), `ms0:`.

That is the honest extent of hardware testing so far. The plugin is
designed to fail safe everywhere else — on any other CFW, any
ARK-4 version with different internals, or any resolution failure it
simply goes inert and you get stock behavior — but PSP Go internal
storage (`ef0:`), other PSP models, and other ARK-4 releases are
**unverified**. Reports (good and bad) are very welcome — please open
an issue.

## If something goes wrong

- The plugin is designed to fail safe: on any version mismatch or
  error it does nothing and you get stock (slow) behavior.
- If the XMB ever misbehaves, boot into ARK's **Recovery Menu**
  (hold R while powering on), go to Plugins, and disable it — or just
  delete the line from PLUGINS.TXT over USB.
- To force a full cache rebuild, delete `ms0:/PSP/SYSTEM/XMBGC.BIN`.
- To reset the recently-played ordering, delete
  `ms0:/PSP/SYSTEM/XMBMRU.BIN`.
- The cache file is capped in absolute size, scaled to the card it
  lives on: 0.5% of capacity, clamped to 8–48 MB — about 10 MB on a
  2 GB card, 20 MB on 4 GB, and the full 48 MB on anything 10 GB or
  larger. The cap covers the whole file, not just its dead space, so a
  library that is only ever added to cannot grow it without limit.

## Staying inside the cap

Roughly 30 KB per game goes on the metadata and icon that make the
list itself instant; backgrounds, animated icons and menu music are
much larger and are what actually fill the budget. When the cache
reaches its cap, three things happen in order, cheapest first:

1. **Trim** — the append point rolls back over any free space at the
   end of the file, so adding a game and removing it again costs
   nothing.
2. **Evict** — the bulky media (ICON1/PIC0/PIC1/SND0) is dropped from
   the least recently played games first. Every game keeps its title
   and icon, so the list stays instant; a dropped background re-caches
   by itself the next time you actually look at that game.
3. **Compact** — the surviving data is rewritten into a fresh file,
   which is what physically shrinks it on the card.

Compaction is incremental: games keep their built state, so nothing
goes cold and no ISO is re-scanned. Only if compaction fails outright
(no room for the scratch file) does the plugin fall back to discarding
the cache and rebuilding it.

## Known limitations

- **Game Categories Lite:** if that plugin is loaded, xmb_gamecache
  automatically steps aside (stock speed) so categories keep working.
- ISOs with file names longer than ~110 characters are skipped.
- The 1.50-addon (`GAME150`) listing and everything else (PSN eboots,
  homebrew folders, videos, savedata) passes through to ARK untouched.
- The free-space figure is a per-boot snapshot (see above); below 1 GB
  free it is always the real count.
- Sorting the Game menu by anything other than date bypasses the
  recently-played ordering (by design — only the dates are faked).
- ARK-4 only. On any other CFW the plugin stays inert.

## Building from source

Requires the [pspdev toolchain](https://github.com/pspdev/pspdev):

```
export PSPDEV=~/pspdev
export PATH=$PSPDEV/bin:$PATH
make
```

## How it works (short version)

At boot the plugin finds VshCtrl's exported XMB IO handlers and patches
the syscall table entries pointing at them, so its wrappers run first.
During a Game-menu directory open it feeds VshCtrl a fake, empty `/ISO`
directory (import hooks on VshCtrl's kernel IO) so ARK's slow per-ISO
parser never runs, enumerates `/ISO` itself, and serves the game list
from the cache. Launching, deleting, and game updates (PBOOT/DLC/
manuals) are handled by replicating ARK's own logic on top of its
exported helpers (`isoOpen`/`isoRead`, `sctrlSE*` launch APIs). The
free-space devctl (`0x02425818`) is intercepted at the user syscall
table, so kernel callers always see the real count. PS1/POPS launches
are caught by trampolining SystemControl's central LoadExec function.

## Disclaimer

This software is provided **as is, without warranty of any kind**, and
the author accepts no liability for any damage arising from its use —
see sections 15 and 16 of the [LICENSE](LICENSE). Use it at your own
risk.

Just in case: an XMB freeze is recovered by holding the power switch
off and disabling the plugin via ARK's Recovery Menu (hold R at
power-on) or by deleting its line from PLUGINS.TXT over USB.

## Credits & license

GPL-3.0-or-later. Portions derived from
[ARK-4 / PRO CFW](https://github.com/PSP-Archive/ARK-4) — thanks to
the ARK team, whose exported kernel APIs make plugins like this
possible.
