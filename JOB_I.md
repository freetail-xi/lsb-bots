# Job I: gear that matches the job, and catch-up levels (DESIGN-review.md proposals 7 and 10)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Read `DESIGN-review.md`
(sections 1 and 3, proposals 7 and 10) and the code first. Jobs D to H and several fixes are already committed
(see `git log`), so build on what is there now: the diary (`noteEvent`, `bot_pool.diary`) and sessions exist.

1. **Gear that matches the job** (proposal 7), in `armourScore` and the weapon pick in `planGear` (`bot_gear.cpp`).
   Keep `usable` (item level 0, no GM gear, job and race). Add a job bias: MND/CHR and healing for WHM/RDM/BRD/SMN,
   INT for BLM, STR/VIT and DEF for WAR/PLD/DRK, accuracy and haste where those item mods exist. The bias only
   reorders choices; it must never leave a slot empty that the old code would have filled. No item level or
   post-2008 gear. Keep the bard instrument case, ammo restock (`bot.ammoId`), and clearing `SLOT_SUB` for
   two-handed weapons.
2. **Catch-up levels while offline** (proposal 10). New `bot_pool` columns `last_seen` (unix seconds, NULL) and
   `play_style` (tinyint NULL: casual, regular or hardcore, rolled once and stored), added in `ensureSchema` the
   same way `diary` was. On `logout`, write `last_seen`. On a successful login (after `enterZone` returns true and
   after the first `prepare`, never during `LoadChar`), if `last_seen` is at least an hour ago, grant exp for the
   offline hours times a per-level rate that depends on `play_style`, capped at 3 days of time and at level 75,
   through the server's own exp function (find how this code or LSB already adds exp). Keep it slow: roughly
   casual 1, regular 2, hardcore 4 levels per real day at level 20, fewer as levels rise. Grant once per login, then
   write `last_seen`, so a retry can't double it. Note one diary line when levels were gained:
   "gained N levels while away, now WAR 24". A NULL `last_seen` (never logged out) grants nothing.
   New setting `bots.CATCH_UP` (default true) turns it off.

Rules: existing code style, no reformatting, no unrelated changes. New settings go in
`settings/bots.lua.example` with comments. You can't compile here, so only use functions, types and members that
already appear in this code (or, for the exp grant, say exactly which LSB function you assumed and why), and list
every assumption in the notes. Write `JOB_I_NOTES.md`: what changed (file and function), the schema change, the
new setting, and a test plan for the test server. Edit only files in this folder.
