# Job T notes

## What changed

Geomancer colures fall through to a spell the level can cast. Indi-Refresh and Geo-Refresh are first only when another party member has MP. A solo geomancer, or one grouped with jobs that have no MP, uses Indi-Fury, then Barrier, Regen, Precision, Voidance and Poison, and the same order of Geo- spells without Refresh. Full Circle is unchanged: only with a luopan out, no target, and 15 seconds after the last fight. `pets()` returns before the wyvern, automaton, jug and avatar paths, so a luopan is not sic'd, healed or dismissed with Release.

A geomancer with no party stands in melee, weapon skills the club, and still nukes through the existing `nuke()` list (Stone through Thunder III). In a party it stays at caster range. GEO/WHM cures at 60% HP when the party has no WHM, RDM or SCH, including while travelling at the emergency line, and is the party's healer in that case. Any other geomancer sub still only cures in an emergency.

Rune Fencer no longer counts runes with `HasStatusEffect`. Three of one element counted as one, so it recast Tellus forever and never reached Swordplay. Runes are counted with `GetStatusEffectCountInIDRange` from Ignis through Tenebrae. The next rune walks Ignis, Gelus, Flabra, Tellus, Sulpor, Unda, Lux, Tenebrae, starting from the target's id, so the set is not one element. While it is the one being hit it uses Valiance or Vallation, then Liement, Pflug under 70% HP, Battuta, and One for All under 40% HP. Foil and Flash are used when it is supposed to hold hate (main tank, or no party). The main tank runs that before Provoke. An off-tank also has Foil on the grab path. Default subjob is still WAR.

The gear picker already prefers a great sword. Adaman Claymore and the other 75-era great swords in this server's item table have jobs mask 2097345, which includes Rune Fencer (bit 21). If none is usable, the next choice is a sword. A sword is not chosen while any great sword fits.

Party forming and seeker invites already treat RUN as a tank. GEO now ranks as support, under WHM and under RDM/SCH, so a party that needs support can take one and a group at 15 or higher can form around a geomancer. Job name and abbreviation tables already include Geomancer/GEO and Rune Fencer/RUN, including `!bots` and the looking-for-party shout.

## Assumptions

- Spell levels are the GEO byte of `spell_list.jobs` on this server: Indi-Poison 1, Indi-Voidance 4, Indi-Precision 10, Indi-Regen 15, Indi-Barrier 28, Indi-Refresh 30, Indi-Fury 34; Geo-Voidance 8, Geo-Precision 14, Geo-Regen 19, Geo-Barrier 32, Geo-Refresh 34, Geo-Fury 38. `castBest` skips a spell `CanUseSpell` rejects, so a lower one is what a low level casts.
- Geo-Poison is an enemy spell (valid target 4). It is not on the luopan ladder, which is cast on the tank or the geomancer. Indi-Poison uses the same self target as Indi-Regen (513).
- `kNukes` is the geomancer nuke list. GEO learns Stone at 4 through Thunder III at 73; Stone IV is 76, so a level 75 cap never reaches it. `castNuke` already refuses a spell the job cannot cast. The -ra spells (Stonera and the rest) are not in this module's spell list and were not added.
- `CMobEntity::m_Element` is the crystal the mob drops (`getCrystalElement`), not a weakness. This module never reads a weakness table, so runes are not chosen from it. They rotate. All eight rune abilities are learned at 5 and share recast id 10. Capacity matches `rune_fencer.lua`: 1, then 2 at 35, then 3 at 65. A new rune past that replaces the oldest, so the bot stops at `most`.
- Vallation is 10, Swordplay 20, Pflug 40, Valiance 50, Battuta 75 and a merit, Liement 85, One for All 95. `tryAbility` does nothing when the ability is not learned, so Battuta, Liement and One for All are no-ops under the level or without the merit. No status-effect names were added for those three; recast stops a repeat.
- A luopan is a pet (`PETID_LUOPAN`, `TYPE_PET`). Logout only despawns a pet whose objtype is `TYPE_MOB`, which a luopan is not. `rest()` still requires `PPet == nullptr` before it treats the owner as a caster, so a geomancer does not /heal until Full Circle puts the luopan away.
- "Someone else spends MP" is `GetMaxMP() > 0` on a party member in spell range, not the geomancer. A paladin counts. A monk does not. The geomancer itself does not.
- GEO/WHM is a healer only as main GEO and sub WHM. Seeker invites and party forming look at the main job only, so any geomancer counts as support there. The usual subjob is still WHM (`defaultSubJob`). RUN still falls through to WAR.
- Great sword job bit: mask 2097345 is WAR + PLD + DRK + RUN. `usable()` requires that bit, req level, ilevel 0, and race. Rusty Greatsword is level 1 on the same mask, so a rune fencer is not left unarmed when the 70 great sword is too high. Sword is rank 2 in `preferredSkills` and is used only when no great sword passes `usable()`.
- `!bot` prints the job as its number for every job (GEO 21, RUN 22). The name tables used by chat and `!bots` already had both jobs. Nothing there was short of the two new jobs.
- This tree was not compiled. New names are ones this module already includes: `SpellID` values from `spell.h`, `ABILITY_IGNIS` through `ABILITY_TENEBRAE`, `ABILITY_LIEMENT`, `ABILITY_BATTUTA`, `ABILITY_ONE_FOR_ALL` from `ability.h`, and `GetStatusEffectCountInIDRange`, which maneuvers already call.

## Test plan

- A solo GEO in a camp: the map log has no spell or ability errors. It engages (club swings, a club weapon skill once TP is up) and the battle log shows an Indi- spell it can cast (Fury, Barrier, Regen, Precision, Voidance or Poison, never Refresh) and, in a fight, a Geo- spell of the same kind. After the fight, about 15 seconds of quiet, then Full Circle. No Fight, Deploy, Assault or Release on that pet.
- A GEO/WHM in a party with no WHM, RDM or SCH: cures around 60% HP, and `!bot` "Heal wanted" goes yes when someone is under that. With a WHM in the party it does not take the cure. Indi-Refresh or Geo-Refresh appears when a partner has MP, and not when the partner is a monk.
- A RUN at 65 or higher on a mob: the battle log shows more than Tellus (the eight runes, three at a time), then Swordplay, then Vallation or Valiance while it is the target, and Foil if it is the main tank. `!bot` hate says the mob is on the RUN, or "clear" when another tank is holding it. Pflug only shows under about 70% HP. No error about an unknown ability. Battuta only if the merit is on the character; Liement and One for All do not show at 75.
- Gear: a level 75 RUN has a great sword in the main hand (Adaman Claymore or another ilevel-0 great sword), not a sword, and an empty sub. A level 1 RUN still has a great sword (the rusty one).
- A party with an open support slot invites a seeking GEO ahead of a damage dealer. A party with an open tank slot invites a seeking RUN. `!bots` and an LFP shout name them GEO and RUN.
