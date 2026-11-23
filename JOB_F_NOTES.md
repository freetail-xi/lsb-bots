# Job F notes

Could not compile here (server headers are not in this folder). Everything below uses functions, types, and members that already appear in this module, except the two standard-library calls called out under assumptions. Those are marked.

The test server reads `settings/bots.lua`, not the example. Copy `SESSION_MIN` and `SESSION_MAX` into that file before testing. Both are read with `settings::get<uint32>`.

## Settings

In `settings/bots.lua.example`:

- `bots.SESSION_MIN` (default 20). Minutes. Shortest session a login can roll.
- `bots.SESSION_MAX` (default 120). Minutes. Longest session a login can roll. `0` turns session limits off.

The roll is inclusive: a bot stays online for a whole number of minutes from `SESSION_MIN` through `SESSION_MAX`. `MAX_ONLINE` is still the cap. Logins still happen one at a time on `LOGIN_INTERVAL` whenever the online count is under that cap, so a logout makes a vacancy the next login fills.

## Schema

`bot_pool.diary`, `text NULL`.

`ensureSchema` (`bot_pool.cpp`) adds it in both places the other late columns use:

- the `CREATE TABLE` (new databases),
- `ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS diary text NULL AFTER chattiness` (a table that already exists).

`loadPool` selects `b.diary` with the rest of the pool. Null becomes an empty string. That text is kept in memory for the process (updated on every new line) and copied onto the bot in `loadDiary` when it logs in. Each line is `<unix seconds> <fact>`, newest last, at most 8. A ninth line drops the oldest.

## 1. Sessions

**`bot_module.cpp` `sessionMinutes`, `login`.** After a successful `enterZone`, the login rolls a length and sets `Bot::sessionEnds`. The map log says `bots: <name> logged in for N minutes`. `SESSION_MAX` of 0 leaves `sessionEnds` unset and keeps the old log line. `balance` and `loginNext` both go through `login`, so both kinds of login get a session.

**`bot_module.cpp` `canEndSession`, `tick`.** Once `now` is past `sessionEnds`, the bot logs out only when all of these are true:

- not `withPlayer` (it is in, or was just in, a real player's party, including the grace while that player zones),
- `isFree` (no party at all — the same check `rotate` uses),
- not dead, not `PAI->IsEngaged()`, not `isInEvent()`, and not `Status::Disappear` or `Status::Shutdown`.

Anything else waits. The check runs again next tick. There is no separate retry timer. A bot in a real player's party does not log out for the session while `withPlayer` is set. After that party is actually gone and the bot is free and out of combat, a session that already ended does log them out. That is the "try again later" case, not a second session.

One session logout per tick. Each logout saves the character, and the tick watchdog is 2 seconds. The others whose sessions have ended go on the following ticks. The log line is `bots: <name>'s session is over`, then the usual `logged out` line.

Session logout does not write `resting_`. Only `rotate` still parks a bot for 15 minutes. A bot whose session ended can be logged back in on a later `LOGIN_INTERVAL`, and that login rolls a new length. `rotate` and `!rotatebots` are unchanged: they still log out unpartied bots only, still park them, and still set `nextLogin_` so a replacement can come immediately.

While no real player is online (`wanted` is false), session logout does not run. The existing drain still logs everyone out after `LOGOUT_GRACE`, including bots in a party.

### Assumptions

- "Not in combat" is `isDead()`, `PAI->IsEngaged()`, or `isInEvent()`. A bot who is only walking is free to log out. Those are the combat checks this module already uses.
- `withPlayer` is updated in `think`, which runs after the session check. The flag from the previous tick is what the check sees. A party that still exists also fails `isFree`, so a bot cannot session-logout in the middle of a party.
- If `SESSION_MIN` is greater than `SESSION_MAX` and the max is not 0, the two are swapped.
- A rolled length of 0 (only possible when `SESSION_MIN` is 0 and the max is not) sets the end to now, so the bot logs out on the next tick once it is free.
- A missing key reads as 0 from `settings::get<uint32>`. `SESSION_MAX` of 0 therefore means "no session limit", which is also what an old `bots.lua` that does not have the key yet will do. Copy the keys in before expecting turnover.
- `rotate` can still log out a bot whose session just started. That path was left as it is. What session logout does not do is add the 15-minute park, so the park does not sit on top of a session ending and block the next login.

### How to test

1. Put `SESSION_MIN` and `SESSION_MAX` in the test server's `settings/bots.lua`. For the turnover test set `SESSION_MIN = 1` and `SESSION_MAX = 2`, and restart the map.
2. Watch the map log for `logged in for N minutes` (N is 1 or 2). `!bots` should stay near `MAX_ONLINE`. As each session ends, the log says `<name>'s session is over` and the count drops by one, then a new login fills it on `LOGIN_INTERVAL` (default 8 seconds). They will not all leave in the same tick.
3. Invite one bot whose session is about to end (or set the max to 1 minute and invite immediately). It should stay in the party past that time. The log should not say its session is over while `!bot <name>` still says `in a player's party`.
4. Leave that party, or let a bot-only party break, and don't be in combat. Once it is unpartied and not fighting, a session that already ended should log it out within a few ticks.
5. Pull a mob with an unpartied bot (or let one be in combat at the 2-minute mark). It should not log out mid-fight. After it disengages, it should.
6. `!rotatebots`. It should still rotate bots that are not in a party, and those names should not log straight back in (they sit in `resting_` for 15 minutes, unless the pool has nobody else). A session-end logout is the one that does not sit out.
7. Set `SESSION_MAX = 0` and restart. Logins should use the old `logged in` line, with no `for N minutes`, and nobody should drop for a session.

## 2. Seeking

**`bot_ai.cpp` `think`, `setSeeking`.** The flag used to be on for every unpartied bot. It is now on only when the bot has no party and `active` is true. `active` is the same value as before: `withPlayer`, or a real player in the bot's zone (`occupiedZones`). An unpartied bot in a zone with nobody real is idle and will not path, hunt, or answer a shout, so `/sea` should not list it.

The character is reloaded on every `enterZone`, which can bring back a seeking flag saved from the previous life. `seekingApplied` is cleared there, and the next `think` that reaches the check sends the flag again even when the in-memory bit already matches. Dead and mid-cutscene bots return before that check, so the send waits until they are up.

### Assumptions

- "Idle because nobody real is in its zone" is `!active`, including a bot that is fighting back after an aggro. It still fights (`think`'s idle return is unchanged). It does not advertise LFP while it cannot come to a party.
- A bot that is unpartied in a zone a real player is standing in still sets seeking, same as before.
- The first send after a load is what clears a flag that was stored on the character. This module never read that saved bit; `Bot::seeking` starts false, so without the forced send an idle bot could stay on `/sea`.

### How to test

1. With the short sessions above, or with normal ones, stand in a zone that has no bots. `/sea all` should not list bots that `!bots` shows in some other, empty zone.
2. Walk into a zone where an unpartied bot is standing. `/sea` should list that one. `!bot <name>` shows it with no party.
3. Invite it. It should drop off `/sea`. Drop the party and stay in the zone. It should show again.
4. Zone away and leave it alone. It should drop off `/sea` again once its zone has no real player.

## 3. World diary

**`bot_pool.cpp` `loadDiary`, `noteEvent`, `recentEvents`.** Up to 8 lines, newest last, on `Bot::diary`. `noteEvent` appends, trims, and writes the whole column. The map log line is `bots: <name> diary: <fact>`. `recentEvents` turns the newest 3 (or fewer, if that is all there are) into one sentence. Empty diary means `describe` adds nothing.

The stamp is the server's local calendar day, not Vana'diel:

- same day: `earlier today`
- the previous day: `yesterday`
- older: `a few days ago`

The prompt keeps the lines in diary order (oldest of those three first), so a death still reads before the raise. `describe` (`bot_chat.cpp`) adds, only when there is at least one line:

`Recent events (true; you may mention them): earlier today, died in Valkurm Dunes; earlier today, got raised.`

The old sentence that forbids inventing a past is unchanged. The diary is the past that sentence is allowed to use, because it is part of "what's above".

Lines, and where they are written:

| Fact | Where |
| --- | --- |
| `logged in at <zone>` | `login`, after `enterZone` succeeds. Zone text is `prettyName` (`_` to space). |
| `reached level <n>` | `prepare` (`bot_gear.cpp`), only when `preparedLevel` was already set and the main level went up. The login `prepare` does not count (level was 0). Several dings in one fight become one line, the level they came out at, because `prepare` waits until they are not engaged. |
| `died in <zone>` | `think`, the tick death is first seen. |
| `got raised` | `think`, the tick they are alive again, if this death did not homepoint. |
| `homepointed` | `think`, the tick the homepoint menu is sent. |
| `joined a party with <name>` | `think`, the tick `withPlayer` becomes true. The name is the first real PC on the party member list. |
| `left the party` / `broke up the party` | `leaveOrBreakUp`. Break up when this bot leads and the call is the disband; otherwise left. |
| `gave <name> a linkpearl` | `linkshellUpkeep`, only after the lua script returns success. |

`deathNoted` / `homeNoted` stop the same death from being written again when the homepoint menu is sent a second time (the old code already clears `diedAt` and will wait out the delay again if they are still dead).

### Assumptions

- The clock is `std::chrono::system_clock` wall time, and the day boundary is `localtime_r` (POSIX). Neither appears elsewhere in this module. `timer::time_point` is never turned into a calendar time here, so it cannot stamp a line that has to survive logout. Assumption: the map process is Linux, `<ctime>` declares `localtime_r`, and the server's local zone is the "today" players mean. A future timestamp (clock skew) is phrased `earlier today`. If `localtime_r` fails, the phrase is `earlier`.
- `fmt::format` is available the same way the other cpp files use it. `bot_pool.cpp` includes `<fmt/format.h>` because it did not use fmt before.
- `ADD COLUMN IF NOT EXISTS` is the MariaDB syntax the chattiness and subjob alters already use.
- The in-memory copy filled by `loadPool` is what the next login in the same process reads. `noteEvent` updates it and the column together. A restart reads the column again in `loadPool`. A line is not also stored on `PoolEntry`; the pool load is what fills the copy.
- A bot that loads already dead gets a `died in ...` line on the first `think`, as if the death were seen then.
- The party name is whichever real PC `members` yields first, not necessarily the person who sent the invite.
- A linkpearl line is skipped when the lua script errors. The older `gives <name> a linkpearl` log line is still printed before the script runs. Trust the `diary:` line.
- `leaveOrBreakUp` still assumes `PParty` is set, as it did before. The new line is written only on that same path.
- Prompt order is oldest-of-the-three first. The design note said newest first and two lines. The job says the newest 2 or 3, stored newest last, so the prompt shows up to 3 in that stored order.
- Facts are the short clauses above, with no period of their own. The prompt adds one period after the whole sentence.

### How to test

1. Restart the map so `ensureSchema` runs. `SHOW COLUMNS FROM bot_pool LIKE 'diary';` should show a nullable `text` column. An existing table should gain it without a new database.
2. Log a bot in. The log should include `diary: logged in at <zone>`, and `SELECT diary FROM bot_pool WHERE name = '<name>'` should show one line starting with a unix time. `!bots` zone and that name should be the same place.
3. Party with the bot, ask what it's been doing (party chat or a tell). The reply can mention logging in, or the party, in that zone. It should not invent a relic, a gil amount, or a zone it was not in. The prompt rule against making up a past is still there.
4. Die with it, get a raise, ask what happened. The log should show `died in <zone>` and then `got raised`. The reply can mention that death and that zone, and should not name a different zone.
5. Let one die with nobody who can raise (or wait out the homepoint timer). The log should show `homepointed` and not `got raised`.
6. Level a bot (fight until it dings, then leave combat so `prepare` runs). The log should show `reached level <n>` with the new main level, once.
7. Have a real player join, then disband or kick. The log should show `joined a party with <player>` and then `left the party` or `broke up the party`.
8. Tell a pearl-wearing bot to ask for a pearl (`offersPearls` / even char id, shell `Wanderers` by default). On success the log shows `gave <you> a linkpearl`.
9. Force a logout (short session, or `!rotatebots` after the 15-minute park, or restart the map). Log the same bot back in. Older lines should still be in the column and in the prompt, with `yesterday` or `a few days ago` only once the calendar day has changed. A ninth distinct event should drop the oldest. The prompt should mention at most the newest three.
