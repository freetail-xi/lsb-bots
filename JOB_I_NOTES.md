# Job I notes

Could not compile here (server headers are not in this folder). Gear scoring uses modifiers and item calls that this module already uses. The exp grant calls two LandSandBoat functions that do not appear anywhere else in this tree. Both are named below, with the signature taken from current LandSandBoat `base` (`src/map/utils/charutils.h` / `charutils.cpp`).

The test server reads `settings/bots.lua`, not the example. Copy `CATCH_UP` into that file before testing. It is read with `settings::get<bool>`.

## Setting

In `settings/bots.lua.example`:

- `bots.CATCH_UP` (default true). On login, a bot who has been away at least an hour can gain exp. `false` turns that grant off. Logout still writes `last_seen` either way.

## Schema

`ensureSchema` (`bot_pool.cpp`) adds two columns the same way `diary` was added: on the `CREATE TABLE` and with `ADD COLUMN IF NOT EXISTS`.

| Column | Type | Meaning |
| --- | --- | --- |
| `last_seen` | `int(10) unsigned NULL` | Unix seconds. NULL until the first logout. |
| `play_style` | `tinyint(3) unsigned NULL` | 0 casual, 1 regular, 2 hardcore. NULL until the first catch-up that actually pays. |

`last_seen` is placed after `diary`. `play_style` is placed after `last_seen`.

## 1. Gear that matches the job

**`bot_gear.cpp` `jobBias`, `armourScore`, `planGear`.**

`usable` is unchanged: item level 0, no GM gear (`GmOnly`), required level, job bit, race. Nothing with item level, and nothing past that filter, was added.

`armourScore` is the old score (required level × 1000, plus DEF × 10, HP, and MP) plus `jobBias`. The bias is built only from positive modifier values, so it cannot push a score below the old score. Every piece the old loop would consider is still considered. A slot that had a winner still has one. Ties keep the first piece, same as `score > best` did before.

Bias weights:

- Every job: ACC × 25, RACC × 25, and HASTE_GEAR × 1.
- WHM, RDM, BRD, SMN: MND × 50, CHR × 50, CURE_POTENCY × 80, HEALING × 15, MACC × 25.
- BLM: INT × 50, MACC × 25.
- WAR, PLD, DRK: STR × 50, VIT × 50, and another DEF × 6 on top of the old DEF term.

Required level still leads. One required level is 1000 points, and one favoured stat point is 50, so the bias moves the choice among pieces about one required level apart. A much lower-level piece does not replace a current one. At the same required level, a white mage's MND piece beats a DEF/HP piece, a black mage's INT piece beats a raw MP piece, and a tank's STR/VIT/DEF piece beats a softer one.

The main-hand and ranged picks still prefer the job's skill order from `preferredSkills` / `rangedSkills`. Inside that skill, the score is damage per delay plus the same bias. A warrior still takes a great axe ahead of an axe. The bias only picks which great axe.

Left as they were:

- Bard instruments (song bonus, ranged slot).
- Ammo choice (damage only) and `bot.ammoId`, which `restock` still refills.
- Beastmaster jugs and the puppetmaster animator / oil override in `prepare`.
- `SLOT_SUB` cleared for a two-handed weapon, and when the main hand is empty. Sub is still shields only.

Other jobs (THF, NIN, SAM, RNG, and the rest, including SCH) keep the old score plus accuracy and haste only.

### Assumptions

- `xi::Mod::ACC`, `RACC`, `MACC`, `HASTE_GEAR`, `MND`, `CHR`, `INT`, `STR`, `VIT`, `DEF`, `CURE_POTENCY`, and `HEALING` are the generated names from LandSandBoat `data/enums/mod.yaml` (keys `acc`, `racc`, `macc`, `haste_gear`, and so on). This module already calls `getModifier` with `xi::Mod::DEF` and the song mods on the same item templates.
- `HASTE_GEAR` is on a 10000 scale (375 means 3.75%). Weight 1 keeps a few percent of haste in the same range as a handful of stat points, under one required level.
- `getModifier` on the item from `xi::items::lookup` returns the item's own mods. `armourScore` already relied on that for DEF, HP, and MP.

### How to test

Rebuild and restart the map. `!bot` does not show gear. Look at the character.

1. One bot of each starting job, plus BRD, NIN, RNG, and PLD, at 20 and at 75. Each slot that had a piece still has a piece. No Judge's gear, no item-level piece.
2. WHM, RDM, BRD, SMN: where two wearable pieces share a required level, the one with MND, CHR, healing skill, or cure potency is the one equipped. BLM: INT ahead of a same-level piece that is only MP. WAR, PLD, DRK: the heavier STR/VIT piece ahead of a same-level soft piece.
3. BRD ranged slot is still an instrument. A two-handed job (SAM, DRK, DRG, WAR with a great axe) has an empty sub slot.
4. RNG (or anyone with a bow): shoot the ammo stack out. Within about 20 seconds `restock` puts `bot.ammoId` back in the ammo slot.

## 2. Catch-up levels while offline

**`bot_pool.cpp` `noteLastSeen`, `catchUp`.** **`bot_module.cpp` `login`, `logout`.**

`logout` writes `last_seen` = now for every logout, including when `CATCH_UP` is false.

`login` calls `catchUp` only after `enterZone` has returned true. That is after `LoadChar`, after the character is in a zone, and after the first `prepare`. A zone change uses `enterZone` and does not call `catchUp`.

`catchUp` grants nothing when any of these is true:

- `CATCH_UP` is false
- `last_seen` is NULL
- the stamp is in the future
- the gap is under 3600 seconds
- `GetExpNEXTLevel` for the current level is 0 (the stamp is left alone, so a later login can try again)

Otherwise it rolls `play_style` if the column is NULL (0, 1, or 2, once, stored with `play_style IS NULL` so a second writer will not replace it) and grants exp. It then writes `last_seen` = now, before the diary line, so a retry of this login cannot pay the same gap. A second login a few minutes later sees a gap under an hour.

The pace, at level 20: casual 1 level per real day, regular 2, hardcore 4. At level L above 20 the pace is that number times `20/L`. Below 20 it stays at the level-20 pace. Each slice of time at a level is turned into exp with `GetExpNEXTLevel` for that level, and the walk moves up a level when the slice completes one, so a long gap is priced at each level it crosses. Hours are `seconds/3600` (a 90 minute gap is 1.5 hours). Time is capped at 3 days (259200 seconds). The grant stops at level 75, and it also stops when a call fails to raise `GetMLevel` (the usual reason is genkai).

One diary line, only when the main level actually rose: `gained 1 level while away, now WAR 24` or `gained 4 levels while away, now WAR 24`. `catchUp` sets `preparedLevel` to the new level first, then `login` calls `prepare` again so spells, subjob, and gear match. That second `prepare` does not also write `reached level`. A later ding from a fight still writes `reached level` the way it did before.

The map line is `bots: <name> was away <hours> hours (<style>), catch-up <exp> exp, now level <n>`.

`login` also copies the new main level onto the in-memory pool entry, which is what the next placement reads. Logout already did that for party levels.

### Assumptions

- **`charutils::AddExperiencePoints(bool expFromRaise, bool awardRegionPoints, bool fromScripts, CCharEntity* PChar, CBaseEntity* PMob, uint32 exp, ...)`** — current LandSandBoat `base`. This module never granted exp. The call is `AddExperiencePoints(PChar->isDead(), false, true, PChar, PChar, grant)`.
  - `expFromRaise` is the dead flag. In that function a dead character is skipped unless the flag is set, and a living character still gets the level-up HP/MP fill when the flag is clear.
  - `awardRegionPoints` is false. This is not a kill, so no conquest points.
  - `fromScripts` is true. That path does not multiply by `map.EXP_RATE`. The levels-per-day figures would scale with the server rate if this flag were false. Combat exp is untouched.
  - The mob argument is the character. There is no mob. The script binding does the same with the player pointer.
  - The last three parameters stay at their defaults (`EMobDifficulty::TooWeak`, no chain, limit points allowed). The loop never calls in at level 75, so a capped 75 does not take merit points from this grant.
- **One level per call.** On current LandSandBoat the function adds the exp, gains at most one level, caps leftover at the next bar minus one, and returns. `catchUp` therefore calls it once per level and never passes more than `GetExpNEXTLevel` of the level it is on. If a fork applies every level inside one call, this loop would grant too much.
- **`charutils::GetExpNEXTLevel(uint8)`** returns the exp required to leave that level (the `experience().NextLevel` table). The catch-up pace is converted to exp with that number, so it follows whatever chart the server loaded. A 0 return at the starting level skips the grant and does not move `last_seen`.
- The function saves the result itself (`SaveCharExp`, and on a ding `SaveCharStats` / `SaveCharJob` as well). `loadPool` reads `char_stats.mlvl` after a restart.
- Genkai is enforced inside `AddExperiencePoints`. Catch-up will not pass it, and will not pass 75 even when genkai is higher. A bot created at a low level keeps whatever genkai `char_jobs` already has (often 50 until a limit break). That is the same ceiling party exp already uses.
- `xirand::GetRandomNumber<uint8>(3)` returns 0, 1, or 2. The other call sites in this module use the same one-argument form.
- `db::preparedStmt` `get<uint32>`, `get<uint8>`, and `isNull` work on the new columns the way they work on `charid` and `diary`.
- `ADD COLUMN IF NOT EXISTS` is the syntax `diary` already uses.
- A stored `play_style` outside 0–2 is treated as regular for the rate.
- Time online after the login stamp is not paid on the next login. Logout writes `last_seen` again, so the next gap starts at logout. Standing in a party does not grant catch-up; the next login pays only the time after the previous stamp. A crash after the login stamp cannot pay the same gap twice.
- A brand-new column is NULL, so the first login after the alter pays nothing until a logout has written `last_seen` (or the column is set by hand for a test).
- `play_style` is rolled on the first paying login, not on logout and not when `last_seen` is NULL.
- Subjob level was already handled in `subJobScript` (no sub under `main.SUBJOB_QUEST_LEVEL`, then half the main via `setsLevel`). The second `prepare` runs that again at the new level. That function was not changed.

### How to test

1. Restart the map so `ensureSchema` runs. `SHOW COLUMNS FROM bot_pool LIKE 'last_seen';` and the same for `play_style`. Both nullable. An existing table gains them without a new database.
2. Put `CATCH_UP = true` in `settings/bots.lua` and restart again if that file was missing the key.
3. Pick a living bot under 75. Note the job and main level (`!bots` or the character).
   `UPDATE bot_pool SET last_seen = UNIX_TIMESTAMP() - 86400, play_style = 1 WHERE name = '<name>';`
   Regular, one day. At level 20 that is about 2 levels. At level 40 it is about 1 (`2 * 20/40`). Log the bot in with a real player online. The map should show one `was away` line and, when the level rose, one diary line `gained N levels while away, now WAR <level>`. Gear and spells should match the new level (a second prepare, no extra `reached level` line).
4. Log that bot out and back in within a few minutes. No second grant, and no second diary line. `last_seen` is recent (`SELECT last_seen, FROM_UNIXTIME(last_seen), play_style FROM bot_pool WHERE name = '<name>';`).
5. `UPDATE bot_pool SET last_seen = NULL WHERE name = '<name>';` and log in. No grant. `play_style` stays whatever it already was.
6. Set `last_seen` to 30 minutes ago. No grant, and `last_seen` stays at that value.
7. Set `last_seen` ten days ago and `play_style = 2` on a level-20 bot. The grant matches 3 days, not 10: on the order of 9 or 10 levels, not 40, and the level stops at 75 if it would pass it. The map line's hour count is 72.0.
8. A level-75 bot with `last_seen` a day ago: the map line shows `catch-up 0 exp`, no diary line, and `last_seen` moves to now.
9. Set `CATCH_UP = false`, restart, set `last_seen` a day ago, log in. No grant. Log out. `last_seen` moves to the logout.
10. Party: set `last_seen` an hour ago, log the bot in, stay in a party for a while. No further catch-up while they are online. After they log out and log in again an hour later, the new grant is that offline gap, not the time they stood in the party.
11. Set `play_style` NULL and `last_seen` a day ago. After login the column is 0, 1, or 2 and the map line names that style. A later login keeps the same value.
12. A bot whose genkai is 50 and whose level is 48 should stop at 50 even if the gap would pay more.
