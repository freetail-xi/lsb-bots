# Job N notes

Could not compile here (server headers are not in this folder).

## 1. Blood pacts never fire

**`bot_combat.cpp` `knowsAbility` (new), used by `tryAbility`. `bot_brain.cpp` `canDo`.**

`tryAbility` asked `charutils::hasAbility`, which reads `m_Abilities` (512 bits). Every blood pact id is at least `ABILITY_HEALING_RUBY` (512): Punch is 544, Predator Claws is 598. `hasBit` then logs `value (N) is out of range` and the check fails, so `pets` never gets past the pact list in `bloodPacts`. The core stores those commands with `charutils::addPetAbility(PChar, id - ABILITY_HEALING_RUBY)` and reads them with `charutils::hasPetAbility`.

`knowsAbility` uses `hasPetAbility(PChar, id - ABILITY_HEALING_RUBY)` when `id >= ABILITY_HEALING_RUBY`, and `hasAbility` otherwise. `tryAbility` is the path `pets` uses for the pact, and also the path a one-shot order takes (`OrderKind::Ability`).

The other call that can be given an id of 512 or more is `canDo`. `findAbility` walks every id `GetAbility` returns, up to `MAX_ABILITY_ID`, so a named pact ("punch", "predator claws") becomes `wanted.ability`. `canDo` now uses the same helper. A party line asking for a pact no longer hits `hasBit`, and a summoner who has the command is not told that nobody can.

Left on `charutils::hasAbility`: `ABILITY_PROVOKE`, `ABILITY_PIANISSIMO`, and the roll range `ABILITY_FIGHTERS_ROLL` through `ABILITY_TACTICIANS_ROLL` (the roster loop in `bot_brain.cpp`, and the roll check in `upkeep`). Those are job abilities. See assumptions.

## 2. Bots away from a real player never party, so they never pull

**`bot_module.cpp` `formParties`.**

`bots.ACTIVE_EVERYWHERE` was read only on the `active` line in the tick loop. `think` then runs, but `formParties` still required `occupied.contains(PChar->getZone())`. With no real player in the zone, nobody was grouped, and an unpartied bot does not pull.

`formParties` now reads `settings::get<bool>("bots.ACTIVE_EVERYWHERE")` once per call, after the `BOT_PARTIES` check, and takes the bot when that is true or the zone is occupied. `askSeekers` is unchanged. With the setting off, the condition is `false || occupied.contains(...)`, which is the old check.

## Assumptions

- `charutils::hasPetAbility` exists and takes the pet-command index (`id - ABILITY_HEALING_RUBY`), matching `addPetAbility` as named in the job. The difference is passed through as the job wrote it.
- `ABILITY_HEALING_RUBY` compares with a `uint16` the same way `ABILITY_FIGHTERS_ROLL` already does in this file.
- `ABILITY_PROVOKE`, `ABILITY_PIANISSIMO`, and every id from `ABILITY_FIGHTERS_ROLL` to `ABILITY_TACTICIANS_ROLL` are below 512. If a roll id were ever that high, the roster loop and the `upkeep` roll check would still call `hasAbility` and still log `hasBit`.
- `ability::GetAbility` returns blood pacts (the existing `tryAbility` path already calls it with those ids), and `MAX_ABILITY_ID` is above 512, so `findAbility` can return a pact id. That is why `canDo` goes through `knowsAbility`.
- `settings::get<bool>("bots.ACTIVE_EVERYWHERE")` is the same read as the `active` line. `withPlayer` was not added to `formParties`. With the setting off, a bot still has to be in an occupied zone.
- No new log line was added for a pact. The core's `hasBit` line is the one that stops. A pact that fires is the ability packet `useAbility` already sends (`JobAbility`).

## Test plan

`bots.ACTIVE_EVERYWHERE = true`, `bots.BOT_PARTIES` left on, a summoner in the pool. Log in and let a tick pass.

1. After bots are online, the map log has no `hasBit` / `value (N) is out of range` lines. Before the fix those ran about a thousand times a minute once a summoner was in a fight with an avatar out.
2. A summoner in a fight, avatar out (Ifrit, Garuda, Titan, or Carbuncle) and MP at 30% or more, uses a pact: Punch, Double Punch, or Flaming Crush; Claw or Predator Claws; Rock Throw, Rock Buster, or Megalith Throw; Poison Nails or Meteorite. That is the `JobAbility` packet from `useAbility`. It should repeat on the ability recast, not only once.
3. In a zone with no real player, unpartied bots still group: `bots: <name> is forming a party of N in zone <id>`. Then the pull lines, which were missing there: `bots: <leader>'s party camps at X,Z`, `bots: <name> is going for <mob> (level ..., ... yalms from camp, back in ~...s)`, and `bots: <name> pulled <mob>`.
4. With `ACTIVE_EVERYWHERE` false, a zone with no real player does not get `is forming a party`, and does not get `camps at` / `is going for` / `pulled`. A zone a real player is in still does, as before.
