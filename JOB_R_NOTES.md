# Job R notes

Could not compile here (server headers are not in this folder). With `GATHERING = false` the new branches are skipped. Hunt, `seekPrey`, wander, party, and combat stay as they are. `restock` only adds a tool when a trip has asked for one, and it only skips the loot purge while a trip is set, so a bot that never gathers is purged on the same rule as before.

Copy `GATHERING` and `GATHER_CHANCE` from `settings/bots.lua.example` into the live `settings/bots.lua` before testing. A missing key fails the same way as any other missing `bots.*` setting.

## What changed

**`settings/bots.lua.example`.** `GATHERING` (false) and `GATHER_CHANCE` (15).

**`bots.h`.** `Gather` is the trip. `Bot::gather` is empty when the bot is not on one. `Bot::nextGather` is the earliest time another trip may start. `kGatherTools` is 12.

**`bot_gear.cpp` `restock`.** If `gather->giveTool` is set, it runs the same Lua `p:addItem({ id, quantity = 12, silent = true })` path as the medicine stacks, on the next tick (restock runs before `think`), and not on the 20 second throttle. While `gather` is set, the bag is not purged. After the trip, a bag with fewer than 5 free slots is purged again as before.

**`bot_ai.cpp`.**

- `startGather` / `onGatherTrip` / `finishGather` / `stopGather`. Called from `think` in the solo branch, at the `seekPrey` call.
- A trip trades with `GP_CLI_COMMAND_ITEM_TRANSFER` (`map/packets/c2s/0x036_item_transfer.h`): one stack slot, quantity 1, the point's id and targid.
- While `gather->waiting` is set, an event that is not the battlefield menu (32000) is ended with `finishEvent`. Any other event still goes through `answerEntry` / `finishEvent`.
- Death, a party, a real player's party, an attacker, and resting each end the trip.

Points, matched at runtime by `prettyName` of the NPC: Harvesting Point (sickle 1020), Logging Point (hatchet 1021), Mining Point and Excavation Point (pickaxe 605). A point counts as visible with the same test `wander` uses: `status == Normal` and the name is not hidden.

## Assumptions

- NPC names pretty-print to exactly `Harvesting Point`, `Logging Point`, `Mining Point`, or `Excavation Point` (`Harvesting_Point` does). The log uses those words.
- "Visible" is that same Normal / not-name-hidden test. A relocating point fails it and the trip ends `point hidden`. No entity with that id ends it `point gone`.
- The nearest such point within 80 yalms is chosen once, at the start, and the trip stays on that NPC id. Its live position is what the bot walks to. A tie keeps the first one `ForEachNpc` yields.
- The trade is legal at 4 yalms because the server allows 6. `distance` is the same helper the rest of the AI uses. The bot faces the point with `PathFind->LookAt`.
- The roll is once per hunt search that found nothing (`huntTarget` actually ran this tick), at the `seekPrey` site. It is not once per follow tick. `huntTarget` also reports nothing when HP is under 80, MP under 50, or the zone is not outdoors or a dungeon; those empty searches can roll too. `HUNT` false never starts a trip, because `seekPrey` is not called. A trip already underway still runs.
- `GATHER_CHANCE` 0 never starts. 100 starts on every empty search that has a point in range. The draw is `GetRandomNumber(100)` against the percent, the same shape as `CAMP_CHANCE`.
- Only a bot with no party, `withPlayer` false, not a citizen, not engaged, and not under Healing. Fewer than 3 free inventory slots does not start a trip (no start line, no 10 minute lockout). Citizens never reach this branch.
- A trip does not stop to chase a hunt target. It does stop when something is already on the bot (`attacked`), when it rests (`resting`), when it joins a party (`joined a party`), or when it dies (`died`). Those four reasons are in addition to the ones named in the job. Each end, including those, waits 10 minutes.
- The tool is given only when the count is 0 at the start. `charutils::getItemCount` is that count (the call `restock` already uses). The packet slot is `SearchItem` on `LOC_INVENTORY`, the same index `useItem` puts in `PropertyItemIndex`. One stack of 12 fits these tools; `addItem` of 12 fills one slot.
- The give lands the tick after the trip starts, because `restock` runs before `think`. The bot walks in the meantime and does not treat "no tool" as fatal until that give has been attempted.
- A break is the tool count dropping by exactly one between the send and the event end (or the 5 second timeout). A drop of any other size is not logged as a break.
- Most zones open an event (often 901) inside the trade packet, before `send` returns, and the yield is already in the bag. The bot calls `finishEvent` immediately in that case. If the event is only visible on the next tick, the top of `think` ends it. The event id is not checked. Any event while a trade is outstanding, other than the battlefield menu, is this gather.
- An event the bot did not trade for, including event 32000, is left to the existing `answerEntry` / `finishEvent` path.
- No event within 5 seconds ends the trip (`no event`), including a refused trade, the wrong NPC, or standing too far. The bot does not retry that trip.
- Trades are at least 4 seconds apart, measured from when the event ended. The first trade can go out as soon as the bot is in range and holding a tool.
- The trip also ends at 8 finished events (`8 trades`), at 5 minutes (`5 minutes`), below 3 free slots (`fewer than 3 free slots`), or at 0 tools (`no tool left`).
- While the trip runs, `think` does not call `seekPrey` or `wander`. A body that needs a raise, an order, or a pull still comes first, and the clock keeps running. In a zone where the bot is idle, the event is still closed, and the walk resumes on the next active tick if the 5 minutes are not up.
- `restock`'s purge (under 5 free slots) does not run during the trip, so the tool and the yield stay. After the trip, that purge is back. A normal 80-slot bag does not hit it.
- Zone text is `campZoneText`: the prettied zone name (`East Ronfaure`, `West Sarutabaruta`).
- `uint8` counts in the log are cast to `int` so they print as numbers.
- `char_inventory.location` 0 is the inventory, and the item column is `itemId`. The HELM reward and `addItem` both write that table. If a row is missing right after the log, the character save has not flushed yet.

## Test plan

Merge the two settings into `settings/bots.lua`. For a visible pass set `GATHERING = true` and `GATHER_CHANCE = 100`. Leave `HUNT = true`. Use one bot that is not a citizen and not in a party. It has to be active: a real player in the zone, or under `SOLO_BELOW` with `SOLO_ACTIVE` in a starting area, or `ACTIVE_EVERYWHERE`. East Ronfaure (101) is logging. West Sarutabaruta (115) is harvesting.

Put it within 80 yalms of a visible Logging Point or Harvesting Point, and with nothing it would hunt within 30 yalms (no unclaimed normal mob near its level). A GM `setpos` of that character next to the point is the reliable way. The hunt search runs about every 8–25 seconds. With `GATHER_CHANCE = 100` the next empty search starts the trip.

Working, in the map log:

- `bots: <name> goes gathering at Logging Point in East Ronfaure` (or `Harvesting Point` / `West Sarutabaruta`). Once, not per tick.
- `bots: <name> gathered at Logging Point (1 of 8)` after each event, then 2 of 8, up to 8. At least 4 seconds between them.
- `bots: <name>'s hatchet broke` (or `sickle`) only when that stack's quantity fell by one.
- `bots: <name> stops gathering: 8 trades` after the eighth, or `5 minutes`, `point hidden`, `point gone`, `no event`, `fewer than 3 free slots`, or `no tool left`.
- Aggro during the trip: it fights, and the line is `stops gathering: attacked`. No second trip for 10 minutes (`goes gathering` stays quiet).

Inventory. Before the trip, and again after a `gathered` line:

```sql
SELECT b.name, i.location, i.slot, i.itemId, i.quantity
FROM char_inventory i
JOIN bot_pool b ON b.charid = i.charid
WHERE b.name = '<name>' AND i.location = 0 AND i.quantity > 0
ORDER BY i.slot;
```

East Ronfaure: a hatchet row, `itemId` 1021, quantity 12 unless one broke. West Sarutabaruta: sickle 1020, same. A new `itemId` that was not in the earlier snapshot is the yield. If the row is absent, zone or log the bot out so the save runs, and query again. Do this while the bag still has 5 or more free slots; after the trip, `restock` deletes non-equipment below that.

`GATHERING = false` (and a restart so no trip is mid-flight): no `goes gathering`, `gathered at`, `stops gathering`, or `broke` lines. A solo bot with nothing to hunt seeks prey and wanders as before. No hatchet or sickle is added.
