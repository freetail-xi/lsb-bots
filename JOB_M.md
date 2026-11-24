# Job M: bots hold conversations over shout (test server, 2026-11-24, build #12)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Read `bot_chat.cpp` (`handleChat`,
`respond`, `ask`, `chatter`, `partyShout`, `isBroadcast`, `mentions`) first. Commits up to `70c043e` are in.

Build #12 logs every line a bot sends. From 01:17 to 01:18 in Southern San d'Oria, three bots made 11 shouts in 57
seconds, each answering the last (shout chatter is meant to be about one per CHAT_SHOUT_MINUTES = 10 per bot):

```
01:17:17 Primrose (shout): Three, two, one... sorry. That chant is no better than a nuke.
01:17:19 Hotuhato (shout): I don't know what that means, but I'm looking for a party to clear my debt!
01:17:21 Hotuhato (shout): I don't know that chant. I just want to clear my spellbook debt.
01:17:25 Primrose (shout): Hotuhato, debts are cleared by gold, not spells. Need a healer?
01:17:37 Primrose (shout): Mouth hugs? Bah! We hunt for stew, not that. Who pulls?
01:17:39 Hotuhato (shout): Gold? I have none, only this diary of rabbits! I don't need hugs.
01:18:02 Hotuhato (shout): Sarutabaruta grasslands are rich! But first, who pulls?
01:18:02 Isolde (shout): im terribly sorry. who pulls?
01:18:04 Isolde (shout): im terribly sorry. inc??
01:18:08 Hotuhato (shout): Isolde, you pull? I'll cast Stone while we wait for stew!
01:18:14 Primrose (shout): Three, two, one... sorry. Who pulls the puller?
```

Note Hotuhato twice in 2 s (01:17:19, 01:17:21) and Isolde twice in 2 s: one heard line is answered more than once,
or two heard lines in a row each get a reply.

Fix these:

1. **No bot-to-bot shout replies.** A shout or yell from another bot (a pool character, `isPoolChar`) is never answered
   by a bot with a model line. Find the path that answers broadcast lines when a bot is named (`respond`, the `named`
   branch) and when not, and skip a broadcast whose sender is a pool bot. Shouts from a real player keep working as
   now (party invites, a named bot answering).
2. **A bot answering a real player's shout answers by tell, not shout**, unless the code already has a reason to shout
   back (e.g. a party invite callout). One reply per heard shout per bot.
3. **One reply per bot per heard line.** Find why Hotuhato and Isolde each sent two lines 2 s apart and stop it (e.g. two
   queued requests for the same bot from the same tick or the same speaker within a few seconds: keep the newest).
4. **Shout rate cap.** As a backstop, a bot sends at most one model-written shout per CHAT_SHOUT_MINUTES; extra shout
   replies are dropped with a debug log. Canned `partyShout` lines keep their own timer.

Rules: existing code style, no reformatting, no unrelated changes. You can't compile here, so only use functions,
types and members that already appear in this code, and list every assumption. Write `JOB_M_NOTES.md`: what changed
(file and function), the cause of each, and a test plan for the test server (which log lines show it working). Edit
only files in this folder.
