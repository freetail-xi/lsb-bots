# Job K: party orders the brain gets wrong (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). A real player (Ddd) played in a
bot party on the test server tonight. Read `bot_brain.cpp` (the party brain: `askBrain`, the tools list, `plansFor`,
`brainOutcomes`) and `bot_chat.cpp` (`handleChat`, `respond`, `carryOut`) first. Commits up to `382528e` are in;
`git log` shows them. These are the server log lines (time, what Ddd typed in party chat, what the brain did):

```
00:16:42 "(H-6)"                          wanted break_camp, guard blocked (correct: a map grid, not an order)
00:33:16 "You're the leader Evodie"       set_tank, 1 job            WRONG: leader is not tank
00:33:45 "this way"                       wanted break_camp, guard blocked, 0 jobs   WRONG: means follow me
00:34:38 "I want experience points and you all are standing at the zone line"
                                          wanted break_camp, guard blocked           WRONG: they want to move out
00:36:32 "fight?"                         wanted break_camp, guard blocked           (fine to block; better: answer)
00:38:55 "let's change camp"              wanted break_camp, guard blocked, 0 jobs   WRONG: plainly asks to move
00:35:15 "bots: emote from Ddd: target ... index 24 ..."  logged 3 times in the same ms for one emote
```

Fix these:

1. **The break_camp guard** (`plansFor`, the line "the party brain wanted camp broken on ..., which doesn't say to")
   blocks real requests to move. Widen its phrase list so "change camp", "move camp", "this way", "over here",
   "come on", "let's move", "lets go", "go somewhere else", "find a better spot", "zone line" (as a complaint about
   where they stand) and similar count, while "(H-6)", a bare question like "fight?", and chat that only mentions a
   place still don't. Keep it a phrase list (no model call), like the existing one. Add the new phrases next to
   the existing ones in the same style.
2. **Leader is not tank.** "You're the leader X", "X leads", "X is leader" must not become `set_tank`. If the brain
   picks set_tank on a line whose only role word is leader/lead, drop that plan (log it like the break_camp guard
   does). If lsb-bots already has a way for a party leader to pass the lead to a named member, use it only if the
   real player is the current leader; otherwise no action. Do not add a new packet path for this.
3. **A no_action line still gets answered.** Check that when the brain returns no_action (or a guard drops every
   plan), the line still reaches `respond` so a bot can reply in party chat, the same as a line with the brain
   off. Lines like "where are we fighting", "who's pulling", "hello?" got no reply tonight. If the code already does
   this, say so in the notes with the exact path, and don't change it.
4. **The emote logged three times.** Find why one emote from a real player is logged once per bot (or per listener)
   and log it once.

Rules: existing code style, no reformatting, no unrelated changes. You can't compile here, so only use functions,
types and members that already appear in this code, and list every assumption. Write `JOB_K_NOTES.md`: what changed
(file and function), the phrase lists before and after, and a test plan for the test server (what to type in party
chat and which log line to expect). Edit only files in this folder.
