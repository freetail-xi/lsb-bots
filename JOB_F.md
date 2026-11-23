# Job F: sessions and a world diary (DESIGN-review.md proposals 3 and 11)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Read `DESIGN-review.md`
(sections 1 and 3, proposals 3 and 11) and the code first. Jobs D and E are already committed (see `git log`), so
build on what's there now. Implement:

1. **Sessions** (proposal 3). Each login rolls a session length between `bots.SESSION_MIN` and `bots.SESSION_MAX`
   minutes (defaults 20 and 120). At session end a bot logs out only if it's free (no party, not in combat, the
   same rule as `rotate`); otherwise it tries again later. A bot in a real player's party never times out.
   `MAX_ONLINE` stays the cap; logins keep coming at `LOGIN_INTERVAL` to refill it, so the population turns over.
   Keep `rotate` and `!rotatebots` working. Also turn the seeking flag (`setSeeking`) off for a bot that is idle
   because nobody real is in its zone, so `/sea` doesn't list bots that won't move.
2. **A world diary** (proposal 11). Each bot keeps up to 8 short event lines, newest last, stored in a new
   `bot_pool` text column (add it in `ensureSchema` the same way other columns are handled, so an existing table
   gets it) and loaded with the pool. Add lines where the code already knows the facts: died (and where), got
   raised, homepointed, levelled up (new level), joined a party with a real player (their name), left/broke up,
   gave someone a linkpearl, logged in (zone). Keep each line short and factual, with a game-time or real-time stamp
   that reads naturally ("earlier today", "yesterday" is fine). `describe` adds "Recent events (true; you may
   mention them): ..." with the newest 2-3 lines, only when there are any. The existing rule against inventing a
   past stays.

Rules: existing code style, no reformatting, no unrelated changes. New settings go in
`settings/bots.lua.example` with comments. You can't compile here, so only use functions, types and members that
already appear in this code, and list every assumption in the notes. Write `JOB_F_NOTES.md`: what changed (file
and function), the schema change, the new settings, and a test plan for the test server.
