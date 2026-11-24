# Job Q notes

Could not compile here (server headers are not in this folder). With `SOLO_BELOW = 0` every new branch is skipped and the old camp, party, active, and balance behaviour is unchanged.

Copy `SOLO_BELOW` and `SOLO_ACTIVE` from `settings/bots.lua.example` into the live `settings/bots.lua` before testing. A missing key fails the same way as any other missing `bots.*` setting.

## What changed

**`settings/bots.lua.example`.** `SOLO_BELOW` (10) and `SOLO_ACTIVE` (true).

**`bots.h` `Bot`.** `preyDrySince`, `nextPreyMove`, and `preySpot` remember a solo bot's search for something to fight. Empty until a bot below `SOLO_BELOW` has gone a minute with nothing in hunt range.

**`bot_world.cpp`.** One table, `kStartingAreas`: West Ronfaure (100) and East Ronfaure (101), North Gustaberg (106) and South Gustaberg (107), West Sarutabaruta (115) and East Sarutabaruta (116). Nation 0, 1, 2 in that order.

- `inStartingArea` is those six zones.
- `otherStartingZone` is the nation's other zone. A nation outside 0..2 uses the pair the bot is standing in.
- `soloPlace` picks a spot for login. `findCamps(level)` limited to that nation's two zones (all six when the nation is unknown). If that list is empty, any `Normal` mob in those zones whose `m_minLevel` is at most the bot's level + 1. The spot is `spotNear` at 5–25 yalms, same ring as `loginNext`.

**`bot_module.cpp` `loginNext`.** A non-citizen below `SOLO_BELOW` is placed with `soloPlace` and `placeAt`, instead of the `CAMP_CHANCE` / pool-home roll. Nation comes from `chars.nation` before the character is loaded. One info line: `bots: <name> solos in <zone text> (level N)`. Citizens still take the earlier `placeCitizen` path and are not sent to a starting area.

**`bot_module.cpp` `formParties`.** Bots below `SOLO_BELOW` are not added to the zone list, so they are never the leader and never a member of a bot-only party. A real player's invite is still accepted in `think`. `SOLO_BELOW` 0 keeps every bot in the list.

**`bot_module.cpp` `tick`.** `SOLO_BELOW`, `SOLO_ACTIVE`, and `ACTIVE_EVERYWHERE` are read once, before the bot loop. A non-citizen, not in a party, below `SOLO_BELOW`, standing in a starting area, counts as `active` when `SOLO_ACTIVE` is true. A bot in a real player's party is active for the same reasons as before (`withPlayer`, or a real player in the zone).

**`bot_module.cpp` `balance`.** A bot below `SOLO_BELOW` is not logged into a player's zone, not traveled out, and not counted toward `BOTS_PER_PLAYER_ZONE`. `LEVEL_WINDOW` is 40, so the old balance pass would have pulled a level 4 out of Ronfaure on the next cycle. At `SOLO_BELOW` and above they are ordinary bots again. Nothing teleports them on the level-up itself.

**`bot_ai.cpp` `think` / `seekPrey`.** After `huntTarget` finds nothing, a solo bot below `SOLO_BELOW` in a starting area walks to another `findCamps` spot in the same zone. That happens only when `HUNT` is on, they are not resting and not fighting, nothing has been in the 30-yalm hunt for 60 seconds, and they have not started a new walk in the last 2 minutes. One info line when the spot is chosen, not on each tick of the walk: `bots: <name> moves on to find prey (level N)`. `Bot::home` moves with them so field wander does not pull them back to the login spot. If `findCamps` has nothing left in the zone, `travel` takes them to a `findCamps` spot in the nation's other starting zone, with the same log line. Reaching `SOLO_BELOW` clears that walk and leaves them where they are. A party with a real player clears it too, and never calls `seekPrey`.

## Assumptions

- Zone ids 100, 101, 106, 107, 115, and 116 are the six areas named in the job, and `xi::ZoneId` can hold them. `g_PZoneList`'s key converts to `xi::ZoneId`, which `findCamps` already relies on.
- `chars.nation` and `CCharEntity::profile.nation` use the same order as the nation names in `bot_chat.cpp`: 0 San d'Oria, 1 Bastok, 2 Windurst. Login reads `chars.nation` because the character is not loaded yet. A failed read, or a value outside 0..2, uses any of the six. While they are online, the cross-zone walk reads `profile.nation`.
- "Fits" at login is `findCamps` first (mob min level + 1 at least the bot's level, and max level at most the bot's level + 2). The fallback is a `Normal` spawn whose min level is at most the bot's level + 1, so the camp is not entirely above them. If that is also empty, they are left on their saved position and a warning is logged. They are not given a town spot by this path, and a saved town position is not cleared when no spawn can be read.
- The "moves on" walk uses `findCamps` only. An empty list in the current zone is "outgrown", and the other starting zone is tried the same way. The weaker login fallback is not used for that walk. If neither zone has a fitting camp, they stay put and look again in 2 minutes.
- Solo means not a citizen, `withPlayer` false, and no party. `SOLO_ACTIVE` does not apply outside the six starting zones, and does not apply in a party.
- The 60 seconds start on the first hunt tick with no target and restart when `huntTarget` returns a mob. Rest, a fight, and the idle return (no real player in the zone and `SOLO_ACTIVE` false) do not call `seekPrey`. The 2-minute cap is on choosing a new spot. Until then, `walkTo` runs each tick until they are within 5 yalms.
- `seekPrey` reads `SOLO_BELOW` when a bot is choosing whether to walk. The tick loop does not read it once per bot for the `active` flag.
- Placement uses the pool level `loginNext` already had (`entry->level`), before `catchUp`. If catch-up then raises them to `SOLO_BELOW` or past it, they stay in the starting area and the special behaviour ends.
- `placeAt` does not change the homepoint. A death can still send them to town. They are not traveled back, and in town they are not `SOLO_ACTIVE`. A real player in that town zone wakes them under the old rule.
- A bot below the level who leaves a starting area with a real player, and is then left there, stays there. They are not pulled home, and they are not hauled to some other player by `balance`.
- Citizens are untouched, including a citizen whose level is under `SOLO_BELOW`.
- The two new log lines cast the level to `int` so it prints as a number.

## Test plan

Merge the two settings into `settings/bots.lua`. Leave `SOLO_BELOW = 10` and `SOLO_ACTIVE = true`. Have a real player online, not necessarily in a starter zone, and let bots log in (restart the map, or wait out `LOGOUT_GRACE`).

Working:

- A bot under level 10 logs `bots: <name> solos in West Ronfaure (level N)` (or East Ronfaure, North/South Gustaberg, West/East Sarutabaruta). San d'Oria in a Ronfaure, Bastok in a Gustaberg, Windurst in a Sarutabaruta. No camp or town line for that login. They are not in a party.
- With no real player in that zone, they still hunt. After a minute with nothing within about 30 yalms, one line: `bots: <name> moves on to find prey (level N)`. Not a line per tick. Not twice inside 2 minutes. If the zone has nothing left near their level, the same line is followed by a zone change to the nation's other starter zone.
- They kill starter mobs. A level-up is `bots: <name> diary: reached level N` while they are still under 10. At 10 the solo lines stop. They stay in the zone they dinged in. No teleport on the level-up.
- No `bots: <name> is forming a party` where `<name>` is a bot under 10. No `bots: <name> heads to` for those names while they are under 10. Invite one yourself: they accept and follow, the same as any other bot.
- Citizens, if `CITIZENS` is on, still log into the city. No `solos in` line for them.

`SOLO_BELOW = 0` and a restart: logins go to a camp or the pool home as before, low bots can form bot parties, and a zone with no real player goes idle again. No `solos in` and no `moves on to find prey`.
