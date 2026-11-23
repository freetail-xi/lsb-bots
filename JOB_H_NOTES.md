# Job H notes

Could not compile here (server headers are not in this folder). Everything below uses functions, types, and members that already appear in this module, except `CCharEntity::isSeekingParty()`, called out under assumptions. That one is not used anywhere else in this tree.

The test server reads `settings/bots.lua`, not the example. Copy `INVITE_SEEKERS` into that file before testing. It is read with `settings::get<bool>`.

## Settings

In `settings/bots.lua.example`:

- `bots.INVITE_SEEKERS` (default true). A bot party that is camped, or in a fight, may tell one seeking player and invite them. `false` turns that off. Parties of bots still form either way (`BOT_PARTIES`).

No new number for the 40 yalms, the 5 levels, the level 15 healer rule, or the 15 / 40 minute cooldowns. Those are the figures in the job.

## 1. Parties with a role

**`bot_combat.cpp` `isTank`.** Moved out of the anonymous namespace and declared in `bots.h`. Same jobs as before: PLD, WAR, NIN, RUN. `formParties` and the seeker step call it. Nothing else about tanking changed.

**`bot_module.cpp` `formParties`, with `chooseLeader`, `nearLeader`, `fillParty`, `partyFrom`.** Still one party per occupied zone per pass, still a 50% chance, still 2 to 6, still only bots who are unpartied, alive, `Status::Normal`, and not already holding invites.

What changed inside that:

- A bot joins only when they are within 5 levels of the leader-to-be (`abs <= 5`, the old rule) and within 40 yalms (`distance < 40`, the same strict less-than as the other "within" checks).
- The leader is a tank when the cluster has one. Order is the same as `mainTank`: PLD, then NIN or RUN, then WAR, higher level breaks a tie. When there is no tank, the leader is the best `pullerRank` (lower is better; a tie keeps the first).
- Seats fill in that order: the leader, then one healer (WHM, else RDM or SCH, higher level breaks a tie — the same ranking as `partyHealer`), then everyone else, better `pullerRank` first, until 6.
- That leader is the bot who queues `Bot::invites`, before any invite is sent. The inviter becomes the party leader when the invites are accepted. This module has no lead-pass packet, so the lead is chosen by who sends the invites.
- If the highest level in that group is 15 or more and nobody is WHM, RDM, or SCH, that group is not formed. A tank is not required at 15. Below 15 a party may be all mages.
- If the bots standing together include someone 15+ with no healer, the ones under 15 are tried on their own, so a pile of low mages still gets a small party.

`partyHealer` itself is not called here. It returns nothing when `PParty` is null, and these bots are not in a party yet.

**`bot_pull.cpp` `autoCamp`.** The puller loop no longer skips `PMember == PLeader`. A pool member is the puller when their `pullerRank` is strictly better than the others, leader included. An equal rank still keeps whoever was seen first (the old comparison). A real player is still not a pool character, and a party that `withPlayer` is set on still does not auto-camp.

### Assumptions

- "Within 40 yalms" is `distance < 40`. "Within 5 levels" stays `abs <= 5`, now measured from the leader rather than from a random start.
- The level-15 check uses the highest member of the group that would actually form. A leader at 14 with a member at 19 does not happen: 19 is more than 5 levels from 14, so they are not in the same group.
- "Needs a healer" at 15+ means at least one WHM, RDM, or SCH. It does not require a tank. "May have no tank" below 15 is the mage-party case; a tank who is there still leads.
- One party per zone per minute is unchanged. A second cluster in that zone waits for the next `formParties` (60 seconds), and only if the 50% roll hits.
- RUN is still a tank, because `isTank` already says so.

### How to test

1. Park six known jobs in one spot in a field zone a real player is in (so the zone counts as occupied): a PLD, a WHM, a THF, a BLM, and two more damage jobs, all within about 5 levels and standing within 40 yalms. Wait one `formParties` pass (60 seconds; the map log is `bots: <name> is forming a party of N in zone ...`). About half the minutes it will skip the zone on the 50% roll. `!bots` should show them in a party. `!bot` the PLD: they should be leading, not following. The WHM should be in the party.
2. Move a BLM more than 40 yalms away before the pass. That BLM should stay unpartied. They should not be warped onto the leader to make the party.
3. Same test at level 20 with no WHM, RDM, or SCH in range. No party. Add a WHM within 40 yalms and within 5 levels. A party should form, WHM included, PLD leading if the PLD is there.
4. Same test under level 15 with only BLMs (and maybe a RDM) standing together. A small party should still form. Whoever `pullerRank` likes leads (a BRD, if you add one, ahead of a BLM).
5. Once a party of 3 or more is together and out of combat, `autoCamp` runs. If the leader is the best puller (a BRD leading, or a NIN with nobody better), `!bot` on the leader should show them pulling. If a BRD is in the party and someone else leads, the BRD should still be the one who runs out.

## 2. Ask a seeking player

**`bot_module.cpp` `askSeekers`, called from `BotModule::tick`.** On the same cadence as the other periodic steps: the first tick it is due, then again every 30 to 60 seconds (inclusive). `INVITE_SEEKERS` false returns immediately. `sendDueInvites` runs every tick while players are wanted, so the invite is not stuck behind the 30 second timer.

A party is eligible when its leader is an online bot, the party is not full, the leader has no invites already queued, the leader's 40 minute cooldown has passed, and either `campSpot` is set or some member is engaged with a mob. One tell per pass.

The player has to be a real character (`isPoolChar` is false), not in a Mog House, `Status::Normal`, not dead, not in a party, not `PAI->IsEngaged()`, and seeking. Instanced zones are skipped, same as `playerZones`. Their level has to sit within `LEVEL_WINDOW` of the party's lowest and highest main level (inside the band, or up to the window past either end). Their job has to be a role the party does not already have. When several seekers fit, a missing tank is asked before a missing healer, and a missing healer before another damage dealer:

- tank (`isTank`) if the party has no tank,
- healer (WHM, RDM, SCH, and `partyHealer` is not a trust) if the party has no healer,
- anyone else only when the party already has a tank and a healer, and still has a free seat.

The tell is canned, through `tell`, not the model. Shape: `hi, need a whm for 12-15 in valkurm dunes, want to join?` The job word is that player's own abbreviation, lower case (so a WHM sees `whm`, a THF sees `thf`). The band is the party's min and max main level, or one number when they match. The zone is `prettyName` of `loc.zone->getName()`, then lowercased. There is no short-name table in this module, so Valkurm is `valkurm dunes`, not `valkurm`.

Five seconds later `sendDueInvites` pushes that name onto the leader's `Bot::invites`. `think` sends it the same way a `formParties` invite is sent, including a cross-zone invite (targid 0) when the player is not in the zone. It is dropped if they partied up, said no, or the leader can no longer invite.

Cooldowns are stored on the module from the moment the tell is sent: 15 minutes for that player, 40 minutes for that leader. One tell per pass, so the next seeker waits for the next pass and then only if their own cooldown has passed.

**`bot_chat.cpp` `saysNoInvites`, `refuseInvites`, `handleChat`, `respond`.** A tell that says "no invites", "no invite", "stop inviting", "dont invite", "don't invite", "do not invite", "stop invites", or "no more invites" (whole words, via `saysPhrase`) records that character until the process ends. The normal tell reply still happens. The same line is not treated as a party request, so the word "invite" does not queue an invite. A later "lfp" from them still can: they asked. The seeker step will not ask them again. `sendDueInvites` also drops an invite that was already waiting.

### Assumptions

- `CCharEntity::isSeekingParty()` is the /sea flag. It is not called anywhere else in this module. On current LandSandBoat it returns `playerConfig.InviteFlg`, which is the flag `setSeeking` turns on by sending `GP_CLI_COMMAND_CONFIG` with `InviteFlg`. If this server named that method differently, `askSeekers` will not compile; that is the one new call.
- "Not in combat" is `!PAI->IsEngaged()`. Dead, zoning (`Status` not `Normal`), Mog House, and an existing party are also skipped.
- "Has a hunt target" means some party member is engaged and their battle target is a mob. Nothing in this module stores the mob `huntTarget` picked before the engage. A party of two, which never gets `autoCamp` (that wants 3), is only asked while a fight is actually up. A camped party of three or more is asked between pulls too, because `campSpot` stays set.
- A trust does not count as the party's healer. `partyHealer` returns a trust only when no player or bot heals; that result is ignored, so the party still asks for a healer.
- The seeker does not have to be in the party's zone. The tell names the zone the bots are standing in. `think` already invites across zones.
- The role word in the tell is the seeker's job, not a fixed "whm" / "tank" / "dd". A party that is short a healer says `whm` to a WHM and `rdm` to a RDM.
- A party that is still working through `formParties` invites is left alone until that queue is empty.
- Cooldowns and the "no invites" set are in memory only. A restart clears both. Logging out does not.
- `g_PZoneList` is the same list `playerZones` walks (`bot_world.cpp`). The value is used as `.second`, which matches that map.

### How to test

1. Put `INVITE_SEEKERS = true` in the test server's `settings/bots.lua` and restart the map. `LEVEL_WINDOW` at 10 is the window this uses.
2. Get a bot party camped or in a fight (`!bot` the leader: camp line, or fighting). Leave a role out. On a second client, match that role and that level band, turn seek party on, stay unpartied, and don't be in combat. Within about a minute there should be one tell from the leader naming the job, the level band, and the zone, then a party invite a few seconds later. The map log is `bots: <leader> asks <you>: hi, need a ...`.
3. Decline the invite. That leader should not ask again for about 40 minutes, and that character should not be asked by another leader for about 15 minutes.
4. Seek on a job 20 levels off, or on a job the party already has (a second WHM when they have a WHM). No tell.
5. Tell any bot `no invites`. The log should say `<name> asked not to be invited`. Turn seek back on, fit the party, and confirm no further tell and no invite from the seeker step. A tell of `lfp` can still get an invite, because that one was asked for.
6. Set `INVITE_SEEKERS = false` and restart. A seeking character who fits should get nothing.
