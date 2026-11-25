# Job U notes

Could not compile here (server headers are not in this folder). Fixes are limited to calls, types, and members already used in this module.

## 1. A gather trip outlives the character

**`bot_ai.cpp` `think`, `finishGather`, `onGatherTrip`, `startGather`. `bot_module.cpp` `logout`, `balance`, `rotate`, `canEndSession`. `bot_gear.cpp` `restock`.**

`Bot::gather` sits on the bot, not on the character. `travel` (balance hauling a free bot to a player zone) sets `Disappear` and the next `enterZone` replaces `session->PChar`. The trip was still set, so the new character walked up to `pointId` in the new zone and could send `0x036` at whatever NPC now had that id. A trade already in flight (`gather->waiting`, or `isInEvent`) is worse: `logout` and `removeCharFromZone` ran while the cutscene still held the character. That is the gather trade the job names, interrupted by a zone or by `LOGOUT_GRACE`.

Likely whenever `GATHERING` is on. Balance runs every 30s and will move a solo bot who is not under `SOLO_BELOW`. Grace logout runs once a second once no real player is left, and it used to take a bot who was mid-trade.

**Fix.** `think` drops the trip with `stops gathering: zoning` as soon as the zone pointer is null or status is `Disappear`, and does not send anything on that path. Death closes a gather cutscene with `finishEvent` while the body is still in the zone, then `stops gathering: died`, and only later sends the homepoint. A trade that never opens an event, and a trip past five minutes, are ended at the top of `think`, including while the bot is idle, so grace is not stuck behind a trip that `onGatherTrip` never reaches. `logout` (grace, rotate, session) and `balance` skip a bot who `isInEvent` or has `gather->waiting`. The bag lookup that Job R added is checked before use; a missing inventory stops the trip (`no inventory`) or refuses to start it, instead of dereferencing null. `onGatherTrip` returns without walking when `PAI` or `PathFind` is null. `finishEvent` returns when `currentEvent` is null.

A failed zone-in still force-logs the bot out. Waiting would call `LoadChar` again every tick.

## 2. A head-out and a camp stay after logout, and a stale camp blocks the next head-out

**`bot_ai.cpp` `outings`, `joinOuting`, `startHeadOut`. `bot_pull.cpp` `pulls`, `breakCamp`, `dropCampIfLeft`. `bot_module.cpp` `logout`.**

`outings` is erased only from `endHeadOut`, which runs in `think`. A leader who logs out mid-trip (grace, or a command) never reaches it. The entry stays for the life of the process. The next time that character leads a party, `joinOuting` copies the old camp onto members and they zone. Same shape for `pulls`: `breakCamp` is the only erase, and nothing called it on logout. After a relog, `hasPull` is still true, `autoCamp` will not make a new camp, and `startHeadOut` returned immediately. A party that wiped to town and gave up the walk back had the same stick: the pull's zone was the field, `hasPull` stayed true, and they never looked for a camp again.

Likely on any long run with `ACTIVE_EVERYWHERE` or bot parties. One entry per abandoned head-out or camp, and a stuck town party after the first wipe that does not walk back.

**Fix.** `logout` calls `breakCamp` for that character id and `dropOuting`. `joinOuting` null-checks `GetLeader` before using it, and erases an outing whose time has passed. `startHeadOut`, once it has decided this party is in a non-hunting zone and is not already walking, calls `dropCampIfLeft`: a pull whose zone is not the leader's current zone is dropped, then the head-out runs. A pull for the zone they are standing in (a "camp here" in town) is left alone. A party with `returnUntil` set is not touched, so a wipe that is still walking back keeps its camp.

## 3. Head-out follows a leader who is zoning

**`bot_ai.cpp` `think` (the return-camp block and the walk-to-spot block).**

Both blocks called `PLead->getZone()` and, for a member who had caught up, `PFollow->getZone()`, with no check that `loc.zone` was set. `follow` itself bails out when the leader's zone is null, but the condition ran first. Other code in this folder (`campZoneText`) checks `loc.zone` before `getZone`. A leader who has already called `travel` is `Disappear` with a zone pointer that may already be null while a member's `think` still runs.

Likely while a party is heading out or heading back and the leader zones a tick ahead of the members.

**Fix.** Those conditions require `loc.zone != nullptr` before `getZone`. The idle return and the two "nowhere to follow" branches clear `PathFind` only when `PAI` and `PathFind` are both set. `sendAction`'s new resting check does the same for `StatusEffectContainer`.

## Assumptions

- `CCharEntity::getZone` is not safe when `loc.zone` is null. The new checks match `campZoneText`. If `getZone` is null-safe, the extra test only skips a follow that `follow` would have skipped anyway.
- `getStorage(LOC_INVENTORY)` can return null around a zone change. When it does not, `freeSlots` is the old count and gather starts and stops on the same "fewer than 3" rule.
- `StatusEffectContainer`, `PAI`, and `PathFind` are normally present on a character in the world. The new checks only change the crash path.
- `isInEvent()` can be true while `currentEvent` is null. If that never happens, the new return does not run.
- `GP_CLI_COMMAND_ITEM_TRANSFER::PropertyItemIndexTbl` and `ItemNumTbl` are at least 10 entries. The loop was already there; it was not resized. See below.
- `charutils::hasPetAbility(PChar, id - ABILITY_HEALING_RUBY)` matches `addPetAbility`, as Job N assumed. Not re-checked; the server headers are not here.
- `breakCamp` on logout only removes a `pulls` entry keyed by that character. A member who was never the leader does not clear someone else's camp. The leader logging out does, including a second or two before grace logs the rest of the party out. They can camp again if they are still online.
- A bot in a cutscene is logged out on a later tick, once `think` has ended the event. Grace still logs out everyone else. A failed zone-in does not wait.

## Test plan

`ACTIVE_EVERYWHERE` on, `GATHERING` on, `GATHER_CHANCE` 100, `SOLO_BELOW` 10, one real player. Map log at info.

1. Gather. Put a solo bot next to a Logging or Harvesting Point with nothing to hunt. After `goes gathering` and at least one `gathered at`, zone it (`balance` will, if you leave the zone and the bot is over `SOLO_BELOW`, or `!zone` it). One line: `stops gathering: zoning`. No `0x036` and no `gathered at` in the destination. No crash, and the bot is not left `Disappear`.
2. Kill the bot during a gather cutscene (the moment after a trade, before `gathered at`). `stops gathering: died`, then the usual homepoint. The map does not sit in the cutscene, and the next life does not `goes gathering` at the homepoint.
3. Start a trip and let the last real player log out. During the trade window the bot is not in the logout line yet. Within about a second of the event ending, grace logs it out. No `removeCharFromZone` while the client would still be in the event. A bot who is only walking to the point, not in a cutscene, can log out immediately.
4. Town party. Let a bot-only party `heads out`, then log the leader out before they arrive (`LOGOUT_GRACE`, or log that character out). Members already walking keep their own copy. A new party led by that same character does not zone to the old camp, and there is a fresh `heads out` or `no camp to head to`, not a silent resume.
5. Camp, then wipe them to town and let the ten minute return expire (or skip the return). The next tick in town is `heads out to`, not a party that never leaves because `hasPull` is still the field camp. A party told "camp here" in town does not get that camp cleared and does not `heads out` while that pull's zone is the town.
6. With `GATHERING` off and `CITIZENS` 0, a normal camp, pull, and head-out still log `camps at`, `is going for`, `pulled`, and `heads out to`. `Invalid GP_CLI_COMMAND_CAMP` / `Engaged` stays gone (Job S). No new per-tick log line.

## Suspicious, not changed

- **`findCamps` on one tick.** `seekPrey` and `startHeadOut` each throttle per bot (2 min and 5 min), but every solo bot and every town leader who comes due together walks every field mob. The tick watchdog is 2s (`bot_module.cpp`). A cache would change which camp is chosen. Left as is.
- **`bot_citizen.cpp` `routines`.** Not erased on logout. `noteCitizenLogin` resets the entry on the next login. One row per citizen id that has ever logged in, not per tick. `recentSays` is pruned after a minute.
- **`enterZone` sets `Bot::home` on every zone**, not only login. A citizen's "home" stop becomes the last zone-in spot, including a homepoint. Not a crash. Left as is.
- **Trade packet.** `onGatherTrip` writes `ItemNumTbl` / `PropertyItemIndexTbl` for `i` in `0..9`. The packet header is not in this tree, so the array length was not verified. A shorter array would smash the packet.
- **`pickTarget`'s nested `ForEachMob`.** Job O added `takenByPuller` inside a loop that was already nested. Left as is; it only reads.
- **`knowsAbility`.** Still `hasPetAbility(PChar, id - ABILITY_HEALING_RUBY)`. `brainOutcomes` already drops a bot whose `entity()` is null before `canDo`. If the offset is wrong, the old `hasBit` log can come back. Not changed.
- **Job S.** `rest` / `restInFight` still wait while `GetBattleTarget()` is non-null. The check does not dereference it. If a despawned mob left a dangling non-null pointer, they wait instead of healing. That is the assumption Job S wrote down.
- **HELM visibility.** `helmVisible` is status `Normal` only. Job R dropped the hidden-name test on purpose.
- **Chat and brain workers.** `citizenReplyAllowed`, `citizenReplyUsed`, and `sayAddressedToOther` run from `handleChat` on the map thread. The worker threads only do HTTP. No new shared state there.
