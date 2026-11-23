# lsb-bots behaviour design

Review of the bot module in `lsb-bots/`, with ideas taken from `xifo/crowds/` (behaviour only). The xifo tree here has no `crowds_field.lua`; field-party behaviour below is from `xifo/crowds/README.txt` and `xifo/crowds/bots/bots_hunt.lua`. Chat wording, personality cards, and the echo guard stay in the gateway. This note is about what a player sees the bots do.

Era assumed throughout: 75 cap, nothing from June 2008 onward. ToAU and Wings of the Goddess jobs are in range. Geomancer and Rune Fencer are not.

---

## 1. How lsb-bots works today

### Presence

`BotModule::OnInit` (`bot_module.cpp`) reads `bots.ENABLED`, builds a `PacketSystem`, and calls `ensureSchema`, `clearStaleState`, `loadPool`, `ensureLinkshell`, and `startChat`. Pool rows live in `bot_pool` (`bot_pool.cpp`). A row with `charid = 0` gets an account named `bot<accid>` (password `!`, so the login server rejects it) and a real character via `createCharacter`. `loadPool` then joins `chars` and `char_stats` and keeps `PoolEntry` (`name`, `charId`, `accId`, `job`, `level`).

The logic tick is not the map's own loop. `OnCharZoneIn` arms `tick` once, on the first real player's `PSession->scheduler`, at `kLogicUpdateInterval` (that constant is in the server headers, not in this module). Each tick:

1. Drops bots whose entity is already gone.
2. Every 5 seconds, `countRealPlayers` (sessions that are not in `bot_pool`). After `bots.LOGOUT_GRACE` with nobody real online, the target online count becomes 0.
3. Otherwise the target is `min(bots.MAX_ONLINE, pool size)`. `loginNext` / `logout` walk toward it, one login per `bots.LOGIN_INTERVAL`.
4. Every 30 seconds, `balance` tries to keep `bots.BOTS_PER_PLAYER_ZONE` free bots in each zone `playerZones` reports.
5. Every 60 seconds, `formParties` (if `bots.BOT_PARTIES`).
6. Every `bots.ROTATE_MINUTES`, `rotate(1)` logs out one unpartied bot and parks its char id in `resting_` for 15 minutes.
7. For each online bot: `prepare` if the main level changed, then `restock`, `linkshellUpkeep`, `think`, `settle`.
8. Then `handleChat`, `chatter`, `deliverChat`.

`loginNext` prefers a job that is scarce online, and prefers a pool level within 6 of some real player's level (`kNearLevel`). With `bots.CAMP_CHANCE`, it calls `findCamps` and `placeAt` so the character loads at a random outdoor or dungeon spawn whose mob level band sits just above the bot (`bot_world.cpp`). Otherwise `placeAtHome` uses the `bot_pool` zone and position. `balance` can also `placeAt` a logged-out bot, or `travel` a free one, to a spot 25–70 yalms from the highest-level real player in a zone.

A bot is a `CCharEntity` on a `MapSession` that never touches the wire (`Bot::session`). `send` pushes client packets through `PacketSystem::dispatch`, so party, chat, casts, and zoning use the normal handlers. `travel` sets `requestedZoneChange` the same way a GM `setPos` does.

`!bots`, `!bot <name>`, and `!rotatebots` are `bot_commands.lua`. They call `BotsStatus`, `BotsInspect`, and `BotsRotate`, bound in `OnInit`.

### What a bot decides

`think` (`bot_ai.cpp`) is the whole decision, once per bot per tick:

1. Pop the entity packet list. Chat (`GP_SERV_COMMAND_CHAT_STD`) is stored on `Bot::heard` (auto-translate bytes stripped). Emotes from real players become a delayed `emoteBack`. A party-request packet becomes an invite the bot will send.
2. Dead: accept a raise, or send the homepoint menu after 60 seconds (`kHomepointDelay`), or 3 minutes if someone in the party or nearby can raise (`partyCanRaise`, `raiserNearby`).
3. If Signet is missing, add it silently.
4. In a cutscene, `answerEntry` follows the leader into a battlefield (event 32000) or `finishEvent` skips it.
5. Party bookkeeping. `withPlayer` is set when `hasRealPlayer` sees a PC who is not in the pool. `greet` runs on that edge. If the real members are gone for 20 seconds (`kPlayerGoneGrace`), `leaveOrBreakUp` disbands the party when this bot leads, or leaves when it does not. A zoning player drops off the member list for a moment; the grace is what covers that.
6. Accept a pending invite if unpartied. Otherwise send one invite from `Bot::invites` (a player who asked in chat, or a bot `formParties` queued). The `/sea` seeking flag is on exactly while `PParty == nullptr` (`setSeeking`).
7. If the zone is not in `occupiedZones` and the bot is not `withPlayer`, stop. It still fights back if a mob is already on it. `occupiedZones` counts a real player in a Mog House; `playerZones` does not, and it skips instanced zones.
8. Otherwise, in order: `followOrder` (`bot_combat.cpp`), `pull` (`bot_pull.cpp`), then a fight (`restInFight`, camp hold, hate recovery, `keepRange` / `fight`, `act`), or out of combat `act`, `conceal`, `rest`, follow, `autoCamp`, `huntTarget`, `wander`.

`follow` warps to the leader past 50 yalms (`kRegroupDistance`) and otherwise pathfinds to 3 yalms. `wander` in a city walks to a random visible NPC within 100 yalms, and sometimes sits or emotes. Outside a city it roams a few yalms around `Bot::home` (the login spot). `settle` (`bot_move.cpp`) raycasts the bot onto the zone mesh so other clients don't draw it in the air.

`formParties` groups 2–6 unpartied bots in an occupied zone, within 5 levels, with a 50% chance per zone per minute. The leader is whichever bot the random start index landed on. No job check.

`autoCamp` (`bot_pull.cpp`) camps a bot-only party of 3 or more, outdoors or in a dungeon, once `partyReady` says everyone is within 15 yalms and at 70% HP. It calls `setPuller` on the best non-leader member (`pullerRank`: bard with Elegy, then NIN, THF, RNG/COR, PLD/WAR). The leader is never the puller. A party that already has a player does not auto-camp; that party pulls only when someone asks (`OrderKind::Pull` / `setPuller`).

`act` is the combat brain, by main job: emergency cures, Echo Drops, heals, cleanses, raises, sleep on adds, `Directives` (`directed`), bard songs, job upkeep, pets, then nukes, debuffs, weapon skills, or a ranged shot. Out of combat a WHM, RDM, SCH, or PLD will `helpStrangers` (raise, cure under 50% HP, Protect/Shell outside town). Standing instructions live in `Bot::directives` (`KeepUp`, `Restraint`), cleared when `directivesOf` sees a new party id.

`prepare` (`bot_gear.cpp`) runs at login and on a level change: subjob from `defaultSubJob` or `bot_pool.subjob` (via Lua `changesJob` / `setsLevel`; those names are the usual subjob calls, and the server sources are not in this tree), `learnSpells`, a fixed blue-magic list, Corsair dice, and `planGear`. Gear is the highest-level ilvl-0 piece the job and race can wear. Armour is scored by required level, DEF, HP, and MP. Weapons are damage per delay. `restock` refills ammo, shihei, Echo Drops, Silent Oil, and Prism Powder, and deletes non-equipment once the bag has fewer than 5 free slots.

`linkshellUpkeep` (`bot_linkshell.cpp`) keeps a single shell, `bots.LINKSHELL` (default `Wanderers`). `member` is `charId % 2 == 0`, so about half the pool wears the pearl. A tell that `wantsPearl` queues `Bot::pearlFor`, and the next upkeep gives that player a pearl.

### Chat, and when it fires

`startChat` detaches one worker if `bots.CHAT_ENABLED`. The worker blocks in `httpPost` (`bot_http.cpp`) against `bots.LLM_URL`. Replies wait out a short typing delay, then `deliverChat` sends `GP_CLI_COMMAND_CHAT_STD` or `GP_CLI_COMMAND_CHAT_NAME`.

`handleChat` runs after every bot has had `think`, so `heard` is this tick's lines:

- Tells always get `ask`. A pearl word grants a linkpearl when `offersPearls`. A party word (`wantsParty`) pushes the sender onto `Bot::invites`.
- Party lines, and tells from someone in that bot's party, go to `askBrain` when `bots.LLM_ORDERS` is on (`bot_brain.cpp`). The model must use tools (`cast`, `keep_up`, `party_order`, `set_tank`, and the rest). `carryOut` writes an `Order` onto the bot (about 20 seconds) and feeds the note back into the chat prompt. If the model doesn't answer, `planOrder` (`bot_orders.cpp`) matches keywords. Pull lines (`pullChoice`, `recallPuller`, `pullAnyway`) are handled before the brain.
- An unaddressed party or linkshell line: one bot answers a question, and a second may answer if `feelsLikeTalking` (chattiness, and not inside `quietUntil`). Say, shout, and yell are answered when the bot is named; otherwise up to two bots may chime in. A party-word shout also invites, from a bot with no party.
- Bots answer bots only until `kMaxBotRun` (3) on that channel, and not in a party that still has a real player.

`chatter` starts lines on its own:

- Every `bots.CHAT_LINKSHELL_MINUTES`, one pearl-wearer speaks in linkshell, but only while `linkshellPlayers` is non-empty.
- Every `bots.CHAT_PARTY_MINUTES`, a bot in a real player's party makes small talk (a small extra chance in the 10 seconds after `foughtAt`).
- Every `bots.CHAT_SHOUT_MINUTES`, an unpartied bot in a zone that contains a real player shouts for a party.

`describe` (`bot_chat.cpp`) is the system prompt the module itself sends: race, job, level, subjob, nation, zone, party list, current fight, directives, nearby mob names, personality, chattiness. Camp zone names are added only when `asksAboutPlaces` saw a "where / camp" phrase. `speakUp` (the spontaneous lines) calls `describe` without that list. The prompt tells the model not to invent places, items, or a past.

`callout` is a fixed party line (raises, cure requests, pull waits). It is not sent through the model, and it is kept out of `memory` so the model doesn't repeat it.

### Settings

`settings/bots.lua.example` is copied to the server's `settings/bots.lua` (it must not live inside the module directory, or the server would run it as a module script). Keys are `xi.settings.bots.*`, read as `settings::get("bots.KEY")`. The pool, personalities, chattiness, and optional `subjob` are columns on `bot_pool`, not settings.

---

## 2. Gaps

What a player can see today is a strong party member and a thin population.

**They log in as a block, then stand still where nobody is.** `loginNext` fills `MAX_ONLINE` (default 36) within a few minutes of the first real player. About 70% appear at a level-appropriate camp anywhere on the map, the rest at their creation home. `think` then idles any bot whose zone has no real player. Those bots still set the seeking flag, so `/sea` lists people who will not move, hunt, or answer a shout until someone walks into the zone. Turnover is one free bot every `ROTATE_MINUTES` (default 20), then a hardcoded 15-minute rest. There is no session length, no "logs in at the Jeuno home point and out an hour later." xifo's roster (`bots_life.lua`) gives each bot a 20–120 minute session, logs them in at a home point, the auction house, or a shop in their nation, and refuses to log out someone who is in your party.

**Town life is accidental.** `wander` only runs when a real player is already in that city, and its destinations are any visible NPC within 100 yalms. There is no home-point crystal, auction counter, or guild as a preferred spot, and `BOTS_PER_PLAYER_ZONE` (default 3) is the whole crowd. xifo weights those spots (rest, AH, idle) and keeps a crowd there for the session.

**Nobody browses, crafts, or sells.** `prepare` spawns the best ilvl-0 gear. `restock` throws away everything that isn't equipment or a small medicine list, so drops, crystals, and stackables never become a bazaar or a synth. A player never sees a bot at a guild counter or the AH. xifo's growth loop spends offline time on crafting, fishing, and the market, and its crowds shout buy/sell lines for era items. Copying the sell side onto this module would dump spawned gear into the economy; the visible part (standing at the counter) does not require that.

**Parties form, but they aren't built.** `formParties` can make six white mages, picks an arbitrary leader, and invites across the whole zone. Members more than 50 yalms from the leader warp (`follow`). `autoCamp` will not make the leader the puller, so the bard who should pull often stands in camp while a worse member runs out. They do hunt, pull, sleep adds, and rest, which is the right skeleton. They will also take an unclaimed mob a real player is walking up to: `huntTarget` and `unpullable` skip engaged or already-claimed mobs, and they do not look for a real player standing nearby. xifo's field parties leave mobs near the player alone.

**They rarely invite a person.** A bot invites when the player's text matches `wantsParty`, or when the player sends a party-request packet. It does not watch the seek flag. A level 75 will invite a level 7 who shouted "lfp". xifo's invite loop (`bots_invite.lua`) only speaks to someone seeking, checks level and missing role (tank, healer, support, damage), sends a short tell, then invites, and backs off if ignored.

**One linkshell, assigned by char id.** Even char ids wear `Wanderers`. There is no social / exp / craft / endgame shell, no founding, no leaving, no breakup, no leader shouting for recruits. A tell that contains "ls" or "pearl" gets a pearl. xifo keeps up to 16 shells with a type, a size, a level band, a daily leave chance, and a recruit shout, and it will not break a shell a real player has joined.

**They don't grow on the calendar.** Fighting grants real experience, and `logout` copies `GetMLevel` onto the in-memory `PoolEntry` (the DB level comes back from `char_stats` on the next `loadPool`). Nothing remembers hours per day, levels a subjob on purpose, unlocks an advanced job, or catches a bot up for the days it was offline. `defaultSubJob` is one fixed partner, applied by script at level `main.SUBJOB_QUEST_LEVEL`. xifo (`bots_growth.lua`, `bots_jobs.lua`) stores a play-style, grants catch-up exp at login, does the subjob quest at 18, and sometimes switches main job from 30. The cap there is 75.

**The prompt is not allowed a past, and the module never gives it one.** `describe` lists the current fight and party, and then tells the model not to invent a past. Level-ups, deaths, invites, and linkshell joins are not recorded. The gateway already remembers lines that were said; it cannot remember a ding that was never written down. xifo keeps a short diary and a per-player note for exactly this.

**Gear reads as "highest DEF."** A white mage and a warrior share `armourScore`. Players notice the robes. Job tables also still name Geomancer and Rune Fencer (`jobColumns` in `bot_pool.cpp`, `isCaster` and `isTank` in `bot_combat.cpp`, the race/job strings in `describe`). A pool row can be one of those jobs on a 75-era world.

**Shouts are the model's guess.** `chatter` asks the model to "shout for a party the way players do" while `describe` forbids inventing a place and does not pass `campZoneNames` (that list is only for "where next" questions). The zone the bot is standing in is in the prompt, so a careful model can name it. Nothing checks that it did.

---

## 3. Proposals

Ranked by what a player would notice, divided by how large the change is. S is a day or two in this module, M is a few systems touching the tick, L is a new loop with its own data. Gateway means a new fact in the prompt the model sees. Where that fact is already built in `describe`, the gateway does not have to change.

### 1. Leave real players' mobs alone

**Player sees:** A bot party in the same zone pulls its own camp. It does not open on the beetle the player is about to claim, and `balance` does not drop a bot on top of the player's pull.

**Where:** `huntTarget` (`bot_ai.cpp`) and `unpullable` / `pickTarget` (`bot_pull.cpp`) gain a distance check against real PCs (not pool chars) and against mobs those PCs are in combat with. Something like 40 yalms matches the xifo field rule; the number should be a setting (`bots.PLAYER_LEEWAY`) so the test server can tune it. `balance` already picks `spotNear` around `PlayerZone::pos`; push the minimum radius out, or skip a camp spot inside the leeway. `beingPulled` does not need to change.

**Size / risk / test:** S. Risk is a bot refusing every mob in a crowded zone (Valkurm with two real parties). Test with two clients: one pulls, the other watches `!bot <name>` and the bot party's camp. Confirm the bot still pulls a mob on the far side of the zone, and still defends itself when something aggros it (`think` already allows that while idle).

**Gateway:** No.

### 2. A level window on who shows up and who gets invited

**Player sees:** A level 12 in Gusgen is not joined by three level 75s, and a "lfp" shout is not answered with an invite from a bot 40 levels away. `/sea` and zone population sit near the levels actually online.

**Where:** `balance` (`bot_module.cpp`) already computes `gap` and picks the closest pool level, then sends that bot anyway. Stop after a max gap (suggest 10, setting `bots.LEVEL_WINDOW`). If nobody in the pool is inside the window, leave the zone under-filled. `loginNext` already prefers `kNearLevel` (6); widen it to the same window so the two paths agree. `respond` (`bot_chat.cpp`, the `wantsParty` invite branches around the tell and the shout) should compare `GetMLevel` before `invites.push_back`. `formParties` already uses 5 levels; leave it.

**Size / risk / test:** S. Risk is an empty zone when the whole pool is 75 and the only player is level 10. That is the better failure. Test: a level 15 client and a pool that also has 70s. `!bots` should show the 70s elsewhere or offline, not in the starter zone. Shout "lfp" on the level 15 and confirm no 70 invites.

**Gateway:** No. The invite simply doesn't happen, so the prompt's "you've already sent them an invite" line stays true only when an invite was queued. Worth fixing that sentence in `ask` if the level check fails: pass a note that no invite was sent. That note is local to `describe` / `ask`, not the gateway.

### 3. Sessions, and a seeking flag that matches them

**Player sees:** People arrive and leave through the day. `/sea` changes. Someone in your party does not vanish at the end of their session. A bot listed as seeking is in a zone where someone could actually meet it, or it is walking to one.

**Where:** New columns or a small table, written from `logout` and read in `loadPool`: `session_end`, and maybe `hours` (see proposal 10). `BotModule::tick` logs a bot out at session end when `isFree` (same rule as `rotate`: never while it is in a party). `loginNext` rolls a new length (suggest 20–120 minutes, `bots.SESSION_MIN` / `SESSION_MAX`) and logs the bot in at a town spot (proposal 4) or a camp, instead of topping the online count up as fast as `LOGIN_INTERVAL` allows. Keep `MAX_ONLINE` as a cap. `rotate` can stay as a manual shuffle; the 15-minute `resting_` park should not outrank a session that just started.

Seeking: `setSeeking` in `think` should be off when the bot is about to take the idle return (`!active && !withPlayer`). An idle bot in a camp nobody can see should not advertise LFP.

**Size / risk / test:** M. Risk is the online count collapsing if sessions are too short, or a party leader hitting session end and disbanding via the existing logout path. `logout` already saves and removes the char; confirm a party leader's logout does something the server accepts (the current `rotate` path only logs out unpartied bots, so this is new). Test: set `SESSION_MAX` to 2 minutes on the test server, watch `!bots` turn over, invite one bot and confirm it stays, then `/sea all` from a zone with no bots and confirm the idle campers are not seeking.

**Gateway:** Optional. A single line in `describe` ("you logged in at Jeuno about twenty minutes ago") stops the model inventing a login story. The gateway's own memory does not need a new channel.

### 4. Town spots worth standing at

**Player sees:** In Jeuno, San d'Oria, Bastok, and Windurst, bots stand at the home point, the auction house, and a guild counter, sit, emote, and walk between those. They still answer emotes (`emoteReply` already does this).

**Where:** `bot_world.cpp` gets a fixed list of era town spots (zone, position, kind: `home`, `ah`, `guild`, `idle`), hand-placed once on the test server rather than "any NPC." `loginNext` and the town branch of `wander` (`bot_ai.cpp`) pick from that list, weighted the way xifo does (more resting and AH than a random guard). Keep the NPC wander as the fallback when a town has no spots. Do not require a player in the zone for the walk if proposal 3 has put them there on purpose; the current idle cut in `think` would still freeze them, so a bot whose session home is a town should count as active while a real player is anywhere in that city. Mog House stays off (both `placeAt` and `travel` already clear `moghouse`).

**Size / risk / test:** S, once the coordinates exist. Risk is a spot inside a wall; `spotNear` and `settle` already pull toward the navmesh, but a bad point still looks wrong. Test: log a bot in at each nation, walk up, confirm it reaches the crystal and the AH door and that `!bot <name>` says "standing" or "moving" rather than falling through the floor.

**Gateway:** No.

### 5. Parties with a role, formed from bots who are already nearby

**Player sees:** An exp party in the dunes has a tank, a healer, and damage, a leader who is standing with them, and a puller who is actually the puller. They don't pop across the zone to stack on the leader.

**Where:** `formParties` (`bot_module.cpp`) should require a distance (suggest 40 yalms, so `kRegroupDistance` stays a recovery tool and not the way the party meets). Score jobs with the helpers that already exist: `isTank`, `partyHealer` / WHM-RDM-SCH, `pullerRank`. Fill tank, healer, then damage, up to 6, still within 5 levels. Pass lead to the tank (or the highest `pullerRank` if there is no tank) before the invites go out. `autoCamp` should allow the leader to be the puller when `pullerRank` says so; today the loop skips `PMember == PLeader`.

**Size / risk / test:** M. Risk is never forming a party when the zone only has mages. Allow a party with no tank below a level (suggest 15), and still require a healer above that. Test: park six known jobs in one zone with a real player, wait one `kGroupingInterval`, `!bots` and `!bot` the leader, watch who runs out to pull.

**Gateway:** No. `describe` already prints the party from live members.

### 6. Ask a seeking player to join, in a tell, then invite

**Player sees:** With seek party on, and no party, a bot leader whose camp is short a healer (or a tank, or a DD) sends a tell that names the real zone, the level band, and the role, then invites a few seconds later. Decline or ignore it and that leader waits a long time before asking again. A setting or a tell ("no invites") turns it off for that character.

**Where:** New function called from `BotModule::tick` every 30–60 seconds, next to `formParties`. Scan `playerZones` / zone chars for a real PC who is seeking, unpartied, and not in combat. Match level with the same window as proposal 2, and match role with `isTank` / healer / everyone else, against the bot party's jobs. The tell should be a fixed string built from `getName()`, `GetMLevel`, and the zone name, sent with `tell` (`bot_chat.cpp`), not `speakUp`. The invite reuses `Bot::invites`. Store the last ask on the player id in a map with a cooldown (suggest 15 minutes per player, 40 per leader). Do this only for bot parties that already have a camp (`campSpot`) or a hunt target, so the tell's zone is where the bots are standing.

**Size / risk / test:** M. Risk is pestering. The cooldown and "only if seeking" are the whole safety. Test: seek on a fitting job, confirm one tell and one invite; decline; confirm silence; seek on a job 20 levels off and confirm silence. `!bot` on the leader should show the party and the zone you were told.

**Gateway:** No, if the tell stays canned. A model tell will invent the camp. The gateway does not need a new channel.

### 7. Gear that matches the job

**Player sees:** Mages in robes and staves, tanks in heavier armour, bards still holding an instrument (that special case already exists in `planGear`).

**Where:** `armourScore` and the weapon pick in `planGear` (`bot_gear.cpp`). Keep `usable` (ilvl 0, no GM gear, job and race). Add a job bias: healing and MND/CHR for WHM/RDM/BRD/SMN, INT for BLM, STR/VIT and DEF for WAR/PLD/DRK, accuracy and haste where the item mods exist. Do not pull in item level or post-2008 gear. `prepare` already re-runs on level change, so a relog or a ding is enough to swap.

**Size / risk / test:** M. Risk is a empty slot when the bias rejects every piece, or a two-hander with a grip still equipped (`planGear` already clears `SLOT_SUB` for two-handed weapons). Test one bot of each starting job plus BRD, NIN, RNG, and PLD at 20 and at 75. `!bot` won't show gear; look at the character. Confirm ammo still restocks (`bot.ammoId`).

**Gateway:** No.

### 8. A few linkshells, and a recruit shout

**Player sees:** More than one pearl in the wild. An exp shell whose members are around the same level, a social shell that will take anyone, an endgame shell that only talks to 55+. Now and then a member shouts in a city, naming that shell. A tell still gets a pearl, and a shell a real player has joined is not dropped on restart.

**Where:** Replace the single `shellId` and `charId % 2` test in `bot_linkshell.cpp`. A small table, created beside `bot_pool` in `ensureSchema`: shell name, type (`social`, `exp`, `endgame`), level band, linkshell id. Three or four rows are enough for the first cut; do not generate names in code on a loop the way xifo does, or two servers will fight over the same `linkshells` row. Assign a bot to a shell at login from its level (`Bot` needs a shell id; the even-charid rule goes away). `linkshellUpkeep` equips that pearl. `chatter`'s linkshell block already picks a wearer while a real player is in the shell; point it at each shell that has a real member. Add a city shout on a long timer (20–45 minutes), canned, from a wearer who is in town: shell name, type, level band. `wantsPearl` stays, but only a wearer of the shell the player named, or any wearer if they didn't name one.

Leave founding, quitting, and breaking for proposal 12. This slice is the part players notice.

**Size / risk / test:** M. Risk is handing out pearls for a shell that failed to register (`ensureLinkshell` already logs "unavailable"). Test: two clients, tell two different bots, equip, confirm linkshell chat is not one shared room unless you joined the same shell. Confirm a restart does not mark the shell broken (`broken = 0` is already the lookup).

**Gateway:** One line in `describe`: shell name and type. Without it the model invents a shell name the moment someone asks. That line is in `bot_chat.cpp`. The gateway's linkshell conversation log should be per shell id if it is currently one global log; if it is already per in-game shell, nothing else is required. I can't see the gateway from this tree, so that split needs a check there.

### 9. Shouts that name the place the bot is actually standing

**Player sees:** "lfp 24 war valkurm" from a bot who is in Valkurm at 24, or a recruit shout for the shell in proposal 8. You don't get invited to a camp in a zone the bot left an hour ago.

**Where:** The unpartied branch of `chatter` (`bot_chat.cpp`) should send a fixed shout, the same way `callout` and `tell` skip the model: job abbreviation, main level, zone name from `loc.zone->getName()`. Keep `speakUp` for party small talk and linkshell, where `describe` has the live party. Drop the "mentioning your job and level" prompt for shouts so the model is not asked to invent a location the system line already forbids.

**Size / risk / test:** S. Risk is a stiff shout. That's the right trade; party chat can stay loose. Test: stand in the zone, wait out `CHAT_SHOUT_MINUTES` (turn it down on the test server), confirm the zone word matches `!bot`.

**Gateway:** No. A canned line does not need a new prompt fact.

### 10. Catch-up levels while they are offline

**Player sees:** A bot you partied with last week is a few levels higher, still under 75, with a subjob that fits. It can say so because the prompt contains the ding. It does not appear at 75 the day after you met it at 20.

**Where:** On `logout`, store `last_seen` and a daily hour budget on `bot_pool` (casual / regular / hardcore, rolled once, as xifo does). On `login`, if the gap is at least an hour, grant exp for the offline hours times a per-level rate, capped at a few days and at level 75, through the server's own exp function (the character is loaded inside `enterZone`, so the grant belongs at the end of a successful login, after `prepare`). Keep the rate low enough that a bot who also hunts while you are online does not skip content. Subjob: `defaultSubJob` already runs inside `prepare`; extend it so a bot under the subjob quest level does not wear a sub, and so the sub level stays at half the main. Do not unlock Geomancer or Rune Fencer. Advanced-job quests (the actual trips) are out of scope for this slice; a column `bot_pool.job` change at 30+ can wait until someone wants to script the quest.

Record one line, "gained N levels while out, now WAR 24", for proposal 11.

**Size / risk / test:** M. Risk is a double grant if login fails halfway and retries, or exp during the `LoadChar` window before the entity is in a zone. Apply the grant once, then write `last_seen`, and only after `enterZone` returns true. Test: set a bot's `last_seen` a day back, log it in, confirm one ding and a second login minutes later does not ding again. Confirm a bot in your party is not also catch-up granted for the time it was online (the stamp moves on `logout` only, so a long session catches up next login; that's what we want).

**Gateway:** The new level is already in `describe` via `GetMLevel`. Add the one catch-up sentence there when it happened this login, so the model can mention it without inventing the number. No new gateway memory is required for that. Optional later: hand the same sentence to the gateway's per-bot memory so it survives the 12-line `memory` deque in `bot_chat.cpp`.

### 11. A short world diary in the prompt

**Player sees:** The bot can talk about the pull that killed it, the party it just left, or the level it just got, and the names match what happened. It still can't invent a relic weapon.

**Where:** A deque on `Bot`, capped around 8 lines, also written to a `bot_pool` text column so it survives logout. Append from the places that already know the facts: `think` on death and on homepoint, `enterZone` / proposal 10 on a ding, `greet` on joining a real party, `leaveOrBreakUp` on the breakup, `linkshellUpkeep` on a pearl given. `describe` adds "Recent events:" plus those lines, next to the party block. The existing "don't make up a past" rule stays; the diary is the past it is allowed to use.

This is the in-game half of what xifo keeps as a diary. It is not a second personality system. The gateway already stores what was said.

**Size / risk / test:** S. Risk is the model reciting the diary every line. Keep it to two lines in the prompt, newest first, and only when non-empty. Test: die in a party, get raised, ask "what happened", confirm the reply can mention the death and does not mention a different zone.

**Gateway:** The lines have to be in the prompt `describe` builds, or the gateway never sees them. No new gateway feature if `describe` includes them. If the gateway replaces the module prompt rather than wrapping it, it needs these lines forwarded. I can't see that wrapping from this tree; whoever owns the gateway should confirm it still sends `describe` through.

### 12. After a wipe, come back as the same party

**Player sees:** A bot party that dies doesn't scatter to three different home nations and sit there. They homepoint, the leader picks the same camp again, and the others `travel` back. A party that was with you still waits on a raise (`kRaiseWait` already does that) and only homepoints when nobody can raise.

**Where:** `think`'s death branch always sends `HomepointMenu`, and the character's home is the `bot_pool` spot from `createCharacter` (`home_zone` is never updated). For a bot-only party, remember the camp (`campSpot` / `Pull::camp`) on the leader before the homepoint, and have `follow` run after they are alive again (it already zones to the leader once `withPlayer` or `active` is true). The gap is the idle cut: a leader who homepoints into an empty city freezes, and the members freeze in their own home cities, so `follow` never runs. Exempt a party that still has a living leader, or a remembered camp, from that idle return until they are back in the camp zone. Cap it (suggest 10 minutes) so a stuck path doesn't keep three zones "active" forever.

**Size / risk / test:** M. Risk is bots pathing into a city they don't belong in, or a camp pointer used after the `Pull` state is gone (`breakCamp` on follow-order already clears it; a wipe should not). Test: pull a too-hard camp with a 3-bot party, wipe, watch `!bots` until they are back on the same spot. Confirm a bot that is in your party still takes your raise instead.

**Gateway:** Optional one-liner in the diary (proposal 11): "wiped at the camp, heading back." No gateway change of its own.

### 13. Stand at the auction house and the guild, and do not sell spawned gear

**Player sees:** In town, some bots linger at the AH and at their nation's guild. A crafter bot might synth an era recipe it can afford. Nobody is buying a hauberk the bot conjured in `prepare`.

**Where:** This is proposal 4's spots, plus a slow action while `wander` is idling at a guild spot. A new file `bot_craft.cpp` is appropriate: a short list of 75-era recipes per guild, a gil purse on `bot_pool` that only goes up from real mob drops (so `restock` must stop deleting crystals and craft materials, and must keep deleting rare and equipment it didn't equip). Synths go through the same packet or Lua path the server uses for a player synth. Cap attempts per session. No bazaar of `planGear` items. If a real economy is unwanted even then, stop at the standing-around and skip the synth; the standing is most of what players see, and proposal 4 already covers it. This item is the synth, ranked last because of the economy.

**Size / risk / test:** L. Risk is gil and material duplication if `restock`'s delete list and the craft purse disagree. Test on the test server with one bot, one recipe (a low alchemy or cooking synth), confirm the item appears and the gil moves, and confirm a second bot does not have a bazaar full of spawned gear. Watch `restock`'s delete path against a bag that holds crystals.

**Gateway:** A diary line ("synth'd a pie at the cooking guild") is enough. No new prompt machinery.

---

## 4. Bugs and fragile spots

**`!bot` reads off the end of the order-name array when the order is an ability.** `OrderKind::Ability` is the 12th enumerator (index 11). `inspect` uses `std::array<const char*, 11> orders` and indexes it with `bot.order->kind` (`bot_ai.cpp` 915 and 957). A bot carrying an ability order makes `!bot <name>` undefined. The other eleven kinds line up with the array.

**`wantsParty` matches the start of a word.** `bot_chat.cpp` 245–260 checks that the character before "inv", "join", "pt", and the rest is not a letter, and does not check the character after it. `says` in `bot_orders.cpp` does check both sides. A tell or shout that contains "inventory", "joining", or "options" can queue `Bot::invites`. "inv" is the dangerous one.

**An exception on the chat or brain thread stops that thread for the rest of the process.** Both workers are detached and have no try/catch (`bot_chat.cpp` `worker` at 475, `bot_brain.cpp` `worker` at 354). `dechunk` (`bot_http.cpp` 210) does `string::append` with a chunk size taken from the wire; a short or timed-out body throws `std::out_of_range`. `JsonReader::parseString` (`bot_brain.cpp` 172) calls `std::stoi` on `\u` escapes and throws if the hex is bad. The chat worker then never reads `shared.requests` again: tells get no answer until the map restarts. The brain worker is worse: `handleChat` has already stored the party line in `waiting` and will not fall back to `planOrder` until `brainOutcomes` returns that id. A dead brain thread means those orders sit forever and keyword orders never run. `httpPost` is only called from these threads, so a throw there does not kill the map tick. It does kill chat.

**The logic tick is pinned to the first real player's scheduler.** `OnCharZoneIn` (`bot_module.cpp` 100–114) stores `scheduler_` and `tickToken_` once and never moves them. I can't see `MapSession` from this tree. If that scheduler is owned by the session and destroyed on logout or zone, the bot tick either stops or runs against a freed scheduler after that player leaves. Worth confirming in the server before trusting a long uptime.

**Party-pointer maps are never pruned, and they key on the object address.** `botRuns` uses `fmt::ptr(PSender->PParty)` (`bot_chat.cpp` 1060). `partyQuietUntil` is `std::map<const CParty*, time_point>` (`bot_chat.cpp` 1309). Neither is erased when the party disbands. The key is not dereferenced, so this is not a use-after-free by itself. A new `CParty` at the same address inherits the old bot-run count or the old quiet timer. `partyLines` and `partners` grow for the life of the process too; those are keyed by id and name, so they only leak.

**`balance` will cross any level gap.** Covered as proposal 2. As it stands (`bot_module.cpp` 455–476), the closest pool level is used even when that is 75 versus a level 1, and `travel` sends them.

**Signet is applied in every zone, and campaign regions want Sigil.** `think` (`bot_ai.cpp` 1079–1082) adds `StatusEffect::Signet` whenever it is missing. `signetWorks` (`bot_combat.cpp` 1926–1934) only treats Signet as the rest-and-TP effect in regions up through Limbus, and wants Sigil on the Crystal War fronts. Bots never receive Sigil, so on those fronts they lose TP when they rest and do not get the regen `signetWorks` is there for. Separate question, which I cannot answer from this tree: whether kills by a Signet PC count for conquest. If they do, a full `MAX_ONLINE` of hunting bots will move region control. Worth a test on the conquest tallies before the population grows.

**`createCharacter` does not roll back.** `bot_pool.cpp` 112–141 inserts the account, then a chain of character rows, and on failure logs "failed part way" and returns. The next start sees `charid = 0` still and can insert another account. `nextId` is `MAX(id)+1` with no transaction. Fine on one map process; two map processes can hand out the same id.

**Every player emote is an info log line.** `think` (`bot_ai.cpp` 1021–1026) logs caster, target, indexes, emote, mode, and param for every emote from a real player, before it decides to answer. A busy city will fill the map log. The answer path itself (`emoteReply`) is fine.

**`describe` says subjobs start at 18.** `bot_chat.cpp` 770. `subJobScript` uses `main.SUBJOB_QUEST_LEVEL` (`bot_gear.cpp` 423). If that setting is not 18, the prompt is wrong and the model will repeat it.

**`findCamps` walks every mob in every outdoor zone and dungeon on the map thread.** `loginNext` calls it on each login (`bot_module.cpp` 354), outside the per-bot `timed` wrapper that warns at 200 ms. `campZoneNames` calls it again the first time a level is described (`bot_chat.cpp` 743). A large zone list can hitch the tick; the watchdog noted in `tick` fires at 2 seconds. The result is not cached except the three zone names per level inside `describe`.

**`restock` and `linkshellUpkeep` share the throttle map with combat.** `throttle` keys `(targetId << 32) | key` (`bot_combat.cpp` 2426). `restock` uses key `0x30000` and target 0 (`bot_gear.cpp` 518). `kAttackKey` is also `0x30000` (`bot_ai.cpp` 80) but it is stored with the mob id, so they don't collide today. A new caller that uses `0x30000` with target 0 would suppress restock for 20 seconds, or be suppressed by it. Fragile, not currently broken.

**Spontaneous shouts fight the prompt.** `chatter` (`bot_chat.cpp` 1395) tells the model to shout "the way players do, mentioning your job and level." `describe`, used by `speakUp`, says not to invent places and only lists camp zones when the player asked where to go. The bot's real zone is in the prompt, so this sometimes works. It is not checked. Proposal 9 replaces it.

**Geomancer and Rune Fencer are in the job tables with no era gate.** `jobColumns` / `jobLevelQueries` (`bot_pool.cpp` 20–49), `isCaster` (`bot_combat.cpp` 2446), `isTank` (`bot_combat.cpp` 1688, RUN), and the name arrays in `describe` and `planOrder`. A `bot_pool.job` of 21 or 22 becomes a character if the `char_jobs` columns exist. On this server they should be rejected in `createCharacter`.

**`subJobScript` is a Lua string.** It only interpolates a job id and a level (`bot_gear.cpp` 440), so it is not an injection path. Pearl names are checked with `isalpha` before they are interpolated (`bot_linkshell.cpp` 127). That part is careful. The assumption that `changesJob` means subjob is not checked in this tree.
