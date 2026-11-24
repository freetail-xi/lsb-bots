# Job S: two small fixes from the citizens and solo test (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Commits up to `888b2a7` are in.
Read `bot_citizen.cpp` (how stops and `{name}` lines pick NPCs, the `ForEachNpc` calls), `bot_ai.cpp` (the
`GP_CLI_COMMAND_CAMP` sender near 194, `rest` near 730, `restInFight` near 793, and every caller that starts a
/heal) and `bot_ai.cpp` `wander` first.

1. **Citizens greet only townsfolk.** A citizen said "Good morning, Synergy Furnace." Synergy Furnace is a post-75
   system NPC.
   - For citizen stops, and for the `{name}` small-talk lines, skip NPCs that are machines, objects or later-era
     systems. Skip a name containing any of these words, case-insensitive: Synergy, Furnace, Ambuscade, Eminence,
     Unity, Survival, Guide, Geomagnetic, Fount, Ethereal, Junction, Waypoint, Door, Gate, Counter, Board, Tome,
     Point, Crystal, Moogle, Station, Home Point, Rift, Spatial, Displacement, Book, Statue, Plaque, Chest, Coffer,
     Tape, ???.
   - Keep the list as one plain array so it's easy to edit.
   - A stop at a skipped NPC is not chosen at all.
   - The `{name}` lines are used only when the stop NPC's name passes this filter and is one or two words.
   - Apply the same filter in `wander` for town strolls if it picks NPCs the same way, so other bots don't loiter
     at a Synergy Furnace either.
2. **No /heal while engaged.** The server warned "Invalid GP_CLI_COMMAND_CAMP ... Invalid state: Engaged" for solo
   bots Rodney and Plod. Find every path that sends the camp-on packet. Don't send it while the bot is engaged
   (`PChar->PAI->IsEngaged()`) or has a battle target. Disengage first if the code's existing rest path means to
   rest mid-fight on purpose (`restInFight`); otherwise skip resting that tick. Camp-off (standing up) is
   unaffected.

Rules: existing code style, no reformatting, no unrelated changes. You can't compile here, so only use functions,
types and members that already appear in this code, and list every assumption. Write `JOB_S_NOTES.md`: what changed
and a test plan (citizen "walks to" and "says:" lines naming no machine NPCs; no "Invalid GP_CLI_COMMAND_CAMP"
warnings). Edit only files in this folder.
