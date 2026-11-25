# Job T: make Geomancer and Rune Fencer bots play their jobs properly (2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server). The pool used to refuse GEO and RUN bots;
that check is gone (commit `0114d9d`), so 26 of the 500 pool bots will now log in as GEO or RUN. Their code paths were
written but have never run. Audit them and fix what is wrong or missing.

Read first: `bot_combat.cpp` `upkeep` (the GEO block near 1370), `runes` (near 2108), `tank` (near 1748), `isTank`,
`isCaster` (near 2456), `jobAbilities`, `castBest`/`tryCast`/`tryAbility`; `bot_gear.cpp` (weapon skills near 90,
the default subjob pairing near 445, `subJobScript`); `bot_brain.cpp` and `bot_orders.cpp` role/tank preference
helpers; `bot_module.cpp` near 62 and 72. Check how the LSB scripts in the server name the spells, abilities and
status effects you use: only use IDs, enums and members that already appear in this code.

1. **Geomancer.**
   - Indi- and Geo- picks: check each spell is one the bot can have at its level (the ladder should fall through to
     what it knows). Indi-Refresh / Geo-Refresh are worth it only with MP users in the party; a solo GEO should pick
     something useful for itself (Indi-Regen or Indi-Fury), not Refresh.
   - Luopan: `PPet` is assumed to be the luopan. Check that a GEO does not also try to use a pet path meant for
     BST/SMN/PUP/DRG (attack orders, pet heals, release). Full Circle only when the fight is over, as now.
   - A GEO solo with no party: make sure it still fights (club melee plus its nukes if the code has a GEO nuke list;
     if not, add the elemental nukes a GEO can cast, using the existing nuke helper).
   - GEO as a healer: if the party has no WHM/RDM/SCH, does anything cure? Default subjob is WHM, so allow the
     existing cure path to run on GEO/WHM only as a backup healer, like RDM.
2. **Rune Fencer.**
   - `runes` only ever casts Tellus. Pick the rune by the target's weakness if the code has element data for mobs,
     otherwise keep a fixed order but don't always use the same element. Keep it simple.
   - It tanks (`isTank` includes RUN). Check `tank` uses RUN's enmity tools: Flash/Foil if the bot has them via the
     existing spell paths, Swordplay, Vallation/Valiance, Pflug, and Battuta/Liement/One for All if those IDs exist in
     this code. Don't invent IDs.
   - Default subjob: `default: return WAR` gives RUN/WAR. That's fine; RUN/BLU or RUN/NIN only if trivial.
   - Gear: Great Sword. Check the gear picker can equip a 75-era great sword on RUN (job bit in the item's jobs
     mask) and falls back to something if none fits.
3. **Talk and names.** Job names and abbreviations already include Geomancer/GEO and Rune Fencer/RUN. Check any
   "what job are you / looking for party" chat and `!bot` commands handle them (e.g. a party seeking a tank counts
   RUN, seeking support counts GEO).

Rules: existing code style, no reformatting, no unrelated changes. You can't compile here, so only use functions,
types and members that already appear in this code or in LSB's headers as this code already uses them, and list
every assumption. Write `JOB_T_NOTES.md`: what changed and a test plan for the test server (log lines to look for:
a GEO casting Indi-/Geo- spells and Full Circle; a RUN using runes, Swordplay, Vallation and holding hate; no
errors). Edit only files in this folder. Read-only git (log, diff, show) is fine; don't commit, push, or change branches.
