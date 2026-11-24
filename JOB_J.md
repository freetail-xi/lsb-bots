# Job J: after a wipe, come back as the same party (DESIGN-review.md proposal 12)

This folder is lsb-bots, a C++ bot module for LandSandBoat (FFXI server, 75 era). Read `DESIGN-review.md`
(sections 1 and 3, proposal 12) and the code first. Jobs D to I and several fixes are already committed (see
`git log`), so build on what is there now: the diary (`noteEvent`), sessions, role-based parties.

Implement proposal 12:
- For a bot-only party (no real player in it), remember the leader's camp (`campSpot` / the `Pull` camp, zone and
  position) before members homepoint after a wipe. A party that has a real player keeps today's behaviour (waits
  for a raise with `kRaiseWait`, homepoints only when nobody can raise).
- After they are alive again, the leader travels back to the remembered camp zone and the members `follow`
  (which already zones to the leader). Exempt such a party from the idle cut in `think` (bots in zones with no
  real player go idle) until they are back in the camp zone, capped at 10 minutes, so a stuck path can't keep
  zones active forever. After the cap, or once back, clear the memory and behave as today.
- Don't use a camp pointer after the `Pull` state is gone: store a copy (zone id + position), not a pointer.
- Sessions must not log out a party member while it is regrouping (treat it as not free).
- Diary: one line for each member, "wiped at <zone>, heading back" (prettyName of the camp zone).

Rules: existing code style, no reformatting, no unrelated changes. Only use functions, types and members that
already appear in this code, and list every assumption in the notes. Write `JOB_J_NOTES.md`: what changed (file
and function) and a test plan for the test server. Edit only files in this folder.
