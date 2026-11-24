# Job P: citizen bots go about their day in town (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Commits up to `4cd7b71` are in.
Read `bots.h` (`Bot`), `bot_module.cpp` (`loginNext`, `available`, `formParties`, `askSeekers`, rotation, sessions,
balance), `bot_ai.cpp` (`think`, `wander` and its `ForEachNpc` near 576, `setSitting`, `emote`, `walkTo`, the hunt
and seeking code), `bot_chat.cpp` (`say`, `Reply`, `chatter`, `handleChat`, `respond`) and
`settings/bots.lua.example` first.

The goal: some pool bots are townsfolk instead of adventurers. They live in one city, walk between its shops and
landmarks, linger, sit, emote, and make small talk in /say. They answer a real player who talks to them. This is a
first version in one city. Later this will move to real NPC entities, so keep the citizen code in its own place
(new file `bot_citizen.cpp` with declarations in `bots.h`) with small hooks elsewhere.

Settings (add to `settings/bots.lua.example` with comments like the others):
- `CITIZENS = 0`: how many pool bots are citizens. 0 turns the feature off, and then behaviour must be exactly as
  now.
- `CITIZEN_ZONE = 230`: the city they live in (Southern San d'Oria).
- `CITIZEN_SAY_MINUTES = 4`: about how often each citizen says a small-talk line, only while a real player is
  within 30 yalms of it. 0 turns small talk off.

Do this:

1. **Who is a citizen.** The `CITIZENS` pool entries with the lowest charId, a stable choice across restarts. A
   citizen is marked on its `Bot` (new member). Citizens log in first, before any other bot, while bots log in at
   all, and they count toward MAX_ONLINE. At login, a citizen is placed in `CITIZEN_ZONE` next to a random named
   NPC there (use `placeAt` / `spotNear` the way `loginNext` does), never at a camp.
2. **Citizens never adventure.** They skip all of these: `formParties`, `askSeekers`, the seeking flag, solo hunting,
   pulling, camps and head-outs, balance moves to other zones, rotation, session-end logouts, party, shout and
   linkshell chatter, and party invites (they decline politely if a player invites one, using the existing decline
   path if there is one). They still fight back if attacked, like an idle bot.
3. **A routine.** While a real player is in the citizen's zone (or `ACTIVE_EVERYWHERE` is true), a citizen repeats:
   - Pick a stop: usually a random named, visible NPC in the zone, found the way `wander` finds NPCs. Sometimes its
     home spot (where it logged in). Never the same NPC twice in a row.
   - Walk there (`walkTo`, or `travel` only inside the same zone) and stand within about 3 yalms, facing it.
   - Linger 30-120 s. While lingering, sometimes emote (`emote`), and sometimes sit for a while at a home stop
     (`setSitting`). Stand before walking on.
   - If the walk fails or takes over 2 minutes, pick another stop.
   - Stagger citizens so they don't all move on the same tick. With no real player in the zone, they stand idle as
     bots do today.
4. **Small talk in /say.** About once per `CITIZEN_SAY_MINUTES` per citizen (randomized), only while a real player
   is within 30 yalms, say one canned line in /say through the existing `say` path. No model call.
   - Write about 40 short lines in this city's voice: San d'Oria, Elvaan pride, knights, the cathedral, prices, the
     weather, Orcs.
   - Some lines use the stop NPC's name ("Good morning, Machielle.").
   - Never the same line twice in a row for a bot, and not the line another citizen within 15 yalms said in the
     last minute.
   - Keep the lines as a plain array so they're easy to edit.
5. **Answering players.** A real player's /say that names a citizen, or comes from within 6 yalms of it and is
   addressed to nobody else, gets a reply in /say through the existing model reply path for say. Add one line to
   the prompt context saying this bot is a resident of the city who doesn't adventure. One reply per heard line.
   Model replies from citizens are capped at one per citizen per 20 seconds.
6. **Logging.** Info lines, at most one per citizen per stop:
   - `bots: <name> (citizen) walks to <NPC or home>`
   - `bots: <name> (citizen) says: <text>`, through the existing "says" log in `say`.

Rules: existing code style, no reformatting, no unrelated changes. With `CITIZENS = 0` nothing changes. You can't
compile here, so only use functions, types and members that already appear in this code, and list every assumption.
Write `JOB_P_NOTES.md`: what changed (file and function), and a test plan for the test server. The plan uses
`CITIZENS = 30` and a player standing in Southern San d'Oria, and says which log lines show it working. Edit only
files in this folder.
