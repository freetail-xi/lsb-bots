# Job Q: bots under level 10 level up solo in the starting areas (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Commits up to `f006b9c` are in.
Read `bot_module.cpp` (`loginNext` and its CAMP_CHANCE placement, the tick loop's `active` line, `formParties`,
balance), `bot_ai.cpp` (`think`, `huntTarget`, `fight`, `rest`, `wander`), `bot_world.cpp` (`findCamps`, `isField`,
`spotNear`, `placeAt`, `placeAtHome`) and `bot_citizen.cpp` (don't change citizens) first.

The owner wants every bot below level 10 to level up on its own, soloing in the starting areas the way new players
do. Bot-only parties of level 4-6 bots in town don't fit that.

Settings (add to `settings/bots.lua.example` with comments like the others):
- `SOLO_BELOW = 10`: bots below this level solo in a starting area. 0 turns this off, and then behaviour must be
  exactly as now.
- `SOLO_ACTIVE = true`: those soloing bots hunt even when no real player is in their zone. They are few, and this is
  how they gain levels while nobody is watching. false keeps today's rule (active only with a real player in the
  zone).

Starting areas: West Ronfaure (100) and East Ronfaure (101) for San d'Oria, North Gustaberg (106) and South
Gustaberg (107) for Bastok, West Sarutabaruta (115) and East Sarutabaruta (116) for Windurst. Keep them in one
small table.

Do this:

1. **Placement at login.** A non-citizen bot below `SOLO_BELOW` is placed in a starting area, never in town and never
   at a camp elsewhere:
   - Prefer its own nation's two zones. Use the nation from the DB or the character if the code can get it before
     login; otherwise any of the six.
   - Pick a spot from `findCamps(level)` limited to those zones. If none fits, use any `Normal` mob spawn point in
     those zones whose level is at most the bot's level + 1.
   - Use `placeAt` / `spotNear` like `loginNext` does. This replaces the CAMP_CHANCE / pool-home choice for these
     bots only.
2. **No bot-only parties below SOLO_BELOW.** `formParties` skips these bots. A real player can still invite one, and
   it accepts as today. A bot-only party is never formed with them.
3. **Active while soloing.** With `SOLO_ACTIVE` true, a solo bot below `SOLO_BELOW` in a starting-area zone counts
   as `active` in the tick loop, the same as with a real player in the zone. Read the settings once per tick loop,
   not once per bot.
4. **Keep moving to find prey.** `huntTarget` only looks 30 yalms out. When a soloing bot below `SOLO_BELOW` has
   found nothing to hunt for 60 s, and isn't resting or fighting:
   - Walk to another spot from `findCamps(level)` in the same zone, and log one info line:
     `bots: <name> moves on to find prey (level N)`.
   - At most once per 2 minutes per bot.
   - When the bot outgrows the zone (no mob in the zone fits its level), it may move to the other starting zone of
     its nation the same way, through `travel`.
5. **Growing up.** When a soloing bot reaches `SOLO_BELOW`, it stops being special: normal party forming and camp
   rules apply from then on. It stays where it is; don't teleport it.
6. **Logging.** One info line at placement: `bots: <name> solos in <zone text> (level N)`.

Rules: existing code style, no reformatting, no unrelated changes. Citizens are untouched. A bot in a real player's
party behaves exactly as today. You can't compile here, so only use functions, types and members that already
appear in this code, and list every assumption. Write `JOB_Q_NOTES.md`: what changed (file and function), and a
test plan for the test server, saying which log lines show it working: "solos in", "moves on to find prey", kills
and level-ups of bots below 10, and no "is forming a party" lines naming a bot below 10. Edit only files in this
folder.
