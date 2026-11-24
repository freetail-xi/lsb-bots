# Job L: a bot party with a low-level player never pulls; log what bots say (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Read `bot_pull.cpp` (pull target
choice, `pullChoice`, `setPuller`, the "is going for" / "pulled" / "waits:" log lines), `bot_chat.cpp` (`say`,
`deliverChat`, `worker`, `handleChat`) and `bot_brain.cpp` (`assign_puller`) first. Commits up to `53ee3b2` are in.

What happened on the test server. Party: Evodie BST 5 (leader, puller), Isolde WHM 4, Plod MNK 5, and the real player
Ddd WAR 1. West Ronfaure (zone 100), camp at about x -291, z 264. Around the camp: Wild Rabbit lv1 (x17), Tunnel Worm
lv1 (x9), Ding Bats lv1 (x8); about 53 yalms out: Forest Hare 2-3, Carrion Worm 2-3, Forest Funguar 4-5 (x1), Scarab
Beetle 4-5, Orcish Fodder 3-4, Mouse Bat 2-4.

```
00:35:47 bots: Evodie's party camps at -291,264
00:36:36 bots: Evodie is pulling for Evodie's party (setPuller)      <- after "pull the mob please"
00:37:22 / 00:37:31 / 00:37:36  "PULL A MOB", "a rabbit a worm, I don't care", "pull" -> assign_puller again
00:40:23 bots: Evodie's party pulls only Forest Funguar (pullChoice)
00:40:39 bots: Evodie is pulling for Evodie's party
(no "is going for", "pulled", "pull by ... ended" or "waits:" line through 00:50; Evodie never picked a mob)
```

Also: the real player said bot replies in party chat were rare. The chat model did answer several lines (the gateway
saw them), but the map log has no record of what a bot actually sent, so we can't tell what reached the player.

Fix these:

1. **Pull targets with a low-level real player.** Find why no mob was chosen between 00:36 and 00:40 with lv1-3 mobs
   all around. If the target filter judges "too weak" against the bots' levels (4-5) while a real player at 1 is in
   the party, judge against the party's lowest member level when a real player is in it (a lv1 player should get
   lv1-3 targets), or whatever the code's own notion of "the player's level" is. Keep the existing rules for
   bot-only parties.
2. **"pulls only Forest Funguar".** Find what line set that restriction. If a bot's own chat line or a line that only
   mentions a monster ("I'll charm the Funguar") can set it, make it need a request from a real player in the party
   (the same speaker check other orders use). Also: when a restriction leaves no target in reach for 2 minutes, drop
   it, log that, and let the puller say so in party chat if the code already has a way to queue a party line.
3. **Log no target.** When the puller has a camp and wants to pull but finds nothing, log one info line with why
   (no mob in range / all too weak / all too strong / restricted to X / claimed), at most once per minute per party.
4. **Log what bots say.** In `say` (or where a reply is actually sent), one info line per line sent:
   `bots: <name> says (<party|tell to X|shout|linkshell|say>): <text>`. Shouts are already logged in chatter;
   don't log those twice.

Rules: existing code style, no reformatting, no unrelated changes. You can't compile here, so only use functions,
types and members that already appear in this code, and list every assumption. Write `JOB_L_NOTES.md`: what changed
(file and function), the cause you found for each problem, and a test plan for the test server. Edit only files in
this folder.
