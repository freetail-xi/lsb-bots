# Job M notes

Could not compile here (server headers are not in this folder). All of the edits are in `bot_chat.cpp`.

## What the log was

From 01:17:17 to 01:18:14 in Southern San d'Oria, Primrose, Hotuhato, and Isolde shouted 11 model lines in 57 seconds. Each line answers the one before it. Hotuhato shouted twice, two seconds apart (01:17:19 and 01:17:21). Isolde did the same (01:18:02 and 01:18:04). Both of Isolde's lines start with "im terribly sorry", which is two samples of one prompt, not two different canned lines.

`partyShout` is the canned LFP line (`lfp 12 blm, ...`). None of these 11 are that shape. They came out of `ask` → `request` → the worker → `say`.

## 1. No bot-to-bot shout replies

**`handleChat`, and `respond`.**

A shout is a broadcast. `respond` answers broadcasts in the same channel: the named loop calls `ask` for every bot whose name is in the text, and the unaddressed branch calls `ask` for two listeners. `ask` copies `heard.kind`, so a shout is answered with a shout. That reply is heard by the other bots, and the loop continues.

`handleChat` also answers a sender whose name is in the pool name set, including shouts, until `kMaxBotRun` (3) on that channel. This log is 11 shouts, so that cap did not stop it. A sender whose name is not in the set is treated as a player: `botRuns` is cleared and `respond` runs with no cap.

A broadcast is now skipped when the sender is a pool bot. That is `botNames.contains(from)` or `isPoolChar` on the character `GetCharByName` returns. The skip in `handleChat` is before the bot-to-bot `ask` and before `respond`. `respond` returns at the top, before the named loop and before the unaddressed broadcast branch, if the sender is a pool character. That covers a line that reached `respond` through the brain. Say, shout, yell, unity, and assist are the broadcast kinds. Party and linkshell between bots are unchanged. A real player's shout still reaches `respond`.

## 2. A player's shout is answered by tell

**`respond` (`answer`).**

`ask` used to send on `heard.kind`. For `MESSAGE_SHOUT` and `MESSAGE_NS_SHOUT`, the reply is now `MESSAGE_TELL` to the speaker, unless `wantsParty` is true. That is the existing party-invite path (the invite packet plus the shout back). A named bot and an unaddressed shout both use this. A yell stays a yell. A say stays a say.

The named loop and the unaddressed loop each answer a given bot once for that line. The unaddressed loop still picks up to two different bots.

## 3. One reply per bot per heard line

**`handleChat` (the heard loop), `ask`, `request`, `worker`.**

Hotuhato and Isolde each sent two lines about two seconds apart. The worker runs one request at a time, then waits out a typing delay, so two requests queued together show up a couple of seconds apart. Two ways that queue happens:

- The same shout is on a bot's packet list twice. Both copies have the same kind, sender, and text, so both were pushed as listeners. The unaddressed loop then asks the first two slots, which can both be that bot.
- Two heard lines in one tick (two packet kinds, or two texts) each call `ask` for the same bot. The bot-to-bot branch only asks `listeners.front()` once per line, so a second line, or a second copy that missed the name set and fell through to `respond`, is the other request.

The heard loop now keeps one listener per bot for a given kind, sender, and text. `ask` keeps one live answer per bot: a second answer in the same `handleChat` call, or a second answer to the same speaker within 5 seconds, cancels the older one if it is still queued, still in flight, or generated but not sent. The newest is the one that is sent. A line already on the wire is not recalled. An optional answer that loses the chattiness roll does not cancel the one already queued.

The same rule applies to every `ask`, not only shouts. Two lines for one bot in one tick produce one model reply. `carryOut` is unchanged. `speakUp`, `greet`, `callout`, `tell`, and `partyShout` do not go through this.

## 4. Shout rate cap

**`say`, and `chatter` where it queues `partyShout`.**

There was no limit on model shouts. `partyShout` already waits on `nextChatter` (`CHAT_SHOUT_MINUTES`). Model replies did not.

`say` now sends at most one model shout per bot per `CHAT_SHOUT_MINUTES`. A later one is not sent. The debug line is:

`bots: <name> drops a shout, one model shout per <N> minutes: <text>`

`partyShout` is marked canned. It still uses `nextChatter`, it does not start this timer, and this timer does not block it. A setting of 0 does not drop model shouts. Yell is not capped.

## Assumptions

- `isPoolChar` is the same pool as the names passed into `handleChat`.
- `GetCharByName` finds an online bot from the name on the chat packet. If it returns null and that name is also not in the pool name set, the shout is still answered. There is no id on `Heard`.
- "Broadcast" is `isBroadcast`: say, shout, yell, unity, assist, and the NS say/shout kinds. A pool bot's say is not answered either. Party and linkshell are not broadcasts.
- A "few seconds" is 5. One tick is one `handleChat` call.
- `MESSAGE_SHOUT` and `MESSAGE_NS_SHOUT` are the shout kinds already handled in this file. Tells use `MESSAGE_TELL`, which is what `say` already sends for tells.
- The party-invite reason to keep shouting is `wantsParty` (party, pt, lfp, invite, and the other words already in that list). A shout that only names a bot, with none of those words, is a tell.
- Up to two different bots may still answer one unaddressed player shout. Each answers once, by tell, unless the line is a party shout.
- `CHAT_SHOUT_MINUTES` of 0 means no cap. The cap is checked when the line is sent, so two model shouts that come due together count as one send and one drop.
- A dropped shout does not set the conversation partner. The party invite itself is queued in `respond`, before the model returns, so a capped shout can still produce an invite.
- The debug line is `ShowDebugFmt`. It shows up when map debug logging is on.

## Test plan

Log lines are the build #12 ones from `say`: `bots: <name> says (<channel>): <text>`. Channels: `shout`, `yell`, `say`, `party`, `tell to <name>`.

1. Primrose, Hotuhato, and Isolde in Southern San d'Oria, no player chat. They must not answer each other. There should be no `says (shout)` of a model sentence, and no `says (tell to Primrose)` (or Hotuhato, or Isolde) after one of them speaks. A canned line may still appear on the chatter timer: `bots: <name> says (shout): lfp ...`. After that line, the other two stay quiet.
2. A real player shouts `hello` (no party word). Each bot that answers does it once: `bots: <name> says (tell to <player>): ...`. Not `says (shout)`. Not two lines from the same bot a couple of seconds apart.
3. The player shouts `Hotuhato hello`. Only Hotuhato answers, one tell: `bots: Hotuhato says (tell to <player>):`.
4. The player shouts `lfp` (or `invite`). A free bot close in level still invites, and that reply stays a shout: `bots: <name> says (shout): ...`. One shout per bot for that line.
5. The player shouts `hello`, then `who pulls?` within 5 seconds. Each bot sends at most one tell, for the later line. Not two tells.
6. Party chat and a tell still answer on those channels: `bots: <name> says (party):` and `bots: <name> says (tell to <player>):`. A bot's party line can still be answered by another bot, within the old run cap.
7. After a model shout (step 4), a second `lfp` inside `CHAT_SHOUT_MINUTES` does not produce another `says (shout)` from that bot. With debug logging on: `bots: <name> drops a shout, one model shout per <N> minutes: <text>`. A canned `says (shout): lfp ...` may still show inside that window.
8. A player's yell, if a bot answers, is still `says (yell)`. A bot's yell is not answered.
