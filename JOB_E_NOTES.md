# Job E notes

Could not compile here (server headers are not in this folder). Everything below uses functions, types, and members that already appear in this module. Assumptions are marked.

The test server reads `settings/bots.lua`, not the example. Copy the two new keys into that file before testing. Both are read with `settings::get<uint32>` (this tree never calls `settings::get<float>`).

## Settings

In `settings/bots.lua.example`:

- `bots.PLAYER_LEEWAY` (default 40). Yalms. Bots leave a real player's fight, claim, and nearby mobs alone, and `balance` will not place or send a bot onto a spot inside that distance.
- `bots.LEVEL_WINDOW` (default 10). Levels. `balance` will not send a bot further than this from the player it is balancing for. Invites use the same gap. `loginNext` prefers a pool level inside it.

"Within" uses the same `distance()` as the rest of the module, and a strict less-than (`distance < leeway`), matching the other "within N yalms" checks (`distance < 15` and so on). A level gap is allowed when it is less than or equal to the window ("further than" the window is skipped).

## 1. Leave real players' mobs alone

**`bot_world.cpp` `leaveForPlayers`.** A mob is skipped when a real player (not `isPoolChar`, not in a Mog House) is fighting it or has claimed it, or when the mob is within `PLAYER_LEEWAY` of a real player who is not in this character's party. Another pool bot standing nearby does not count.

**`bot_world.cpp` `nearRealPlayer`.** True when any real player in that zone is within the given leeway of a point. If the zone cannot be looked up, it returns true so `balance` will not use an unchecked spot.

**`bot_ai.cpp` `huntTarget`.** After the existing "dead / not spawned / engaged / claimed / not normal" filters, a mob `leaveForPlayers` rejects is not chosen.

**`bot_pull.cpp` `unpullable`.** Same check, reason `"beside a player"`. `pickTarget` calls `pullable`, which calls `unpullable`, so the pull choice goes through it. `pullBlocker` will say so ("N beside a player") when that is why nothing can be pulled.

**`bot_pull.cpp` `loosePullable`.** The "pull anyway" fallback (`nearestAny`, used only when `pickTarget` found nothing and the leader forced a pull) uses the same check. Without it, a forced pull would take the mob `pickTarget` had just refused.

**`bot_module.cpp` `balance`.** The camp spot is still `spotNear` around the highest-level real player's position. The inner radius is `max(25, PLAYER_LEEWAY)` and the outer radius stays at least 70 (so the default 40 keeps a 40–70 ring; the old ring was 25–70). Up to four tries. A point that is still inside the leeway of any real player is not used, including `spotNear`'s fallback onto the player when no navmesh point turns up. That zone is left alone for this pass (the next `balance`, 30 seconds later, tries again).

Self-defence was not changed. `combatTarget` and the idle return in `think` still fight a mob that is already on the bot. `beingPulled` was not changed.

### Assumptions

- "Fighting" means that player is engaged and their battle target is the mob, or the mob is engaged and its battle target is that player. "Claimed" means `m_OwnerID.isSet()` and `m_OwnerID.UniqueNo` equals that character's id (the same comparison `bot_pull.cpp` already uses for party claim). A player who has only targeted the mob, or who is on the hate list without a claim and without either of those battle targets, is not treated as fighting. Standing within the leeway still protects the mob.
- "Own party" is the same `PParty` pointer. An alliance member in another party counts as someone else, so their nearby mobs are left alone.
- Engaged or claimed mobs were already skipped for everyone, including other bots. That is unchanged. The new check is the real-player fight/claim plus the leeway.
- The check runs when the target is chosen. A hunt target already stored on `focusTarget`, or a pull already outbound, is not dropped if a player walks up during the approach.
- Mog House characters are ignored, same as `playerZones`. A zone with no navmesh point outside the leeway gets no `balance` placement (previously `spotNear` dropped the bot on the player).

### How to test

1. Put `PLAYER_LEEWAY = 40` in the test server's `settings/bots.lua` and restart the map.
2. Two clients in a field zone (Valkurm is the one the design calls out). One player pulls. On the other, `!bot <name>` the bot party's puller (`pullStatus` names the mob it is going for) and watch the camp.
3. The party should not open on the mob that player claimed, and should not pull an unclaimed mob standing within 40 yalms of that player. A mob on the far side of the zone should still be pulled.
4. Let something aggro a bot (or stand in a zone where a bot is idle and drag a mob onto it). It should still fight back. `!bot` shows "fighting ...".
5. For placement: stand still in a zone that `balance` is filling (`BOTS_PER_PLAYER_ZONE` above 0). Bots that log in or zone in for you should not appear within 40 yalms. If the log says nothing and the zone stays under-filled, the four spot tries all landed inside the leeway; wait another 30 seconds or raise the leeway and try a less crowded spot.
6. Optional: set `PLAYER_LEEWAY` very small and confirm they go back to camping near the player (still at least 25 yalms out, the old inner radius).

## 2. A level window

**`bot_module.cpp` `balance`.** Still picks the closest pool level, then the closest free online bot in an empty zone. Either is sent only when `abs(level - players.level) <= LEVEL_WINDOW`. `players.level` is the highest real player in the zone, which is the player `balance` was already placing around. If the closest candidate is further than the window, that candidate is not sent. A logged-out bot outside the window does not block an already-online bot who is inside it. If neither is inside the window, the zone stays under-filled.

**`bot_module.cpp` `loginNext`.** Removed `kNearLevel` (6). The "near a player's level" preference now uses `LEVEL_WINDOW`. It is still a preference: if nobody in the pool is inside the window, a further bot can still log in, at a camp for its own level or at its pool home. That matches the old `kNearLevel` behaviour. `formParties` is still 5 levels, as the design said to leave it.

**`bot_chat.cpp` `levelNear`, `respond`.** Both `wantsParty` invite sites compare `GetMLevel` before `invites.push_back`.

- Tell (and a say/shout/yell that names the bot): that bot invites only when the sender is within the window.
- Shout/yell that is not aimed at one bot: the first unpartied listener inside the window invites. A bot outside the window does not take the invite and does not block a later listener who is close enough. If every unpartied listener is outside the window, nobody invites.

**`bot_chat.cpp` `describe` and `ask`.** `describe` takes `sentInvite` (default true, so party chat, linkshell, greet, and `speakUp` keep the old sentence). When the level check skips an invite, `ask` is called with `sentInvite` false and the note `"You have not sent them an invite."` The system prompt then says "If someone wants to party, you have not sent an invite." It does not say an invite was sent.

### Assumptions

- The sender's level comes from `zoneutils::GetCharByName(heard.from)`, which this file already uses for the speaker. If that returns null, the gap cannot be seen, so the invite is still queued and the old "you've already sent them an invite" sentence stays. Assumption: a miss means "don't invent a block", not "block them".
- `balance` compares against `PlayerZone::level` (highest real player in the zone), not against every player there. A level 10 and a level 75 in one zone are balanced for the 75.
- `loginNext` will still log a far bot in when no in-window pool row is available. What it will not do is what `balance` used to do: travel or place that far bot into the low-level player's zone.

### How to test

1. Put `LEVEL_WINDOW = 10` in `settings/bots.lua`. Use a pool that has both low levels and 70s, and a level 15 client.
2. `!bots`. The 70s should be offline, or online in some other zone (their own camp or home), not brought into the starter zone by `balance`. A 70 whose pool home is that starter zone can still appear there via `loginNext`'s home placement (30% when `CAMP_CHANCE` is 70). That path was not part of this change. `balance` itself should not `placeAt` or `travel` them onto the level 15; the map log line is `bots: <name> heads to <zone>`.
3. On the level 15, shout `lfp`. A bot more than 10 levels away should not invite. A bot within 10 can. The level 70, if it replies at all, should not claim it sent an invite (the prompt it was given says it did not).
4. Tell `lfp` directly to a level 70 bot. It should answer and not invite. Tell `lfp` to a bot within 10 levels. It should invite, and that prompt still says an invite was sent.
5. Set `LEVEL_WINDOW` to 0 and confirm only an equal level is invited or balanced in. Set it back to 10.

## 3. Shouts name the real zone

**`bot_chat.cpp` `partyShout` and `chatter`.** The unpartied shout no longer calls `speakUp`. It is queued like `tell` and `callout`: a fixed line, sent on the next `deliverChat`, not written by the model. Party small talk and linkshell lines still use `speakUp`.

One of four lines, picked at random. `<job>` is the abbreviation (`WAR`, `MNK`, ... the same list `describe` and `!bots` use), `<level>` is `GetMLevel`, `<zone>` is `loc.zone->getName()` with `_` turned into a space (the same display form as `describe` and `!bots`):

- `lfp 24 war, valkurm dunes`
- `LFP WAR24 @ Valkurm Dunes`
- `24war lf pt in valkurm dunes`
- `lfp war24 valkurm dunes`

A bot who was told to quiet down does not shout until that timer ends (`quietUntil`, the same one `greet` honours). `speakUp`'s chattiness roll is not applied. That roll lived inside `speakUp`, which this path no longer calls, and the design's test is "wait out `CHAT_SHOUT_MINUTES` and the line is there."

### Assumptions

- There is no short zone-name table in this module, so the shout uses `getName()` rather than a nickname like "valkurm" with the "Dunes" dropped. Underscores become spaces so it matches `!bot` / `!bots`. Assumption: `getName()` is the English zone name, possibly with underscores, as the rest of this module already treats it.
- An unknown main job (index past the 23-name table, or `NONE`) is written `ADV`.
- The line is a normal shout (`MESSAGE_SHOUT`), not a callout, so it is stored in the bot's chat memory when it is sent.

### How to test

1. In `settings/bots.lua`, set `CHAT_SHOUT_MINUTES` to 1 and `CHAT_ENABLED` to true. Restart the map.
2. Stand in a zone with an unpartied bot. `!bot <name>` shows the zone (underscores already shown as spaces) and the job number; `!bots` shows the abbreviation and the level.
3. Within about a minute (the timer jitters between half and one and a half times the setting, and the first tick only schedules the next shout) the bot should shout one of the four lines. The zone words should match `!bot`. The job abbreviation and level should match `!bots`.
4. Say "quiet" / "shut up" to that bot, wait for the next interval, and confirm it does not shout until the quiet timer (20 minutes) is over, or tell it "you can talk" and confirm the next interval can shout again.
5. Party chat and linkshell lines should still be model replies, not these fixed LFP lines.
