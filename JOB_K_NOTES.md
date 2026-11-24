# Job K notes

Could not compile here (server headers are not in this folder). `handleChat`, `respond`, and `carryOut` were not changed.

## 1. break_camp guard

**`bot_brain.cpp` `plansFor`**, the `break_camp` branch.

`saysPhrase` is unchanged: the line is already lower-case, and a phrase counts only as whole words (the character before it is the start or not a letter, and the character after it is the end or not a letter). Still no model call.

Before:

`break`, `breaking`, `leave`, `leaving`, `move`, `moving`, `pack up`, `packing up`, `let's go`, `lets go`, `follow me`, `new camp`, `relocate`, `head out`, `heading out`, `move on`, `moving on`, `done here`, `camp elsewhere`

After, those plus:

`change camp`, `move camp`, `switch camp`, `this way`, `over here`, `over there`, `come on`, `come here`, `let's move`, `lets move`, `go somewhere else`, `somewhere else`, `find a better spot`, `better spot`, `zone line`

`move` already matched the whole word inside `move camp` and `let's move`. `let's go` and `lets go` were already in the list. They are listed again so the wording from the job sits next to the old phrases. The lines that were actually blocked were `this way`, `let's change camp` (`change camp`), and `standing at the zone line` (`zone line`).

Still no match, so the guard still drops `break_camp`:

- a map grid such as `(H-6)`
- a bare question such as `fight?`
- chat that only names a place (`we're in Valkurm`, `Batallia`)

`zone` alone is not a phrase, so `what zone` does not count. `zone line` does.

When the phrase matches, the old `break_camp` body runs: one Follow order per bot, subject is `PSpeaker->PParty->GetLeader()`, and `OrderKind::Follow` calls `breakCamp` on that leader. The party follows the current leader. It does not start following the speaker when the speaker is not the leader.

## 2. Leader is not tank

**`bot_brain.cpp` `plansFor`**, the `set_tank` branch.

If the brain calls `set_tank` and the line's role words are only leader/lead, the plan is dropped and this is logged, same shape as the camp guard:

`bots: the party brain wanted a tank set on "<line>", which is about the leader`

Leader words: `leader`, `lead`, `leads`, `leading`.

Tank words that keep the plan: `tank`, `tanks`, `tanking`, `tanker`, `mt`.

`lead` does not match inside `leader` (the next character is a letter). `tank` does not match inside `tanking` or `tanked`. So `You're the leader Evodie`, `Evodie leads`, and `Evodie is leader` drop. `You're the leader and you tank` does not drop. `I'll tank` does not drop, because there is no leader word.

There is no way in this folder for a party leader to pass the lead to a named member, so nothing is called in its place. Group packets this module sends are invite (`GP_CLI_COMMAND_GROUP_SOLICIT_REQ`), accept (`GP_CLI_COMMAND_GROUP_SOLICIT_RES`), leave (`GP_CLI_COMMAND_GROUP_LEAVE`), and breakup (`GP_CLI_COMMAND_GROUP_BREAKUP`). The comment on `leaveOrBreakUp` in `bot_ai.cpp` ("leaving passes the lead on") is the server picking a new leader when the current one leaves. `GetLeader()` is only read. No new packet was added. Whether the speaker is the current leader does not matter, because there is no handoff to run. The party leader and the main tank stay as they were. Other tool calls on the same line are still applied. If this was the only plan, the empty-plan path in section 3 answers in party chat.

## 3. no_action still reaches respond

Already the case. Not changed.

Path when the brain is on:

1. `handleChat` (`bot_chat.cpp`) calls `askBrain`. On a non-zero id it stores every listener in `waiting[id]` and does not answer yet.
2. A later `handleChat` calls `brainOutcomes` (`bot_brain.cpp`). `plansFor` has no branch for `no_action`, so that call adds no `OrderPlan`. A guard `continue` (camp, pulls, hate-as-puller, and now leader-as-tank) also adds nothing for that call.
3. Back in `handleChat`, after the `waiting` entry is turned back into listeners:

```
plans = outcome.plans
if (!outcome.answered) plans = planOrder(...)
if (plans.empty()) plans = pullerAnswer(...)
if (plans.empty()) respond(listeners)
else carryOut(plans, ...)
```

`respond` is the same function a line uses when the brain is off. Brain off is `askBrain` returning 0, then `planOrder`, then `pullerAnswer` for party chat, then the same `respond(listeners)` at the bottom of the per-message loop.

`respond` for party (`isGroup`: `MESSAGE_PARTY` or `MESSAGE_NS_PARTY`), when the line does not name a bot: one bot is asked. A `?` is not optional. A line with no `?` is optional (`feelsLikeTalking` may skip it, and the prompt allows `PASS`, which `worker` drops). That optional rule is the brain-off rule too.

`who's pulling` is the exception inside this same block. `pullerAnswer` matches `pulling`. When a bot is the puller, it returns a plan and `carryOut` asks that bot with `pullReport` (not optional). The brain summary can still say `0 job(s)`, because `pullerAnswer` runs after `plansFor`. That is also what brain-off does.

So `hello?` and `fight?` (guard drops the camp plan, zero plans left) are handed to `respond` and a reply is requested. `where are we fighting` is handed to `respond` as optional, same as with the brain off. A missing reply on a line with no `?` is that optional/`PASS` path, not a line that never reached `respond`.

## 4. Emote logged once

**`bot_ai.cpp` `think`**, the `GP_SERV_COMMAND_MOTIONMES` branch.

`bot_module.cpp` calls `think` once per online bot in the same tick, with the same `now`. The server queues the emote on each nearby bot's packet list. Each `think` pops its own copy. The log sits before any reply, and `PCaster` is only a real player (`caster != PChar->id` and not `isPoolChar`), so one player emote was logged once per bot that received it. Three party bots, one line three times in the same millisecond.

The first copy in a tick is logged. Later copies with the same caster id, target id, target index, caster index, emote, mode, and param are not. A different emote in the same tick still logs. The next tick starts empty. The `ShowDebugFmt` text is the same. Reply emotes are unchanged and still decided per bot.

## Assumptions

- `saysPhrase` whole-word boundaries are the right match. `move` already allowed `move camp` and `let's move`. Adding those phrases does not change that. `lets go` was already allowed.
- `switch camp`, `over there`, `come here`, `lets move`, `somewhere else`, and `better spot` are the "and similar" phrases. `somewhere else` also matches inside `go somewhere else`. `better spot` also matches inside `find a better spot`. None of them occur in `(H-6)`, `fight?`, or a bare place name.
- `leading` counts as the lead role word (`you're leading`). `tanker` and `mt` count as tank words, so `you're the leader and the mt` still sets a tank. `mt` is only special when a leader word is also present.
- `tanked` does not count as a tank word (`tank` does not match it). `you tanked that, you're the leader` drops `set_tank`.
- No function in this folder sets a party leader. Not adding one is the "otherwise no action" case. `GetLeader()` was not wired to a handoff that does not exist.
- `break_camp` still follows the current leader, not the speaker. `this way` from someone who is not the leader follows the leader.
- One module tick shares one `timer::time_point` (`now` in `bot_module.cpp`). The emote fields on each bot's copy of `MOTIONMES` are the same. A bot that skips `think` this tick and pops the packet next tick logs it again.
- More than one identical emote from the same player in a single tick (same target, indexes, emote, mode, param) logs once.
- Debug logging is on for the test server. The emote line is `ShowDebugFmt`. A successful chat reply has no map log of its own.

## Test plan

Rebuild, restart the map, party with the bots, `bots.LLM_ORDERS` on. The guard line is printed inside `plansFor`, then `brainOutcomes` prints the summary. N is how many bots are still in the party.

The summary lists every tool the model called, then the number of plans that survived. A later `no_action` can sit beside the real tool (`break_camp, no_action -> N job(s)`). Judge the guard line and the job count.

| Type in party chat | Log to expect |
| --- | --- |
| `(H-6)` | If the model calls `break_camp`: `bots: the party brain wanted camp broken on "(H-6)", which doesn't say to`, then a summary with `break_camp` and `0 job(s)`. Camp stays. A summary of `no_action` and `0 job(s)` with no guard line is also fine. |
| `fight?` | Same guard line, with `fight?`, then `0 job(s)`. A bot should answer in party chat (`?` is not optional). |
| `we're in Valkurm` | If the summary lists `break_camp`, the guard line is there and jobs are 0. If it lists only `no_action`, there is no guard line. Camp stays either way. |
| `this way` | No guard line. Summary contains `break_camp` and `N job(s)`. Bots follow the current party leader and camp clears. |
| `let's change camp` | No guard line. `break_camp` and `N job(s)`. |
| `I want experience points and you all are standing at the zone line` | No guard line. `break_camp` and `N job(s)`. |
| `over here`, `come on`, `let's move`, `lets go`, `move camp`, `go somewhere else`, `find a better spot` | No guard line. `break_camp` and `N job(s)` when the model calls `break_camp`. |
| `You're the leader Evodie` | `bots: the party brain wanted a tank set on "You're the leader Evodie", which is about the leader`, then a summary that still names `set_tank` but has `0 job(s)` if that was the only real call. Party leader unchanged. Main tank unchanged. A bot may still answer in party chat. |
| `Evodie leads` and `Evodie is leader` | Same guard line, with that text. Leader and tank unchanged. |
| `I'll tank` or `Evodie, you tank` | No leader guard line. Summary job count is 1, and a bot says who the main tank is. |
| `You're the leader Evodie, you tank` | No leader guard line. `set_tank` still applies, because `tank` is in the line. |
| `hello?` | `bots: party brain on "hello?": no_action -> 0 job(s)` (or whatever tool, then 0 jobs if a guard drops it). A bot answers in party chat. |
| `where are we fighting` | Reaches `respond`. No `?`, so the reply is optional, same as with the brain off. A bot may answer, or the model may `PASS` and say nothing. |
| `who's pulling` | Summary may say `no_action -> 0 job(s)`. If a bot is the puller, that bot still answers with what it is pulling (`pullerAnswer` / `carryOut`). If nobody is pulling, `respond` runs and the reply is optional. |
| A real player emotes once, three bots in range | One debug line, not three: `bots: emote from <name>: target <id> index <n>, caster index <n>, emote <n>, mode <n>, param <n>`. Bots may still emote back. |
