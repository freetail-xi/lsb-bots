# Job J notes

Could not compile here (server headers are not in this folder). No new setting and no schema change.

A bot-only party that dies at a camp homepoints, then the leader zones back to that camp and the others follow. A party with a real player still waits on a raise and, if it homepoints, stays in town.

## What changed

**`bots.h` `Bot`.** Two fields:

- `returnCamp` — a `Place` (zone id and position), copied off the Pull. Empty when this bot is not walking back.
- `returnUntil` — when the walk ends. Unset (`time_point{}`) means not regrouping. Set means regrouping, including while `PParty` is missing for a zone tick.

**`bot_pull.cpp` `campPlace`.** Returns that copy, or empty. It reads `Pull::zone` and `Pull::camp` and returns a `Place` by value. Nothing keeps a `Pull*`.

**`bot_ai.cpp` `think`, with `partyIsBots`, `rememberCamp`, `startReturn`, `campZoneText`.**

On a death tick, `rememberCamp` stores `campPlace` when `partyIsBots` is true and this bot does not already have a camp or a walk in progress.

The raise wait is the same as before: `kRaiseWait` (3 minutes) when `partyCanRaise` or `raiserNearby`, otherwise `kHomepointDelay` (60 seconds). Accepting a raise still sends `RaiseMenu`. Coming back alive without having homepointed still writes `got raised`, and it drops `returnCamp` when `returnUntil` is unset.

On the homepoint tick, `think` still writes `homepointed` and still sends `HomepointMenu`. `startReturn` then, for a bot-only party that has a camp, sets `returnUntil` to this tick plus 10 minutes (`kReturnCap`), writes one diary line per member, and logs `bots: <name> is heading back to <zone>`. The line is `wiped at <zone>, heading back`. `<zone>` is `prettyName` of the camp zone. A second homepoint does not move `returnUntil`.

Once the bot is alive again, `think` drops both fields when any of these is true: the bot's zone is the camp zone, the tick is at or past `returnUntil`, `withPlayer` is set, or `returnCamp` is empty. The cap log is `bots: <name> stopped heading back to <zone>`. After the drop, the idle cut and the rest of `think` run as they did before.

Until that drop, the idle cut does not apply. The party leader calls `travel` to the copied zone and position. Any other member calls `follow` on the party leader when that leader is alive and either already in the camp zone or already in this bot's zone. Otherwise the member stands and waits, still exempt from the idle cut. `follow` is what zones a member to the leader.

**`bot_module.cpp` `isFree`.** False while `returnUntil` is set, as well as when `PParty` is set. `canEndSession` uses `isFree`, so a session does not log a regrouping bot out. `rotate` and `balance` use the same helper, so they leave that bot in place too.

## Assumptions

- A bot-only party is `PParty` set, `withPlayer` false, and `hasRealPlayer` false. `withPlayer` stays true for the 20 second grace after a real player leaves, and it stays true for the whole time the bot is dead, because the grace is handled on a living tick. That party keeps the raise wait and does not walk back.
- A dead real player still counts as a real player. The bots wait for a raise and homepoint without walking back.
- The camp is the Pull for this party leader: its zone id and its position, as a `Place`. `campSpot` is not the source. `campSpot` is empty when the leader is more than `kCampDrift` (20) from the flag, or the leader's zone differs, so a wipe off the flag would otherwise forget the camp. The leader can be dead. The bot taking the copy has to be in the Pull's zone, and that zone is not a city. A city Pull is ignored. A party with no Pull (a duo, or a party that never camped) homepoints and stays there.
- The copy is taken on death ticks, while the bot is still in that zone, which is at least one `kHomepointDelay` before the homepoint menu. Each bot stores its own `Place`. `breakCamp` is not called on a wipe, so the Pull is still there for the copy and is still there when they get back. A follow order is still what clears it.
- If a member's death ticks never see the Pull (the leader is already off the party, so `pullFor` cannot find it), that member does not walk back.
- The 10 minutes start when the homepoint menu is sent, on the same clock as the other `think` delays. Time spent dead before that menu does not count. Time spent zoning does, because `returnUntil` is an absolute time. The cap is checked on a living tick. A bot stuck in `Disappear` is checked on the next tick that actually runs `think`.
- "Back" is this bot's current zone equaling the copied zone. The leader's `travel` is what puts the leader on the copied coordinates, and only when the leader is in some other zone. A homepoint that is already inside the camp zone counts as back and does not walk to the flag. Home points are nation cities, so a normal wipe does not hit that case.
- Members follow the party leader, not `followId`. They do not follow a leader who `isDead()`. `follow` also refuses a leader whose status is not `Normal`, which covers zoning. A member does not follow the leader into the leader's home nation: that leader's zone is neither the camp nor the member's zone, so the member waits. Two bots who share a home nation do follow inside that city until the leader leaves it.
- `stay` does not hold a bot in town during the walk. The return runs before the normal follow path and returns. `stay` is left as it was.
- A cutscene tick finishes the event and does not travel. The next living tick does.
- Clearing the memory does not clear the Pull. In a camp zone that has a real player, today's camp and pull logic runs again once the memory is gone. In a camp zone with no real player, the idle cut stands them on the spot they just reached. That is today's idle rule.
- `withPlayer` becoming true mid-walk drops the memory on that tick. `active` was computed before `think`, so that one tick can still take the idle return. The next tick treats them as a player's party.
- `isFree` is what session logout, `rotate`, and `balance` share. A regrouping bot fails it even when `PParty` is null. When no real players are left (`target == 0`), the over-cap logout still logs every bot out, regrouping included. That path does not use `isFree` alone.
- If `PParty` is null on a living tick, the leader does not `travel` that tick. The bot waits, still exempt, until the party pointer is back or the cap hits. Homepoint does not remove a party; this is the zoning gap.
- The diary name uses `prettyName` of `loc.zone->getName()` when the bot is standing in the camp zone, which is the homepoint tick. `zoneutils::GetZone` supplies the name only when those differ. If neither yields a name, the line says `somewhere`.
- Both `homepointed` and `wiped at <zone>, heading back` are written, in that order, so the wiped line is the newest. A raise writes `got raised` and does not write the wiped line.
- The walk is not stored in `bot_pool`. A map restart drops it. The next login does not resume it.
- `!bots` shows the zone. It does not show `returnCamp`. The map log lines above are the check that the walk started or hit the cap.

## How to test

Rebuild and restart the map.

1. Three bots, no real player in the party, camped in a field you are standing in. `!bot <leader>` should show a camp. Pull something that kills them. If someone alive can raise, the homepoint is 3 minutes; if everyone is dead, it is 60 seconds.
2. The log should show, for each member, `diary: homepointed` and then `diary: wiped at <zone>, heading back`, plus `is heading back to <zone>`. The zone words should match the camp, with spaces rather than underscores.
3. `!bots` should show the home cities, then the leader in the camp zone, then the members in that zone, on the camp spot. Stay in the zone and they should camp again: the Pull was not cleared. Zone out before they arrive and they should still come back, then stand idle once the log has no further heading-back line and the memory has cleared.
4. Raise the same kind of party before the homepoint. The log should show `got raised` and should not show `wiped at` or `heading back`. They stay at the fight.
5. Join the party, then wipe. They should take your raise. If you do not raise them, they wait out the 3 minutes when you can raise, then homepoint. The log should show `homepointed` and should not show `heading back`. They stay in their home cities.
6. A party with no camp (two bots, or a party whose `!bot` has no camp). Wipe them. `homepointed` only. They stay home.
7. Set `SESSION_MIN` and `SESSION_MAX` to 1 and restart. Wipe a camped bot party so the session time falls during the walk back. The log should not show `session is over` while it still says they are heading back. `!rotatebots` during that walk should not take them. After they are back, pull one out of the party. A due session, or `!rotatebots`, can log that one out again.
8. After the wiped line, ask one what happened. The reply can mention the wipe and that camp zone.
