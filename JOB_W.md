# Job W: bots on a server split across several map processes

LSB can run several `xi_map` processes, each serving the zones whose `zone_settings.zoneip`/`zoneport` match its
`--ip`/`--port` (`zoneutils::GetZonesAssignedToThisProcess`; `zoneutils::GetZone(z)` is null for a zone another
process runs). Each process loads this module. The goal: spread the 500+ bots over 2-4 processes (CPU cores) on the
test server, with every bot online in exactly one process and bots still able to travel anywhere.

Today that breaks, at least here:
- `clearStaleState()` (bot_pool.cpp) deletes every bot session and party row at startup, so a second process
  starting kicks the first one's bots.
- Every process logs in from the same `bot_pool`, so two processes log in the same bot.
- `login()` refuses a zone this process doesn't run ("which this map server doesn't run"), so a bot whose `travel()`
  destination is on another process gets stuck.
- `online_`, recruits, outings, pulls/camps, citizens and `GetCharByName`/`GetChar` lookups are per process.

## What to build

1. **Single process stays exactly as today.** When this process serves every zone (`GetZonesAssignedToThisProcess`
   with ip 0, or every pool bot's zone is local), all new behaviour is off. Test that path first in your head:
   nothing may change for Freetail live, which runs one process.
2. **Ownership.** A bot belongs to the process that serves the zone it's in.
   - Logins: a process only logs in bots whose saved zone (or the login zone it picks: camp, pool home) is local.
     Pick login zones from local zones only.
   - Claim before login, atomically, so two processes can't take one bot. Use a new column, for example
     `bot_pool.owner_port SMALLINT UNSIGNED NOT NULL DEFAULT 0` (0 = nobody), with
     `UPDATE bot_pool SET owner_port = ? WHERE charid = ? AND owner_port IN (0, ?)` and check affected rows.
     Release it (set 0) on logout. Add columns next to the existing ones in bot_pool.cpp (the
     `ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS ...` lines at about line 192), so they're safe to re-run.
   - `clearStaleState()` only clears sessions, parties and claims belonging to *this* process's port (sessions:
     `server_port`; claims: `owner_port`). A crashed process's leftovers are cleared when it restarts.
   - Caps (`BOTS_PER_PLAYER_ZONE`, pool size, balance) count this process's bots, split fairly: the
     total target divided by the number of processes that own bots, or per local zone, whichever the code fits
     best. Say which you chose.
3. **Handoff when a bot travels to a zone another process runs.** In `travel()` (and anything else that sets
   `loc.destination`), if the destination isn't local:
   - save the character's new zone and position the way a normal zone change would (charutils save calls
     already used in this module), write a handoff row (for example `bot_pool.handoff_zone`, `handoff_at`), log
     the bot out cleanly here (party kept), and release the claim to the target port;
   - every process polls (every few seconds, not per tick) for handoff rows whose zone is local, claims them and
     logs them in at the saved position. Log one line each side:
     `bots: <name> handed to map <port> (zone <z>)` and `bots: <name> arrived from map <port> (zone <z>)`.
   - A handoff that nobody picks up in 60 s is cleared and the bot goes back to the pool (logged).
4. **Cross-process logic degrades safely.** Recruiting, cross-zone invites, outings, camps and pulls only consider
   bots and zones on this process. A party whose members end up on different processes must not crash or
   spin: check every `GetChar`/`GetCharByName`/`GetZone` result for null (most already are; find the rest).
   Prefer camps in local zones (`pickPartyCamp`, `findCamps` callers). A party leader on another process is
   treated like "leader not here": follow logic waits or the bot leaves after a timeout, never a tight loop.
5. **Logs.** At startup: `bots: map <port> serves <n> zones, owns bots in <m> of them` and, when split,
   `bots: split mode: <k> map processes share the pool`.

## Not this job
- The server config (which zones go to which port, start scripts). The test server's operator does that.
- Real players are unaffected: LSB already moves them between processes.

## Rules
Existing code style, no reformatting, no unrelated changes. You can't compile here, so only use functions, types
and members that already appear in this code or in LSB's headers as this code already uses them (read
`../freetail-server/src/map` for LSB, read-only), and list every assumption. Write `JOB_W_NOTES.md`: what changed,
the new columns, the single-process guarantee and why it holds, and a test plan for the test server (two processes,
each zone set listed by the operator; no double logins; a bot walks across the process boundary and arrives; a
crashed process restarts without kicking the other's bots; one-process run unchanged). Edit only files in this
folder. Read-only git (log, diff, show) is fine; don't commit, push or change branches.
