# Job O: bot-only parties camp and pull (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Commits up to `e9f7dfb` are in.
Read `bot_pull.cpp` (`autoCamp` near 1149, `setPuller`, `pickTarget` near 632, `unpullable`, `kPullNear`/`kPullLimit`
near 130, the pull end reasons near 1506-1522), `bot_ai.cpp` (the `autoCamp` call near 1508, the `returnCamp` /
`returnUntil` path near 847-880 and 1299-1330, `travel` near 956), `bot_world.cpp` (`findCamps`, `spotNear`,
`placeAt`) and `bot_module.cpp` (`formParties`, `loginNext` near 597) first.

A 500-bot test with `bots.ACTIVE_EVERYWHERE = true` formed 41 bot-only parties (32 of 2, 6 of 3, 1 each of 4, 5, 6).
Only 5 bots ever pulled, about 13 pulls in 15 minutes. Pull endings: "target gone" 32, "couldn't get to it in time" 13,
"back at camp" 10. Pulls ran 100-145 yalms from camp. Causes found:

- `autoCamp` refuses parties under 3 members, so every party of 2 never camps.
- `autoCamp` refuses City zones, and nothing moves a party formed in town out to a field. Many parties formed in
  zones 230-246.
- `pickTarget` falls back to `kPullLimit` (180). In crowded low-level zones a far target is usually killed by another
  party before the puller gets there ("target gone").
- Bots send actions while sitting or resting: the core logs "Invalid GP_CLI_COMMAND_ACTION ... Character in invalid
  animation state" (Chokuwochukowu 104 times, Xavier 46).

Fix these:

1. **Parties of 2 camp.** In `autoCamp`, accept a bot-only party of 2 or more. `toughestGap` already keeps a small
   party's targets easy.
2. **A bot-only party in town heads out to a camp.** When the leader of a bot-only party (`partyIsBots`, not
   `withPlayer`) is in a zone that `autoCamp` would refuse as not hunting, has no Pull, and isn't already heading
   somewhere (`returnUntil` unset):
   - Pick a camp for the party's level with `findCamps` (choose one at random among them).
   - Send the party there through the existing `returnCamp` / `returnUntil` / `travel` path, so the members follow.
   - Log one info line: `bots: <leader>'s party heads out to <zone text> (level N)`.
   - Try this at most once per 5 minutes per leader. If `findCamps` returns nothing, log that once and wait the
     same 5 minutes.
   - Today "arrived" means only "in the camp's zone". For this head-out case, the leader must also be within about
     30 yalms of the camp position before the trip ends. Otherwise `autoCamp` would set the camp at the zone line.
     Keep the after-a-wipe return path exactly as it is now.
   - Add a member to `Bot` in `bots.h` if you need one (comment it like the others).
3. **Shorter pulls, and no racing.**
   - Lower `kPullLimit` from 180 to 100.
   - In `pickTarget`, skip a mob that another bot puller is already going for (another party's active pull
     target). Use whatever the Pull struct already stores for its current target.
   - Keep `kPullNear` at 50. Keep the "wandered off" check tied to `kPullLimit`, as it is now.
4. **No actions while sitting.** Find where a bot rests or sits (heal or `/heal`, `Animation`, `animation`, rest
   status) and where it then uses an ability, casts, engages or attacks. Before such an action, a bot that is
   resting must stand up first, the way the code already ends a rest elsewhere, or skip that tick. If there is no
   existing way to stand a bot up, say so in the notes and only skip the action while resting.

Rules: existing code style, no reformatting, no unrelated changes. With the setting `bots.ACTIVE_EVERYWHERE` false
and a real player in the party, behaviour must stay as now, except items 1, 3 and 4, which apply to all bot-only
parties. You can't compile here, so only use functions, types and members that already appear in this code, and list
every assumption. Write `JOB_O_NOTES.md`: what changed (file and function), the cause of each, and a test plan for
the test server. The plan should say which log lines show it working: "heads out to", pull lines "is going for" and
"pulled" from many more leaders, pull distances under 100, fewer "target gone", and fewer "invalid animation state"
warnings. Edit only files in this folder.
