# Job R: bots gather at HELM points (test server, 2026-11-24)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Commits up to `5159f06` are in.
Read `bot_ai.cpp` (`think`, `sendAction`, `send`, `makePacket` uses, `finishEvent` near 125 and the battlefield
event handling, `huntTarget`, `seekPrey`, `wander` and its `ForEachNpc`, `walkTo`), `bot_gear.cpp` (`restock` and
how it gives items with Lua `p:addItem`, and how it finds items in the inventory), `bot_citizen.cpp` (leave citizens
alone) and `settings/bots.lua.example` first.

Goal: bots on their own sometimes gather (harvesting, logging, mining, excavation) at the HELM points in their zone,
the way players do. This lays the ground for crafting later.

What the server does (from the LSB tree; you can't see it, so trust these):
- A point is an NPC named `Harvesting Point`, `Logging Point`, `Mining Point` or `Excavation Point`. When a point
  relocates, it is hidden for about 30 s and then moves. A hidden NPC refuses trades.
- A player trades exactly one tool to the point. Tools: sickle 1020 for harvesting, hatchet 1021 for logging,
  pickaxe 605 for mining and excavation. The server's Lua picks and adds the item, may break the tool, and in most
  zones starts event 901. The item is already given before the event, so the bot can end the event right away,
  the same way `finishEvent` ends the battlefield menu. While the event is up, the server refuses the next trade.
- The trade packet is client 0x036, struct `GP_CLI_COMMAND_ITEM_TRANSFER`. Fields: `uint32 UniqueNo` (the NPC id),
  `uint32 ItemNumTbl[10]` (quantities), `uint8 PropertyItemIndexTbl[10]` (inventory slot indexes),
  `uint16 ActIndex` (the NPC targid), `uint8 ItemNum` (how many entries, 1 here). The header is
  `map/packets/c2s/0x036_item_transfer.h`; include it the way the other c2s packet headers are included. The NPC
  must be within 6.0 yalms, and the bot must not be in an event.
- Starter zones with points: West Sarutabaruta (115) has harvesting, East Ronfaure (101) has logging. Many other
  field zones and dungeons have points too, so find them at runtime by NPC name, not from a list.

Settings (add to `settings/bots.lua.example` with comments like the others):
- `GATHERING = false`: off means behaviour is exactly as now.
- `GATHER_CHANCE = 15`: percent chance, each time a solo bot has nothing to hunt (where `seekPrey` would run), that
  it goes gathering instead, if a visible point is within 80 yalms.

Do this:

1. **Who gathers.** Only a bot with no party, not a citizen, not in a real player's party, not fighting or resting,
   and in a zone that has a visible point within 80 yalms. Pick the nearest visible point.
2. **The tool.** Give the bot the right tool when it starts a trip, if it has none: 12 of them, the same way
   `restock` gives items. Find the tool's inventory slot index for the packet the way the gear code finds items.
3. **A trip.**
   - Walk to within 4 yalms of the point and face it.
   - Send one `GP_CLI_COMMAND_ITEM_TRANSFER` with the tool, quantity 1.
   - Once the bot is in an event, end it with the same code `finishEvent` uses.
   - Wait at least 4 s between trades.
   - The trip ends after 8 trades, after 5 minutes, when the point is hidden (it moved) or gone, when the bot is
     attacked (it fights, as now), when it has fewer than 3 free inventory slots, or when it has no tool left.
   - After a trip, no new trip for 10 minutes.
4. **Never stuck.** If a trade gets no event within 5 s (refused, wrong NPC, too far), end the trip. If the bot is
   in an event it didn't start, leave it to the existing code.
5. **Logging.** Info lines:
   - `bots: <name> goes gathering at <point name> in <zone text>`
   - `bots: <name> gathered at <point name> (<n> of 8)`, after each event ends.
   - `bots: <name> stops gathering: <reason>`
   - `bots: <name>'s <tool> broke`, when its tool count drops by one after a trade.

Rules: existing code style, no reformatting, no unrelated changes. With `GATHERING = false` nothing changes. You can't
compile here, so only use functions, types and members that already appear in this code or are named above, and
list every assumption. Write `JOB_R_NOTES.md`: what changed (file and function), and a test plan for the test
server. The plan should cover a solo bot in East Ronfaure or West Sarutabaruta, which log lines show it working,
and how to check a gathered item in the bot's inventory in the DB (char_inventory). Edit only files in this folder.
