# Job O notes

Could not compile here (server headers are not in this folder).

## 1. Parties of 2 camp

**`bot_pull.cpp` `autoCamp`.**

`autoCamp` returned false when `PParty->members.size() < 3`. The 500-bot run formed 32 parties of 2, and none of those camped, so none of them pulled. `toughestGap` already treats a party under 3 as an even match (gap 2). The size check is now `< 2`. `withPlayer` still refuses, so a party with a real player does not camp itself.

## 2. A bot-only party in town heads out

**`bots.h` `Bot::headingOut`, `Bot::nextHeadOut`. `bot_pull.cpp` `hasPull`. `bot_ai.cpp` `startHeadOut`, `joinOuting`, `endHeadOut`, and the return block in `think`.**

`autoCamp` only camps in an outdoor zone or a dungeon, and only when the zone is not a city. Parties formed in zones 230-246 stayed there. Nothing sent them to a field.

`startHeadOut` runs for the leader of a bot-only party (`partyIsBots`, which already requires `!withPlayer`) when `returnUntil` is unset, the zone is one `autoCamp` would refuse, and `hasPull` is false. It calls `findCamps` with the party's highest level and picks one at random. That place is stored on the leader as `returnCamp` / `returnUntil` (capped at the same `kReturnCap` as a wipe return) with `headingOut` set, and in `outings` under the leader's id. The log line is `bots: <leader>'s party heads out to <zone> (level N)`.

`joinOuting` copies that onto each other member, so the existing return block zones the leader (`travel`) and the members follow. Once the leader is in the zone but still more than 30 yalms from the spot, `think` walks the leader there (`walkTo`) and the members keep following. The trip ends for the leader only inside that 30 yalms. A wipe return (`headingOut` clear) still ends on entering the zone, and its log line is still `stopped heading back to`.

`nextHeadOut` is set to 5 minutes when the leader looks, including when `findCamps` is empty. The empty log, once per that window, is `bots: <leader>'s party has no camp to head to (level N)`.

## 3. Shorter pulls, and no racing

**`bot_pull.cpp` `kPullLimit`, `takenByPuller`, `pickTarget`.**

`kPullLimit` was 180. A pull that far is usually dead before the puller arrives ("target gone", and "couldn't get to it in time"). It is now 100. `kPullNear` is still 50. The "target wandered off" check still uses `kPullLimit + 10`.

`pickTarget` skips a mob whose id is `Pull.mob` on another party's Pull while that Pull is outbound or bringing it back (`stage != Waiting`), in the same zone. Two parties no longer both log `is going for` on the same unclaimed mob.

## 4. No actions while sitting

**`bot_ai.cpp` `sendAction`.**

Casts, job abilities, ranged shots and weapon skills are rejected while the character is in the sit animation. The validator's text is `Character in invalid animation state`. Attack is rejected as sitting. `sendAction` already dropped those action ids while `StatusEffect::Healing` is on, and left standing up to `rest`, `restInFight`, and the fight branch in `think`.

`/sit` does not add that status. `wander` sets `bot.sitting` and sends the sit packet. A party leader has no `PLeader` from `leaderOf`, and with no battle target the stand at the top of `think` does not run, so `act` cast and used abilities while the bot was still down. Chokuwochukowu and Xavier match that: sitting in town, sending actions every tick.

For attack, cast, job ability, shoot and weapon skill, `sendAction` now calls `setSitting(false)` when `bot.sitting` is set, then sends the action. The sit packet is dispatched first. While the Healing status is on, the action is still not sent. Standing up inside `sendAction` on every heal would end the rest: `think` calls `act` before `rest`, and `act` tries to cast.

## Assumptions

- `hasPull` is `pullFor != nullptr`, the same test `autoCamp` already uses to mean the party has a Pull.
- `notHunting` is the `autoCamp` zone test inverted: missing the outdoors or dungeon bit, or having the city bit. A null zone is left alone.
- The level passed to `findCamps` is the highest `GetMLevel()` in the party. For a bot-only party that is what `levelOf` returns. It is passed as `uint8`.
- `findCamps` returns field spots for that level. One of them, chosen with `xirand::GetRandomNumber`, is a camp the party can walk to. The place's position is the mob spawn.
- `CMobEntity::getZone()` matches `Pull.zone`. `EntityId` equality is the same identity `pickTarget` already uses for `gaveUpOn`.
- An active pull target is `Pull.mob` while `stage` is not `Waiting`. That covers going out and bringing the mob back. `nearestAny` (the "pull anyway" fallback) is unchanged and can still choose a mob another puller has.
- `packets.dispatch` runs the sit handler before `send` returns, and sit-off sets the animation to standing. The action packet is validated after that. This is the same stand-then-act order `think` already uses when `bot.sitting` is cleared for a target or a leader.
- `PChar->animation` is not read anywhere in this folder. The sit flag this code keeps is `bot.sitting`, set by `setSitting` in the same call as the sit packet. Chair sits are not sent by this code.
- There is already a way to stand a bot up: `setResting(false)` for `/heal`, and `setSitting(false)` for `/sit`. `/heal` keeps the old "skip the action" path so a bot who is down to rest is not stood up by `act`.
- Head-out uses `kReturnCap` (10 minutes) as the walk limit. The wipe path is the same block with `headingOut` clear, so "in the zone" is still enough to finish a wipe return.
- Members copy the leader's camp onto their own `returnCamp` / `returnUntil`. That is what makes the existing return block zone them and follow. A member stays on that path until the leader's outing is gone and they are next to the leader, or until they are within 30 yalms of the spot. A quiet destination zone does not idle them at the zone line (`returnUntil` still exempts them).
- `withPlayer` still makes `autoCamp` and `startHeadOut` return immediately. `kPullLimit`, the taken-target skip, and the sit stand-up apply to every bot, including one in a real player's party.

## Test plan

`bots.ACTIVE_EVERYWHERE = true`, same 500-bot setup. Map log at info.

1. Town parties leave. For leaders who formed in a city (zones 230-246 and any other non-hunting zone): `bots: <leader>'s party heads out to <zone> (level N)`. One line per leader per 5 minutes. If a level has no field camp: `bots: <leader>'s party has no camp to head to (level N)`, and not again for 5 minutes. A wipe return is still `bots: <name> is heading back to <zone>` and, on giving up, `stopped heading back to`.
2. After that line, the same leader's party camps and pulls. `bots: <name> is pulling for <leader>'s party`, then `bots: <name> is going for <mob> (level ..., N yalms from camp, ...)`, then `bots: <name> pulled <mob>`. Many more leaders than the 5 from the last run. Parties of 2 are in that set.
3. The `N` in `is going for` is under 100. `target wandered off` still uses that same limit. `pull by <name> ended: target gone` is rarer than 32 in 15 minutes, and `couldn't get to it in time` rarer than 13. Two parties do not both `is going for` the same mob.
4. `Invalid GP_CLI_COMMAND_ACTION` / `Character in invalid animation state` drops for bots who sit in town (Chokuwochukowu, Xavier, and the others). A bot who is `/heal` resting still does not send the attack, cast, ability, shot or weapon skill until `rest` or the fight path stands them up.
5. With `ACTIVE_EVERYWHERE` false and a real player in the party: no `heads out to` line, and `autoCamp` still does not camp that party. Pull distance, the taken-target skip, and standing up from `/sit` still apply.
