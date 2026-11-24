# Job L notes

Could not compile here (server headers are not in this folder).

## What actually stopped the pulls

Evodie was the party leader and the puller. `think` passes `leaderOf()` into `pull`. `leaderOf` returns null when the bot is the leader, so it does not follow itself. `pull` treated that null as "no leader" and returned before it looked for a mob.

That is why 00:36 through 00:50 has `setPuller` and then nothing: no `is going for`, no `pulled`, no `pull by ... ended`, and no `waits:`. It stayed silent after 00:40:23 as well. Forest Funguar is level 4-5, about 53 yalms out, which is inside the 180-yalm limit and inside the old level band for a level 5 party. A named mob is also pulled at any level. If `pull` had been running, that funguar would have been a legal target. The level filter alone cannot explain the silence.

`pull` now uses the party leader when `leaderOf` is null. A leader who is pulling measures camp from herself.

## 1. Low-level real player

**`bot_pull.cpp` `levelOf`.**

The old function was the highest `GetMLevel` in `PParty->members`. This party is 5, 4, 5, and 1, so the reference was 5. `kWeakestGap` is -3, so the usual pick wants mob level 2 or higher. A level 1 rabbit is gap -4, which fails that pass. The fallback pass (`kFallbackGap` -9) within 50 yalms would still have taken it, once `pull` ran. The usual pass would also have preferred a level 2-8 mob within 50 yalms over those rabbits.

With a real player in the party, `levelOf` is now the lowest member. A real player is `TYPE_PC` and not `isPoolChar`, the same test used everywhere else in this module. Bot-only parties still use the highest. Trusts are not in `members`, and a trust does not count as the real player.

For Ddd at 1, in a party of 4, `toughestGap` is still +3. The usual band is mob levels 1 through 4. Level 5 is too strong unless it was named. The job's "lv1-3" is the prey around that camp (rabbits, worms, and bats at 1; hares and worms at 2-3). Level 4 still passes the existing +3 cap. No new cap was added.

## 2. "pulls only Forest Funguar"

**`bot_pull.cpp` `pullChoice`, and the waiting branch of `pull`.**

The log line is only printed in `pullChoice`. The list is set when the line names a mob in the zone and also contains, as whole words, `only`, `just`, `pull`, `pulls`, `pulling`, `stick to`, or `go for` (or an avoid phrase, which logs `leaves alone` instead).

`I'll charm the Funguar` names Forest Funguar and does not contain those words, so that exact sentence does not set the list. These do: `I'll just charm the Funguar`, `I'll go for the Funguar`, `funguar only`, `pull the funguar`. `I'm going for the Funguar` does not, because `go for` is not a whole-word match inside `going for`.

`a rabbit a worm, I don't care` names the rabbits and worms, and `care` is one typo from `hare`, but it has no trigger phrase, so it did not set this list. Typo matching was left as it was.

The sentence from 00:40:23 is not in this folder. In `handleChat`, a speaker whose name is in the pool name set is skipped before `pullChoice`, same as the other orders. A pool bot is now also rejected inside `pullChoice` with `isPoolChar`. That is the character-id form of the same check, so a chat name that misses the pool name set cannot set the list either. A real player in the party still can. `pull anything` from a bot no longer clears a list.

If a restriction has left `pickTarget` with nothing for 2 minutes, it is erased. The log is `bots: <leader>'s party drops its pull restriction (<Pulling only ...>) after 2 minutes with nothing in reach`. The puller queues `Nothing in reach for that, so I'll pull whatever's here.` through `callout`, which is the existing party-line path, and then picks again with no list. `callout` sends nothing when `withPlayer` is false or the bot was told to be quiet. The info line is still written. Time the leader spends away from camp still counts toward the 2 minutes, because the clock is not paused on that early return.

A named mob that is up and within 180 yalms is still pulled at any level. The 2 minutes start only when the search finds nothing.

## 3. Nothing to pull

**`bot_pull.cpp` `noTargetReason`, called from `pull`.**

When the puller is at camp, not held, and the search finds nothing, one info line:

`bots: <leader>'s party has nothing to pull: <reason>`

Reasons, in the order they are chosen: `no mob in range`, `restricted to <names>`, `claimed`, `all too weak`, `all too strong`. At most once a minute per party, on the `Pull`. `beside a player` is counted as `claimed`. An avoid list that removes every mob in reach, and a mob that still looks pullable but was given up on, are logged as `no mob in range`. The line is also written when the party is waiting on HP. The old `waits:` line is unchanged.

## 4. What bots say

**`bot_chat.cpp` `say`.** Removed the `bots: <name> shouts:` line in `chatter`.

Every line `say` sends is one info line:

`bots: <name> says (<party|tell to X|shout|linkshell|say>): <text>`

Tells use `tell to <name>`. Shouts go through `say` when they are sent, so the old chatter line would have been a second copy. Yell, unity, and assist use `channelName` (`yell`, `unity`, `assist`). Callouts (`Pulling ...`, the restriction line, `waits:` party text) are party lines and are logged here when they are sent.

## Assumptions

- `leaderOf` returning null for the leader is intentional for following, and was the wrong signal for pulling.
- `isPoolChar` is the same pool as the names `handleChat` already skips.
- `levelOf` still walks `PParty->members` only. A lower-level trust does not move the reference.
- The existing gap constants were not retuned. Level 1 with a party of 4 allows mob levels 1-4, not a hard stop at 3.
- The 00:40:23 text itself is not in this folder. Only the `pullChoice` conditions above produce that log line.
- `callout` may not speak if the bot is quiet or no longer `withPlayer`.

## Test plan

1. Same party: Evodie BST 5 leads and pulls, Isolde WHM 4, Plod MNK 5, Ddd WAR 1, camp in West Ronfaure near the level 1 rabbits. After `pull`, the map should show `Evodie is going for` a level 1 rabbit, worm, or bat within a few seconds, then `pulled`. Not silence, and not a level 4-5 funguar while level 1s are closer.
2. Walk Evodie off camp. `waits: Holding pulls while Evodie is away from camp.` Come back and she pulls again.
3. A bot-only party around level 5, with a level 5 mob and a level 1 mob both within 50 yalms, still prefers the level 5. No real player, so the old highest-level rule stays.
4. From Evodie, party chat `I'll charm the Funguar` and `I'll go for the Funguar`. No `pulls only` line. From Ddd, `funguar only`. One `pulls only Forest Funguar`, and she goes for it (`asked` still ignores level).
5. From Ddd, `pull only` a mob that is not up or not within 180 yalms. For 2 minutes, once a minute: `has nothing to pull: restricted to ...`. Then `drops its pull restriction` and, in party chat, `Nothing in reach for that, so I'll pull whatever's here.` That party line is also `says (party):`. She then pulls a rabbit.
6. Camp with nothing spawned within 180 yalms. `has nothing to pull: no mob in range` once a minute, not every tick. Claim every nearby rabbit and the line becomes `claimed`. A camp of only level 5+ mobs, with Ddd at 1, becomes `all too strong`.
7. A bot reply, a tell, a linkshell line, and an LFP shout each produce one `says (...)` line. The shout is not also `bots: <name> shouts:`.
