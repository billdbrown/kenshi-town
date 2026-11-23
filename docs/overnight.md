# Overnight list (2026-11-23)

Bill is asleep. Work this list in order with the autotest harness
(`scripts/autotest.py`, save `kt-test`, commands in `scripts/autotest/README.md`).
This file is the loop's memory: update the status lines as you go, and commit it.

## Rules

- Only load `kt-test`. Never save over it, never touch another save.
- One fix per commit, pushed to `origin` and `github`, so anything can be reverted.
- Bug fixes, tests and the icon prototype only. Anything that needs Bill's taste or a
  design choice goes under "For Bill" below, not into the code.
- Run `smoke.txt` after every plugin change. If it fails and the change can't be fixed
  quickly, revert the change.
- Stop if Kenshi fails to launch three times running. Make sure Kenshi is closed and no
  scenario file is left in `Kenshi/mods/TownLedger/autotest/` before stopping.
- At the end: a session note in the vault (`raw/sessions/`, wrap-session skill) with what
  passed, what was fixed, what's open, and the questions for Bill.

## The list

1. **The founder and the stone mine.** With a Building Materials store holding materials,
   the founder on the Engineer job did not build a placed Stone Mine. Reproduce it: place
   the store (complete, stocked) and a mine site, give the founder the job, run game time,
   log what he does. Needs harness commands for running game time, giving a job, adding
   items to a building and reading a character's current task. Find the cause; fix it if
   it's ours.
   - status: **done, not reproduced with a store** (2026-11-23). Scenarios `engineer-mine`,
     `engineer-second`, `engineer-chest` and a no-job control:
     - Engineer job (JOB_BUILDER, 125) + a complete, stocked Building Materials store: he
       builds the mine, and a second mine placed later without being told again.
     - No job: the mine stays at 0%.
     - Materials only in a General Storage Chest: he never builds (wanders, "Aimless").
     So Bill's first report (chest) is vanilla behaviour. His second (a stocked store, still
     no building) doesn't reproduce here; see For Bill.
     - `getPermajobCount` reads 0 for jobs given with `addJob` even while they work.
2. **Arrivals.** Force an arrival, accept one and turn one away; check names, tags,
   stats within the intended range, squad placement, the toast and the HUD count.
   - status: **done** (2026-11-23), scenario `arrivals` passes 13/13. Two bugs fixed:
     - arrivals appeared in the middle of town (`getPositionOutsideTownGates` returns the
       Hub's centre); they now spawn 15 m outside one of the Hub's gates;
     - Turn away only dropped them from the list and they stood there for good; they now
       leave (removed).
     Confirmed: one background stands out (top stat 14-24, two or three stats above 8),
     accept gives "[Citizen] Name" in the Citizens squad, the toast, the tray alert and
     "Town (3 at gate)" all show.
3. **A full guided-start run.** Tasks 1 to 10 driven end to end by a scenario, each
   completing only when its condition is really met, with a screenshot of the tracker at
   each step. Keep it as the regression test.
   - status: not started
4. **Overhead icons (prototype).** Bill wants icons over things in the world, for a
   city-builder feel, with the long-term aim of menus that hang off places:
   - over the bar; over the nomad animal trader and other traders;
   - over the headquarters, once there is one (for now: the repaired Longhouse the Town
     Hall task picks). Clicking the HQ icon opens the Kenshi Town window;
   - over the leader of a squad of bandits or other hostiles, so they can be seen coming.
   He is comfortable smoothing Kenshi's "mouse over to learn who they are" for this mode.
   Tonight: prove the mechanism (world position to screen position each frame, a MyGUI
   image there, hidden when off screen or behind the camera, clickable) with placeholder
   icons, and screenshots for Bill. Find out first whether vanilla already draws such icons
   and can be reused. Icon art, sizes, which groups get one and at what distance are Bill's
   to choose: offer options, don't settle them.
   - status: not started

If there is time left: the garrison contract (jump the clock past grace; fee, warnings,
guards leaving), the selection lock, the ankh (construct it, use it, screenshot the bow).

## For Bill

(questions and choices that came up overnight)

- **The founder and the mine.** With a finished, stocked Building Materials store and the
  Engineer job he builds it every time here. When it failed for you: was the store itself
  finished (not still a construction site)? Was the mine a site, or built but not being mined
  (mining is a separate job)? Was he given Engineer by shift-clicking, and on what?
- **Arrivals are forgotten on a reload.** Who is waiting at the gate is only in memory, so
  after loading a save they're still standing there as plain Drifters, not in the Roster.
  Options: remember them by name tag (as citizens are), or send them away on load.
- Turned-away drifters now vanish on the spot. Walking off would look better; it needs an
  order to leave and a later clean-up. Worth it?
- Idea, not built: an alert "builders can't find building materials" when a site needs
  materials and no Building Materials store holds any (a general chest doesn't count).
