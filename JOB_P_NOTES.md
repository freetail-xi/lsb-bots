# Job P notes

Could not compile here (server headers are not in this folder). With `CITIZENS = 0` no bot is marked, and the new branches do not run.

Copy the three new keys from `settings/bots.lua.example` into the live `settings/bots.lua` before testing. A missing key is the same failure as any other missing `bots.*` setting.

The map build is assumed to compile every `.cpp` in this module. There is no source list in this folder. `bot_citizen.cpp` has to be part of that compile or the new calls will not link.

## Who is a citizen

**`settings/bots.lua.example`.** `CITIZENS` (0), `CITIZEN_ZONE` (230, Southern San d'Oria), `CITIZEN_SAY_MINUTES` (4).

**`bots.h` `Bot::citizen`.** Set in `bot_module.cpp` `login` from `citizenIds`: the `CITIZENS` pool rows with the lowest char ids, sorted, so the set is the same after a restart. They count toward `MAX_ONLINE` because they sit in `online_`.

**`bot_module.cpp` `loginNext`.** While any of those rows is `available`, that login is the lowest such char id. `placeCitizen` runs first, then `login`. The normal camp / home pick skips those char ids, so a citizen is never sent to a camp.

**`bot_citizen.cpp` `placeCitizen`.** `placeAt` in `CITIZEN_ZONE` at `spotNear` the position of a random named NPC (1 to 3 yalms). `enterZone` then stores that spot as `Bot::home`.

**`bot_module.cpp` `balance`.** Does not log another bot in while a citizen is still waiting, does not choose a citizen as the bot to log in or to `travel`, and does not count citizens toward `BOTS_PER_PLAYER_ZONE`.

**`bot_module.cpp` `tick` logout, `rotate`, `canEndSession`.** Citizens are not rotated and do not get a session end (`login` leaves `sessionEnds` unset). Over the cap, a non-citizen goes first. When no real players are left (`target == 0`), citizens log out with everyone else. `!rotatebots` will not drop them.

## They do not adventure

**`bot_module.cpp` `formParties`, `askSeekers`, `sendDueInvites`.** Citizens are left out.

**`bot_module.cpp` `OnIncomingPacket`.** An invite to a citizen is not rewritten and does not make them leave a party. The packet still reaches the server, so `InvitePending` is set.

**`bot_ai.cpp` `think`.** Citizens clear `invites` and call `declineInvite` instead of `acceptInvite`. `wantSeek` is false. `focusTarget` is cleared, so they do not keep a hunt target. `rememberCamp`, `startReturn`, `joinOuting`, `startHeadOut`, `followIn`, `followOrder`, and `pull` are skipped. After the existing fight block, `citizenAct` runs and returns, so they do not reach solo `act` / `rest`, follow, camp, `huntTarget`, or `wander`.

**`bot_citizen.cpp` `declineInvite`.** Same packet as `acceptInvite`, with `GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Decline`. There is no decline helper in this module. On current LandSandBoat that value is 0, the inviter gets the "invitation declined" message, and `InvitePending` is cleared.

**`bot_chat.cpp` `chatter`.** Citizens are skipped for linkshell, party, and shout lines. **`handleChat`.** A tell whose listeners are all citizens skips the order / brain path, so they are not given a pull, camp, or attack. **`respond`.** A citizen who is named, or who answers a tell, does not queue a party invite.

A mob that is actually hitting them still goes through the normal fight block, same as an idle bot. `combatTarget` is unchanged.

## The routine

**`bot_ai.cpp` `think`.** When `active` is false and nothing is attacking them, `citizenPause` and the existing idle return (path cleared, no walk). `active` is the same flag as before: a real player in the zone, or `ACTIVE_EVERYWHERE`.

**`bot_citizen.cpp` `citizenAct`.** Only in `CITIZEN_ZONE`. Outside it, the path is cleared and they stand. No `travel` to another zone.

On the first tick, and again after `citizenPause`, they wait `(charId % 23) + random(0..12)` seconds before the next stop, so they do not all step off on the same tick. A walk that was already chosen is resumed after that wait and is not logged again. Between stops, `charId % 5` seconds.

A stop is a random NPC with `Status::Normal`, not name-hidden, and a name that contains a letter (the same status and hidden test as `wander`), or, 25% of the time, `home`. The NPC they just visited is not picked again, and home is not picked twice in a row. Wander's 100 yalm cap is not applied, so a stop can be across the city.

`walkTo` with a within of 3. Then `LookAt` the stop. If they are still outside 3 yalms after 2 minutes, or for 10 seconds they are not following a path and not closing the distance, that stop is dropped and another is chosen. The 2 minute clock restarts if they were paused mid-walk.

They linger 30–120 seconds. About half the time, one emote from the same eight `wander` uses in town. At a home stop, about 40% of the time, `setSitting` for a stretch, then stand. They stand before the next walk.

`emote` and `setSitting` were local to `bot_ai.cpp`. They are now declared in `bots.h` and defined next to `walkTo`. The bodies are the same, and `wander` still calls them.

One info line when a stop is chosen, not each tick of the walk: `bots: <name> (citizen) walks to <NPC or home>`.

## Small talk

**`bot_citizen.cpp` `speak`, `kLines`.** Forty lines in the array at the top of `bot_citizen.cpp`. Ten of them contain `{name}`, filled with the NPC they are stopped at. Those are skipped at home.

The first line is `random(20 .. CITIZEN_SAY_MINUTES * 60) + (charId % 47)` seconds out. After that, the same half-to-one-and-a-half spread as `chatter` (`sayJitter`). `CITIZEN_SAY_MINUTES` 0 sends nothing.

They speak only when a real player (not a pool bot, not in a mog house) is within 30 yalms. Not the line they said last, and not a line another citizen within 15 yalms said in the last minute (kept in this process only). If every line is blocked, they try again in 20 seconds.

**`bot_chat.cpp` `sayCanned`.** Queues a fixed `/say` on the same reply list as `tell`. **`say`** logs a citizen `/say` as `bots: <name> (citizen) says: <text>`. Other channels keep `bots: <name> says (channel): ...`. "Quiet down" still drops the canned line. No model call.

## Answering a player

**`bot_chat.cpp` `respond`.** One citizen reply per heard `/say`:

- If the line names one or more citizens, the one whose name appears first, and only if their 20 second reply cap is free. A named citizen who is on cooldown still counts as addressed, so nobody else takes the line.
- If the line names no bot, the closest citizen within 6 yalms, unless `sayAddressedToOther` finds another character in the zone or a visible NPC named in the line (names shorter than 3 letters are ignored). That reply returns before the optional shout/say pile-on.
- Citizens are skipped in that pile-on, and they are not the bot who invites off an "lfp" shout.

The reply is `ask` with `sentInvite` false, so the prompt does not claim an invite went out. It is a `/say` because the heard kind is say.

**`bot_chat.cpp` `describe`.** For a citizen, "Right now" says `at home in the city` instead of `looking for a party`, and one line is added: `You are a resident of this city. You do not adventure.`

**`bot_chat.cpp` `ask`, `bot_citizen.cpp` `citizenReplyAllowed` / `citizenReplyUsed`.** One model reply per citizen per 20 seconds, from when the request is queued. Canned small talk does not use this cap.

## Assumptions

- `GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Decline` exists beside `Accept` in `0x074_group_solicit_res.h`, which this module already includes. It was not named anywhere in this folder. The decline is that packet only; they do not also `/say` it.
- "Next to" an NPC at login is `spotNear` at 1–3 yalms. `loginNext`'s 5–25 ring is for camps. If the zone is not on this map, `placeCitizen` warns and does not move them (and does not send them to a camp). If the zone has no named visible NPC, it warns and uses `spotNear` around the origin, out to 30 yalms.
- NPC choice uses `wander`'s status and `IsNameHidden` checks, plus a letter in the pretty name, and not the 100 yalm cap.
- Home is where they logged in (`Bot::home`), about 25% of stops, never twice in a row.
- A walk "fails" when they are not on a path and not getting closer for 10 seconds, or when 2 minutes pass. Pausing (no real player in the zone) restarts that clock.
- A citizen who dies still homepoints. They do not copy a camp or walk back to one. If the homepoint is outside `CITIZEN_ZONE`, they stand there until something else puts them back. They do not `travel` themselves.
- Small talk only runs in `CITIZEN_ZONE`. "Within 30" and "within 6" and "within 15" are `<=`.
- A `/say` that names a citizen is answered even past 6 yalms, as long as they heard it. One citizen answers that line. An adventurer who is also named still answers through the old path.
- The 20 second cap is every model reply (say, tell, shout), not only `/say`.
- Tells to a citizen are conversation only. A linkpearl tell still uses the existing pearl path.
- Citizens can still be logged out to meet `MAX_ONLINE` once every non-citizen who is free is already gone.
- The new `.cpp` is picked up because the server compiles every module cpp. That is not visible from this folder.

## Test plan

Merge the three settings into `settings/bots.lua`. Set `CITIZENS = 30`, `CITIZEN_ZONE = 230`, `CITIZEN_SAY_MINUTES = 4`. Stand a real player in Southern San d'Oria and restart the map (or wait out `LOGOUT_GRACE` and let them log in again).

Working:

- The first logins are citizens, before any camp login. `!bots` shows those 30 in Southern San d'Oria, and not in a party. No `logged in for N minutes` on them.
- `bots: <name> (citizen) walks to <NPC>` and sometimes `walks to home`. One line per stop, not a stream while they walk. They do not all change stops on the same tick. They stop near the NPC, face it, and some sit only after a `walks to home`.
- With you within about 30 yalms, `bots: <name> (citizen) says: ...`. Lines sound like the city. A stop at a named NPC sometimes includes that name (`Good morning, Machielle.`). The same citizen does not repeat the same line back to back. Two citizens standing together do not echo the same line within a minute.
- `/say` their name: one `/say` back, and the log is `bots: <name> (citizen) says: ...`. A second `/say` to that same citizen inside 20 seconds gets no model reply.
- `/say hello` within 6 yalms, naming nobody: one nearby citizen answers. Naming an NPC (`Machielle, hello`) does not make a citizen answer unless that citizen's own name is in the line.
- Invite one. You get the declined-invitation message. They stay unpartied. No `is forming a party`, no `is going for`, no `heads to`, no `lfp` shout, and no linkshell line from those names.
- Step out of the zone. The `walks to` lines stop. Come back and they start again, not all on the first tick.
- Aggro something onto one. They fight it. They do not run off to hunt on their own.

`CITIZENS = 0` and a restart: no `(citizen)` lines, logins go to camps or home as before, and party, hunt, shout, and balance behave as they did.
