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
   - status: **done** (2026-11-23). `guided-start.txt` passes 21/21: the founder repairs the
     Longhouse and builds the bench, store, mine, processor and beds himself on the Engineer
     job; a drifter is accepted and made a Miner (and goes to work the mine); stockpile and
     guard complete. Each task was checked not to complete early. Not covered: the research
     itself (sites are placed directly). Found and fixed on the way:
     - **Sites need more than building materials**: the store 1 Iron Plates, the processor 6,
       a camp bed a Sleeping Bag. The tracker said "Building 0%"; it now says "Needs 6 Iron
       Plates". This, not the Engineer job, is why things didn't get built.
     - **The Bar's three inn beds counted as the town's**, so "A bed for everyone" completed
       with no bed built. Town beds now exclude the Bar's.
     - Long toasts were cut off at both ends; they may now run wider than the column.
     - Harness: safe mode (removes raiders), no autosaves during a run, the "current game"
       name set after a load.
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

- **I overwrote your three autosave slots. Sorry.** Two long test runs (02:16 to 02:36) let
  Kenshi autosave as usual, into `autosave0`, `autosave1` and `autosave2`; they now hold test
  states of `kt-test`. Named saves are untouched. The harness now holds Kenshi's autosave
  timer at zero during a run, so it can't happen again.
- **Dust Bandits raid the Hub on day one and kill the founder.** A squad of nine, "ransacking
  town", arrives about 90 s into a run at fast speed (both runs watched). It's Kenshi's own
  raid system for player towns (Dust bandits: `1st`, `raid 1`, `tax collector`, `raid
  filler`). The founder has 1 in every combat stat. No Holy Nation Outlaws were within 80 m
  of him; the bar's Trade Ninjas fought the raiders after he was down. Choices: turn Dust
  Bandit raids off (as with the Hungry Bandits), delay them until the town has a guard, or
  make the hired garrison actually defend. Tests run with a "safe mode" that removes
  hostiles; the game is unchanged.
- **Possible real bug, unconfirmed:** `SaveManager::load(name)` leaves Kenshi's "current game"
  name at the last autosave, and the plugin keys each game's record (garrison clock, cleaned,
  guided-start task) on that name. The harness hit this. If the Load menu behaves the same,
  loading a named save after an autosave reads the autosave's record. Worth checking in the
  log next time you load a named save: the plugin could log which record it picked.

- **The founder and the mine.** With a finished, stocked Building Materials store and the
  Engineer job he builds it every time here. When it failed for you: was the store itself
  finished (not still a construction site)? Was the mine a site, or built but not being mined
  (mining is a separate job)? Was he given Engineer by shift-clicking, and on what?
- **Arrivals are forgotten on a reload.** Who is waiting at the gate is only in memory, so
  after loading a save they're still standing there as plain Drifters, not in the Roster.
  Options: remember them by name tag (as citizens are), or send them away on load.
- Turned-away drifters now vanish on the spot. Walking off would look better; it needs an
  order to leave and a later clean-up. Worth it?
- **Where do Iron Plates and Sleeping Bags come from in the guided start?** Tasks 3, 5 and 8
  need 1 plate, 6 plates and a Sleeping Bag per bed. A new founder has 1,000 cats and none of
  them. Options: say "buy them at the bar" in the hints; give them as task rewards (a plate
  with the bench task, six with the mine task); a starting chest; or cheaper recipes.
- **With nothing he can build, the Engineer walks to the Hub's ruined gates and stands
  there.** That's the "runs to the gates" you saw. Un-ruining the gates at claim stops it
  (tested), but gives the town working gates, which changes who can walk in. Your call.
- **One materials store holds about 100**, and task 9 asks for 100 stockpiled: the store must
  be brim-full, or there must be two. Lower the target, or leave it?
- **"Storage: Building Materials is full" shows as an alert** all through the guided start.
  For a stockpile that's good news, not a problem. Drop it for resource stores?
- A naked founder's pack takes 3 Building Materials, 5 Iron Plates or 1 Sleeping Bag at a time.
- Idea, not built: an alert "builders can't find building materials" when a site needs
  materials and no Building Materials store holds any (a general chest doesn't count).
