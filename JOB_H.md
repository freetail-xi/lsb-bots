# Job H: parties with roles, and asking a seeking player (DESIGN-review.md proposals 5 and 6)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Read `DESIGN-review.md`
(sections 1 and 3, proposals 5 and 6) and the code first. Jobs D, E and F and several fixes are already committed
(see `git log`), so build on what is there now. Implement:

1. **Parties with a role** (proposal 5) in `formParties` (`bot_module.cpp`):
   - Only bots within 40 yalms of the leader-to-be join. Keep the existing "within 5 levels" rule.
   - Fill tank, then healer, then damage, up to 6, using the helpers that already exist (`isTank`, `partyHealer`
     or WHM/RDM/SCH, `pullerRank`). Below level 15 a party may have no tank; at 15 and up it needs a healer.
     If the zone only has mages, still let a small party form below 15 rather than none.
   - The tank leads (or the best `pullerRank` when there is no tank). If the code needs the lead passed, do it
     before the invites go out.
   - `autoCamp` lets the leader be the puller when `pullerRank` says so (today the loop skips `PMember == PLeader`).
2. **Ask a seeking player** (proposal 6). A new step called from `BotModule::tick` every 30 to 60 seconds, next to
   `formParties`, behind a new setting `bots.INVITE_SEEKERS` (default true):
   - Find a real player (not a pool bot) who is seeking a party, unpartied, not in combat, and within
     `LEVEL_WINDOW` of a bot party that already has a camp or hunt target and has room.
   - The party's leader sends one canned tell built in code (not the model), naming the real zone (`prettyName`),
     the level band and the role it's short of, in era player style, e.g. "hi, need a whm for 12-15 in valkurm,
     want to join?". Then it queues an invite through `Bot::invites` a few seconds later.
   - Cooldowns: 15 minutes per player, 40 minutes per leader. A player who tells a bot "no invites" (or similar,
     use the existing chat handling) is never asked again until the server restarts.

Rules: existing code style, no reformatting, no unrelated changes. New settings go in
`settings/bots.lua.example` with comments. You can't compile here, so only use functions, types and members that
already appear in this code, and list every assumption in the notes. Write `JOB_H_NOTES.md`: what changed (file
and function), the new settings, and a test plan for the test server. Edit only files in this folder.
