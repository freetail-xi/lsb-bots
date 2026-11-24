# Job S notes

## What changed

Citizen stops and town strolls skip machines, objects, and later-era systems. The names live in one array, `kSkipNpc` in `bot_citizen.cpp`. `skippedNpc` matches a pretty name against that list. `pickStop` never chooses a skipped NPC. `{name}` lines are used only when that stop's name passes the same filter and is one or two words. `wander` uses the same filter when it picks a town NPC to stroll to.

`/heal` (camp mode On) is not sent while `PChar->PAI->IsEngaged()` or `PChar->GetBattleTarget()` is set. `setResting` drops that packet. `rest` skips the tick instead of resting. `restInFight` (WHM/BLM, on purpose) sends disengage first and camps only once the bot is no longer engaged and has no battle target; until then it returns true so the fight loop does not attack again. Camp mode Off (standing up) is unchanged.

## Assumptions

- A list entry matches as a whole word or phrase, case-insensitive, using `saysPhrase`: a letter immediately before or after the entry blocks the match. "Synergy Furnace" matches Synergy and Furnace. "Home Point #1" matches Home Point and Point. "Protocrystal" does not match Crystal. "Fountain" does not match Fount. "Encounter" does not match Counter.
- The name is passed through `prettyName` first, so underscores are spaces. "Synergy_Furnace" and "Home_Point" match.
- "???" matches a name that contains those three characters with a non-letter on either side, including a name that is only "???".
- One or two words means one or two runs of non-space characters. An apostrophe does not split a word, so "d'Oraguille" is one word and "San d'Oria" is two.
- A townsfolk name of three or more words can still be a stop. `{name}` lines are not used for it.
- `placeCitizen` still stands a new citizen beside any visible named NPC, including one this filter would skip. That choice is a coordinate, and it is not logged as "walks to". `sayAddressedToOther` still treats those NPCs as someone a player can address.
- Town `wander` already required Normal status, a visible name, and 100 yalms. It now also requires `skippedNpc` to be false. It does not use the citizen `hasLetter` test. Outside cities, wander still roams around home and does not pick NPCs.
- The only camp-on sends were `setResting(..., true)` from `rest` and `restInFight`. No other caller starts `/heal`.
- `GetBattleTarget()` is null once disengage has been applied. Idle bots already have a null battle target. If a target stayed set after a successful disengage, `rest` would keep skipping and a WHM or BLM in `restInFight` would keep waiting instead of fighting.
- Disengage is the existing `disengage` helper (AttackOff on the bot). If that packet has not cleared Engaged yet, camp-on waits for a later tick. `restInFight` still returns true on that tick so `think` does not send Attack again.
- `rest` does not disengage. When the fight target is already gone, `think` still sends AttackOff on its own before `rest` runs. If that has not landed, `rest` skips this tick and the bot continues with follow, hunt, or wander.
- This tree was not compiled. The patch uses `prettyName`, `lowerCase`, `saysPhrase`, `IsEngaged`, `GetBattleTarget`, `disengage`, and the existing camp packet only.

## Test plan

- Citizens in a city that has a Synergy Furnace, Survival Guide, Home Point, door, gate, moogle, and "???". Logs of `walks to` name townsfolk or home, and never those objects. Logs of `(citizen) says:` never greet them ("Good morning, Synergy Furnace." does not appear). A one- or two-word shop NPC can still be greeted by name. A three-word townsfolk name can be walked to, and the `{name}` lines are not used there.
- Another bot strolling in town does not path onto a Synergy Furnace or the other skipped names.
- Solo Rodney and Plod, low HP after a fight, and a WHM or BLM low on MP during a fight: the map log has no `Invalid GP_CLI_COMMAND_CAMP` / `Invalid state: Engaged`. The mage disengages, then `/heal`s. Standing up when full, when the leader walks off, or when the mage is hit still happens.
