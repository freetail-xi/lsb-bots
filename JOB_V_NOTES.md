# Job V notes

Could not compile here (server headers are not in this folder). New calls are limited to functions, types, and members this module already uses, except the C++ map node handle called out below.

`PARTY_LEVEL_SPREAD` (3) and `PARTY_RECRUIT_MINUTES` (20) are in `settings/bots.lua.example`. Invites read the spread. The recruit timer and the leader cooldown after a disband read the minutes. `LEVEL_WINDOW` is still only login placement and `balance`.

## What changed

**Recruiting.** `formParties` no longer grabs whoever is standing within 40 yalms. A tank leads if one is in the zone, otherwise the highest level. That bot gets a recruit row: their level is the centre of the band, and where they stand is the gathering spot (a town is fine). One invite every 4 seconds, in order: a free bot in that zone, then an idle bot in another zone, then a seeking player through `askSeekers`. A candidate fits when they are within the spread of the leader's level at the start and the party's highest minus lowest would still be at most twice the spread. Citizens, bots under `SOLO_BELOW`, and bots with a real player are left out.

Slots fill tank, then a healer from level 15 (`kHealerLevel`), then one support (RDM, BRD, COR, GEO; a red mage already taken as the healer does not count), then damage. Tanks use `tankScore`, healers use `healerRank`, supports and damage dealers use `pullerRank`.

While the row says they are still gathering, members stay at the spot. They can rest. They do not hunt, wander, or head out alone. A bot who accepted in another zone is zoned with `travel` to a spot near the leader, then walks the last part. Only an idle bot is invited across zones (not fighting, not resting, not in an event or a gather trip, not already heading out).

The leader shouts on `CHAT_SHOUT_MINUTES`, and only when a real player is in the zone, the same check an unpartied LFP shout uses. The line is the band and the open slots, for example `LFM 18-24, need WHM + 2 DD`.

Still short, and no camp picked yet, after `PARTY_RECRUIT_MINUTES`: one log, the leader breaks the party up, and that leader waits the same number of minutes before starting another. `0` never disbands. A party that has already picked a camp does not disband for being short; it recruits from the camp.

**Leaving.** A party heads out at 6 members. With a real player in it, it also heads out early when they say "head out", "lets head out", "let's head out", "go to camp", "lets go camp", "let's go camp", or "ready" in party chat. "let's go" is still Follow. The camp is `findCamps` at the average level of everyone in the party, skipping a spot another party is already camped on, preferring this zone, then a neighbouring field, then the zone with the most open camps. The existing head-out walk runs. A pull left over from a zone they have left is dropped first, so a wipe that ended in town does not stick them there.

**At camp.** The party keeps fighting while it is short and invites replacements under the same band. New bots travel to the camp. When the average is 3 or more above the average the camp was picked at, and nobody is in a fight, it picks another camp and heads out. A bot who has pulled the other bots' levels more than twice the spread apart leaves, says "out of the level range" in party, and goes back to the pool. A real player is never removed. A party that levels together stays together and moves camp instead.

**Logs, one line each.** `bots: <leader> recruiting (band 18-24) in zone <z>`, `bots: <name> joins <leader>'s party (from zone <z>)`, `bots: <leader>'s party is full, heading to camp in zone <z> (avg 21)`, `bots: <leader>'s party disbands: short for 20 minutes`, `bots: <name> leaves <leader>'s party: out of band`. A player sending them early logs `heads to camp` instead of `is full`. A camp that the average has outgrown logs `heads to a new camp`.

## Test plan

`BOT_PARTIES` on, `INVITE_SEEKERS` on, `PARTY_LEVEL_SPREAD` 3, `PARTY_RECRUIT_MINUTES` 20, `SOLO_BELOW` 10, `CHAT_SHOUT_MINUTES` 10. One real player in a city. Map log at info.

1. Party to six, inside the band. Put several unpartied bots of mixed jobs in that city, levels within 3 of each other, and at least one tank and one healer at 15 or above. Within a few minutes: `recruiting (band A-B) in zone <city>`, then one invite every few seconds, tank then healer then a support then damage. No bot more than 3 from the leader's start level, and the party's highest minus lowest never over 6. At 6: `is full, heading to camp in zone <z> (avg N)`. They are not hunting in town before that. A bot under `SOLO_BELOW` is not invited.
2. A bot from another zone. Leave one idle bot, in band, in a different zone, not fighting or resting. After the same-zone bots are used up: `joins <leader>'s party (from zone <old>)`, then they zone to the leader and walk in. A bot who is in a fight, resting, gathering, or already heading out is not the one who comes.
3. The camp fits the average. The zone in the head-out line is a field, and the mobs there are the ones `findCamps` returns for that average (about the party's level, up to 2 above). A second party does not pick a spot within 40 yalms of the first party's camp. With the player standing in the city, the shout, if the timer is due, is `LFM A-B, need ...` and it does not fire in a zone with only bots.
4. "head out" leaves early. Join the party at 3 or 4. Say "head out" (or "ready", "go to camp"). They leave before 6: `heads to camp in zone <z> (avg N)`. "let's go" still follows and does not pick a camp. A bot saying "ready" to the party does not.
5. Refill at camp. At the camp, have one bot leave or log out. The party keeps fighting. A replacement in band is invited; if they were in another zone they travel to the camp. One line: `joins <leader>'s party (from zone <z>)`. There is no `disbands: short for 20 minutes` from the camp.
6. Also worth one pass. A town party left at 4 for 20 minutes: `disbands: short for 20 minutes`, and that same leader does not immediately start another. A bot who is 7 levels above the other bots, between fights: `leaves <leader>'s party: out of band`, and the player is not kicked if they are the one outside the band. After the whole party has gained about 3 levels: `heads to a new camp`, and only once the current fight is over.

## Assumptions

- `CParty::members` is the player and bot members. Trusts are not in that list (`withTrusts` adds them afterwards), so the average and the band do not count trusts. `IsFull()` is the party of 6; the code also treats `members.size() >= 6` as full.
- `GetLeader()` can be cast to `CCharEntity` when the leader is a player or a bot. The module already does that. Only the party leader can invite, so the recruit row is kept on a bot who leads. If a real player takes the lead, the "head out" flag is stored and no bot starts the walk until a bot leads again.
- A cross-zone invite passes targid 0, which this module already did when the target was in another zone. Assumed the server still delivers it and the bot's accept still runs.
- `travel` zones to `loc.destination` at `loc.p`. The new call only passes a zone and a `spotNear` position. What the zone engine does with `requestedZoneChange` was not re-read.
- `findCamps(level)` is unchanged: normal mobs with minimum level at least `level - 1` and maximum level at most `level + 2`. "A few levels above" the average is that window. Mob tables were not checked.
- There is no distance between zones in this module. Neighbours are a fixed list of retail zone ids (Sandy 230-233 to 100, 101, 102, 140; Bastok 234-237 to Gustaberg and Zeruhn; Windurst 238-242 to Sarutabaruta; Jeuno 243-246 to 105, 110, 119, 120). `data/enums/zone.h` is not here, so those numbers were not checked against the enum. Anything not on the list is still eligible; it just loses the "nearby" preference.
- Another party's camp blocks a new pick when `pulls` has that leader, same zone, within 40 yalms. `pulls` is keyed by leader id, as `breakCamp` already assumes. 40 is the old form range, not `kCampDrift`.
- `zoneutils::GetZone` and `GetChar` behave as the existing camp and invite code expects. A join logged before `joinedFrom` is set (a party that already existed, or a relog) uses the bot's current zone. `joinedFrom` and `notedParty` are not saved.
- `isPoolChar` is every bot and no real player. The out-of-band check uses it so a real player's level does not push a bot out. Invites still use every member's level, players included, so a player sitting outside the span stops further invites until that span shrinks.
- The band centre does not move after the party starts. Invites stay inside the original window even after the party has leveled and moved camp. That is the formula in the job. Replacements after a long camp can fail the centre check even when they are close to the party's current levels.
- The disband timer is only for a party that has never picked a camp. Once `campAvg` is set, being short recruits from the camp instead of breaking up. The cooldown after a disband is `PARTY_RECRUIT_MINUTES`, as the settings comment says.
- `std::map::extract`, `insert_return_type` (`position`, `inserted`, `node`), and a mutable `node.key()` are available. Nothing else in this folder uses them. They are C++17; the file already uses C++20 (`std::erase`, `ranges`). If a rekey hits a leader who already has a row, the old row is put back on its previous key.
- Breaking the party up is the existing `GP_CLI_COMMAND_GROUP_BREAKUP` from the leader. Assumed the server then drops every member back to unpartied.
- "let's go camp" is handled as head-out as well as the job's "lets go camp", so the apostrophe form does not match Follow ("let's go"). "ready" is a whole word; a bot saying it does not send the party.
