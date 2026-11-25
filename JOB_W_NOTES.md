# Job W notes

Could not compile here. New calls are functions, types, and members this module or the map headers it already includes already use (`g_PZoneList`, `GetZone`, `GetZoneIPP`, `GetZonesAssignedToThisProcess`, `GetManagedZones`, `CZone::GetPort`, `IPP`, `rowsAffected`, `charutils::SaveCharPosition`, `charutils::removeCharFromZone`, `CCharEntity::m_moghouseID`). Nothing outside this folder was edited.

## What changed

**Split detection** (`prepareSplit`, once at startup, before `clearStaleState`). This process's zones are `g_PZoneList` plus `GetManagedZones` (lazy zones that are assigned but not loaded). It serves every zone when that set is at least `GetZonesAssignedToThisProcess(IPP(0, 0))`. Endpoints are distinct `(zoneip, zoneport)` rows in `zone_settings`. The process port is the one non-zero `zoneport` on its zones (`CZone::GetPort`, then `zone_settings` for zones not loaded yet). Mixed ports (what `--ip 0` looks like when several ports are configured) leave the port at 0.

Split mode is on only when this process does not serve every zone, there is more than one endpoint, and the port is a single non-zero value. Startup always logs `bots: map <port> serves <n> zones, owns bots in <m> of them` (`m` is how many of those zones currently have an enabled pool bot saved in `chars.pos_zone`). When split: `bots: split mode: <k> map processes share the pool`.

**Claims.** `owner_port` is taken before `placeAt` / `placeCitizen` / `login`. The update is `owner_port = ? WHERE charid = ? AND owner_port IN (0, ?) AND handoff_zone = 0`. MariaDB reports changed rows, not matched rows, so a bot this process already owns updates 0 rows; a follow-up SELECT accepts the row when `owner_port` is already this port and no handoff is open. Logout sets `owner_port` back to 0. A failed `enterZone` releases the claim too.

**Logins.** A process only places a bot in a zone it runs. Camps come from `findCamps`, which already walks `g_PZoneList`. A pool home on another map is not a login zone: a local camp is used when one exists, otherwise that bot is left for the other map and the next candidate is tried. Citizens are skipped when `CITIZEN_ZONE` is not local. With split off, `zoneLocal` is true and the first rolled candidate is still the one who logs in; the home-zone query is not run.

**Caps.** `bots.MAX_ONLINE` is split by zone share: `(MAX_ONLINE * localZones + allZones / 2) / allZones`, at least 1 when the cap and the local zone count are both non-zero. That is the whole target divided by zones, not by however many processes currently hold a claim (that count is 0 at startup and jumps as logins land). `BOTS_PER_PLAYER_ZONE` is unchanged. It is already per zone, each zone is on one process, and the count is this process's `online_`. `balance` uses the zone share as the process cap.

**Handoff.** `travel` to a zone that is not local, with a non-zero `zoneport` that is not this process, sets the destination and queues one row. It does not set `requestedZoneChange` or `Disappear`, so the zone server does not also try to move them. At the end of the tick the module saves the position (`SaveCharPosition` when they have not already disappeared, then the same `chars` position update `placeAt` uses, including `pos_zone`), takes them out of the zone without `Status::Shutdown` (so `removeCharFromZone` does not delete `accounts_parties`), drops the session, destroys the character (`PopMember` keeps the party row; `m_moghouseID` is cleared so the destructor sends the usual party reload), then writes the handoff. The row is published only after the session is gone, so the other map cannot log them in while this one still has them. Log: `bots: <name> handed to map <port> (zone <z>)`.

A zone change this module did not start (a homepoint, anything else that leaves `BLOWFISH_PENDING_ZONE` with a destination this process does not run) takes the same path.

Every 3 seconds, not every tick, each process expires handoffs older than 60 seconds and logs in arrivals whose `handoff_zone` it serves, at the saved position, even if that would pass the local cap. A free solo arrival is not the bot the over-cap logout drops for 60 seconds; a party member is not free, so the existing logout already leaves them. Log: `bots: <name> arrived from map <port> (zone <z>)`. An expired row is cleared and logged `bots: <name> handoff expired (zone <z>); back in the pool`.

**Crash cleanup.** One process: the original two deletes, every bot session and every bot party row. Split: bot sessions with `accounts_sessions.server_port` equal to this port; party rows for bots this port owns, plus party rows for bots nobody owns that have no session (leftovers from before `owner_port` existed). Then claims and handoff columns for this port are cleared. The other process's sessions, claims, and party rows stay.

**Other maps.** Recruit zoning does not travel at a camp this process does not run. `pickPartyCamp` skips a camp that is not local (`findCamps` already only returns loaded zones). A party member whose copied camp is on another map travels there once, which hands them off, instead of clearing their path every tick. A leader who is not on this map (`GetLeader()` null, or their zone is not loaded here) starts a 60 second wait; the bot then leaves the party and logs `bots: <name> leaves the party: leader is on another map`. Follow returns immediately if the leader pointer is null. `autoCamp` returns false if the leader or their zone is null, instead of reading the zone type off a null zone. `GetChar` / `GetCharByName` / `GetZone` sites that use the pointer already tested it; the leader-away check and `autoCamp` are the ones that did not.

## New columns

Added next to the existing `ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS` lines, so a second start is safe:

| Column | Type | Meaning |
| --- | --- | --- |
| `owner_port` | `smallint unsigned NOT NULL DEFAULT 0` | Map `zoneport` that has this bot. 0 is nobody. |
| `handoff_zone` | `smallint unsigned NOT NULL DEFAULT 0` | Destination zone of an open handoff. 0 is none. |
| `handoff_at` | `int unsigned NOT NULL DEFAULT 0` | Unix seconds the handoff was published. Older than 60s is dropped. |
| `handoff_from` | `smallint unsigned NOT NULL DEFAULT 0` | Port that sent them. `owner_port` is overwritten with the destination, so the arrival line would otherwise not know the source. |

## Single-process guarantee

Freetail live is one `xi_map`. It either passes `--ip 0` (so `GetZonesAssignedToThisProcess(IPP(0, 0))` is the full zone list and this process's zone set covers it) or it is the only `(zoneip, zoneport)` in `zone_settings`. Either way `split_` stays false.

While it is false, every new branch is a no-op or the old code:

- `claimBot` returns true and does not write. `releaseBot` returns. Login, balance, and the camp roll do not look up `bot_pool.zone`.
- `zoneLocal` is true for every zone, so `travel` still sets `requestedZoneChange`, recruits still zone, and `pickPartyCamp` still takes every camp `findCamps` returned.
- `localOnlineCap` is `bots.MAX_ONLINE`.
- `clearStaleState` runs the same two `DELETE`s as before, and nothing else.
- The leader-away block is not entered. `follow` only returns early when the leader pointer is null, which the old callers did not pass.
- Handoff queue, poll, expire, and arrival login do not run.

## Test plan

Two `xi_map` processes, each with its own non-zero `zoneport` (the same port on two IPs is not distinguished). Zone sets are whatever the operator put in `zone_settings`. One real player, so bots log in. Map log at info.

1. Startup, each process. `bots: map <port> serves <n> zones, owns bots in <m> of them` and `bots: split mode: <k> map processes share the pool` with the same `k` on both, and `k` equal to the number of `(zoneip, zoneport)` groups. Ports in the log match the process `--port`. Neither line says it serves every zone.
2. No double login. Let both sit with players online until the caps fill. `SELECT charid, COUNT(*) FROM accounts_sessions s JOIN bot_pool b ON b.charid = s.charid GROUP BY charid HAVING COUNT(*) > 1` is empty. `owner_port` is either 0 or one of the two ports, never a bot online on both. A bot whose saved zone is on map A does not appear on map B.
3. Walk across. Party a bot and zone them to a zone the other process runs (a border they can walk, or a homepoint / outpost in the other set). Within a few seconds the source logs `handed to map <other port> (zone <z>)` and the destination logs `arrived from map <source port> (zone <z>)`. The bot is in that zone on the destination, once. Their `accounts_parties` row is still there if the party was kept, and they are in the party again after the arrival login.
4. Crash. Kill one process. The other's bots stay online (sessions and `owner_port` unchanged for that port). Restart the killed process. It logs split mode again, and it does not log out the survivor's bots. Its own old sessions and claims are gone; those bots log in again only into its zones. A handoff left sitting for more than 60 seconds logs `handoff expired` and that bot is back in the pool (`owner_port` 0, `handoff_zone` 0), not stuck online.
5. One process, unchanged. Stop the second map. Run the way live runs (one process, `--ip 0`, or a `zone_settings` with a single endpoint). There is no `split mode` line. `clearStaleState` still clears every bot session and party row. Bots log in up to `MAX_ONLINE`, including camps and homes anywhere, and a zone change does not log `handed to map`. Caps, parties, and camps match a build from before this job.

## Assumptions

- This process's port is `zone_settings.zoneport` for the zones it serves, and that value is what `enterZone` writes to `accounts_sessions.server_port` (`PZone->GetPort()`). It is also `--port` when the operator set them the same, which is how a zone is assigned to a process. Two processes that share one `zoneport` and differ only by IP cannot be told apart; split mode would treat them as one port. The test server needs a distinct port per process.
- `CZone::config_` is protected. The module cannot read the process `--ip` / `--port`. The port is taken from the zones it loaded. If those zones do not share one non-zero port, split stays off.
- "Every pool bot's zone is local" is not treated as an off switch when `zone_settings` has more than one endpoint. If it were, a process that happened to hold every saved bot at startup would take the single-process path, wipe the other process's sessions in `clearStaleState`, and never hand a later zone change across. Serving every zone (ip 0, or the only endpoint) is what turns the new behaviour off. A live server with one endpoint still stays on the old path.
- `GetZonesAssignedToThisProcess(IPP(0, 0))` returns every `zone_settings` row. Compared with the loaded plus managed set, that is "this process serves every zone".
- Lazy loading: managed zone ids count as local, and their `zoneport` is read from `zone_settings`. Camps still come only from zones that are actually loaded, which is what `findCamps` already did.
- `rowsAffected()` is changed rows. Re-claiming a bot this process already owns is confirmed with a SELECT. `claimArrival` changes `handoff_zone`, so that update still counts when `owner_port` was already the destination.
- `db::preparedStmt` only accepts a string literal (`CheckedQuery` is consteval). Zone reads are two fixed statements (`bot_pool.zone` and `chars.pos_zone`), not a formatted query.
- Party rows have no `server_port`. On a split restart they are deleted for bots this port owns. `PopMember` does not delete `accounts_parties`; `RemoveMember` does, and it runs only when status is `Shutdown`. A handoff never sets `Shutdown`. The in-memory party on the source drops the member; the row stays for `ReloadParty` after the destination inserts a session. `ReloadParty` joins `accounts_sessions`, so the bot is not in the party on the destination until that login.
- A leader who zoned to the other process is gone from this process's member list (`PopMember` nulls `m_PLeader` and does not assign a new one). That is the same signal as "leader not here". The bot waits 60 seconds, and leaves only if it is not in a fight. It does not call `travel` in a loop. A same-process zone takes a few seconds, under that wait.
- The 60 second handoff expiry and the 3 second poll use wall-clock unix time in `handoff_at`. Two processes can both see an expired row; the update is conditional, and only the one that changes a row logs it.
- An arrival that fails `enterZone` releases the claim. The position is already the destination zone, so the process that serves it can log them in on a later pass. They will not retry the handoff row, because claiming an arrival clears it.
- `BOTS_PER_PLAYER_ZONE` is not divided. Splitting it again would double-shrink a cap that is already local.
- Pre-upgrade party rows (`owner_port` 0, no session) are deleted on a split start. Party rows for a bot who still has a session, or whose `owner_port` is the other process, are not.
- `GetChar` / `GetCharByName` return null for a character on another process. Invites, chat, and orders already ignore that null. Recruiting only walks `online_`, so it never invites a bot loaded on the other map.
- `autoCamp`'s new null return only happens when the leader has no zone. The call site is the bot itself while it is being ticked; with a zone loaded, the camp decision is the same as before.
