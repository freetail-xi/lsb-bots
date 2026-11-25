# Job U: crash and stall review of Jobs N–S (2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server). It runs inside `xi_map`, so a crash here
takes the whole map server down for everyone. Jobs N–S were written without a compiler and have only had a short
test run: blood pacts and ACTIVE_EVERYWHERE (N), camps and pulls (O), citizens in `bot_citizen.cpp` (P), solo
leveling and `seekPrey` (Q), gathering at HELM points (R), the NPC skip list and no /heal while engaged (S).
`git log` shows the commits; `JOB_*_NOTES.md` explain each one.

Review that code for problems that crash, hang or slowly break the server, and fix them:

1. **Dangling pointers.** Any `CBaseEntity*`, `CNpcEntity*`, `CMobEntity*`, `CCharEntity*` or zone pointer kept
   between ticks (in `Bot`, statics, maps, lambdas) after the target can despawn, the bot can zone, or a player can
   log out. Store IDs and look them up again each tick, the way other code here already does.
2. **Null checks.** `loc.zone`, `PAI`, `PPet`, `PBattleTarget`, `StatusEffectContainer`, `GetEntity`, item and
   weapon lookups, `findCamps`/`ForEachNpc` results that can be empty.
3. **Containers.** Erasing from or adding to a container while iterating it, including in `ForEachNpc`/`ForEach*`
   callbacks that zone or log out a bot.
4. **Zoning and logout mid-action.** A gather trade, citizen walk, camp walk, pull or `seekPrey` travel interrupted
   by a zone change, death, logout or `LOGOUT_GRACE` ending. State must reset and nothing should be sent for a bot
   that isn't in a zone.
5. **Endless loops and log spam.** Anything that can retry every tick forever (path not found, event never ends,
   no camp in zone, trade refused) and any log line that can print every tick.
6. **Threads.** Anything the chat or brain worker threads touch that the map thread also changes, without the lock
   the existing code uses.

Fix only real problems. Small, local fixes in existing style; no refactors, no reformatting, no behaviour changes
beyond the fix. You can't compile here, so only use functions, types and members that already appear in this
code, and list every assumption. Write `JOB_U_NOTES.md`: each problem (file, function, what goes wrong, how
likely), the fix, and a short test plan. List anything suspicious you did not change. Edit only files in this
folder. Read-only git (log, diff, show) is fine; don't commit, push, or change branches.
