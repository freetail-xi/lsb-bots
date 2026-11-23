# Job E: three small behaviour changes (DESIGN-review.md proposals 1, 2 and 9)

This folder is lsb-bots, a C++ bot module for LandSandBoat (an FFXI server, 75 era). Read `DESIGN-review.md`
(sections 1 and 3) first. Implement these three proposals, in the existing code style, with new settings added to
`settings/bots.lua.example` (each with a short comment and a sensible default):

1. **Leave real players' mobs alone** (proposal 1). In `huntTarget` (`bot_ai.cpp`) and the pull target choice
   (`unpullable` / `pickTarget` in `bot_pull.cpp`), skip a mob that a real player (not a pool bot) is fighting or
   has claimed, or that is within `bots.PLAYER_LEEWAY` yalms (default 40) of a real player who isn't in the bot's
   own party. A bot still defends itself when something attacks it. In `balance` (`bot_module.cpp`), don't place
   or send a bot to a camp spot within that leeway of a real player.
2. **A level window** (proposal 2). Add `bots.LEVEL_WINDOW` (default 10). `balance` doesn't send a bot whose level
   is further than that from the player it's balancing for (leave the zone under-filled instead), and
   `loginNext`'s "near a player's level" preference uses the same window. A bot doesn't queue an invite for a
   player more than LEVEL_WINDOW levels away (the `wantsParty` invite branches in `bot_chat.cpp`, tell and shout);
   when it skips one, the reply prompt must not say an invite was sent.
3. **Shouts name the real zone** (proposal 9). The unpartied shout in `chatter` (`bot_chat.cpp`) becomes a fixed
   line built in code, not by the model: job abbreviation, main level and the current zone's name, in era player
   style. Use a few varied templates, for example "lfp 24 war, valkurm", "LFP WAR24 @ Valkurm Dunes",
   "24war lf pt in valkurm". Lowercase or short zone names are fine if the code has them; otherwise use the zone
   name as it is.

Rules: don't change anything else and don't reformat files. You can't compile here (the server headers aren't in
this folder), so only use functions, types and members that already appear in this code, and say in the notes
where you had to assume something. Write `JOB_E_NOTES.md`: what changed (file and function) for each part, the
new settings, and how to test each on the test server.
