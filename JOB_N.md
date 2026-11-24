# Job N: blood pacts never fire; bots don't group away from a real player (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Commits up to `dd19133` are in.
Read `bot_combat.cpp` (`tryAbility` near line 200, `bloodPacts` near 1559, the pact call near 1691) and
`bot_module.cpp` (the `active` line near 526, `formParties` near 616, `askSeekers` near 682) first.

A test with 500 bots and `bots.ACTIVE_EVERYWHERE = true` showed two problems.

1. **Blood pacts are checked against the wrong bitset.** `tryAbility` calls `charutils::hasAbility(PChar, id)`.
   That reads `m_Abilities`, which is 512 bits. Every blood pact ID is at least `ABILITY_HEALING_RUBY` (512), for
   example `ABILITY_PUNCH` 544 and `ABILITY_PREDATOR_CLAWS` 598. So `hasBit` logs "value (N) is out of range" about
   1,000 times a minute, and summoner bots never use a pact. The core stores pacts in `m_PetCommands`, offset by
   `ABILITY_HEALING_RUBY` (`charutils::addPetAbility(PChar, id - ABILITY_HEALING_RUBY)`), and reads them with
   `charutils::hasPetAbility(PChar, id - ABILITY_HEALING_RUBY)`.
   Fix: in `tryAbility`, when `id >= ABILITY_HEALING_RUBY`, check `charutils::hasPetAbility(PChar, id - ABILITY_HEALING_RUBY)`,
   otherwise keep `charutils::hasAbility(PChar, id)`. Look for any other `hasAbility` call in this folder that can be
   passed an ID of 512 or more, and route it the same way, using a small helper if there is more than one.
2. **With ACTIVE_EVERYWHERE, bots away from a real player never form parties, so they never pull.**
   `bots.ACTIVE_EVERYWHERE` is read only at the `active` line in the tick loop. `formParties` takes only bots where
   `occupied.contains(PChar->getZone())`. Fix: in `formParties`, also take the bot when
   `settings::get<bool>("bots.ACTIVE_EVERYWHERE")` is true, the same way the `active` line does. Read the setting once
   per `formParties` call, not once per bot. Do not change `askSeekers`. With the setting off, behaviour must be exactly
   as now.

Rules: existing code style, no reformatting, no unrelated changes. You can't compile here, so only use functions,
types and members that already appear in this code or are named above, and list every assumption. Write
`JOB_N_NOTES.md`: what changed (file and function), the cause of each, and a test plan for the test server (which log
lines show it working: no "hasBit ... out of range" errors after login, a summoner bot using a pact, and
"camps at" / "is going for" / "pulled" lines in zones with no real player). Edit only files in this folder.
