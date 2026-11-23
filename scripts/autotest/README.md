# Autotest scenarios

    python scripts/autotest.py scripts/autotest/<scenario>.txt [--no-install] [--timeout N]

The runner installs the build, writes the scenario to
`Kenshi/mods/TownLedger/autotest/scenario.txt`, launches Kenshi, waits for the
plugin's report, ends Kenshi and removes the scenario (normal play never sees
it). Results, screenshots included, land in `build/autotest/<scenario>-<time>/`.
It takes over the screen while it runs, and refuses to start if Kenshi is open.

How a run gets into the game (each learned the hard way, 2026-11-22): Kenshi started
directly hands over to Steam and exits, so the runner follows `kenshi_x64.exe` by name;
it presses OK on Kenshi's settings launcher with a window message; and the `load` step
runs from MyGUI's `eventFrameStart`, because the game loop hook doesn't tick at the main
menu and Kenshi never fires Ogre frame listeners.

`construct` adds progress directly, without materials; the plugin's finisher then
completes anything of ours at 100%. So it proves "task waits for completion", not
"builders fetch materials".

The runner backs up and restores the plugin's state files (`contracts.txt` and co.), so a
scenario that jumps tasks doesn't change the save's record for the next run.

During a run the plugin holds Kenshi's autosave timer at zero (an early long run overwrote
the player's autosave slots), and sets Kenshi's "current game" name to the loaded save
(`SaveManager::load(name)` leaves it at the last autosave, and the plugin keys its per-game
record on it).

The test save is **`kt-test`**: a "Kenshi Town: The Hub" new game, saved right
after the Hub was claimed. Nothing is saved during a run, so it stays as it is.

## Commands (one per line, `#` comments)

| command | does |
|---|---|
| `load <save>` | from the main menu, loads a save |
| `wait_ready [secs]` | waits until in game, Hub found and ours, settled 5 s (default 240 s) |
| `wait <secs>` | real seconds |
| `speed <x>` | game speed |
| `camera <x> <z>` | moves the camera |
| `camera_on <name>` | Kenshi's own framing (`focusCameraOnObject`) of the nearest Hub building whose name contains `<name>`; it becomes `current` |
| `view <yaw> <pitch> <zoom>` | experimental: `manuallySetOrientationAndZoom`; positive pitch looks down, but zoom isn't a distance (left the camera at ground level) |
| `pick <name>` | makes it `current` without moving the camera |
| `screenshot <name>` | `<name>.png` of the game window |
| `dump` | the Hub's building list (`hub_buildings.txt`) |
| `repair <name>` | our Repair on the nearest ruin by that name; it becomes `current` |
| `place <stringId> <dx> <dz>` | a construction site near the camera; it becomes `current` |
| `place_near <stringId> <dx> <dz> [done]` | a site (or a finished building) beside the founder; becomes `current` |
| `stock <count> <item name>` | items into the current building |
| `job <task number> [name\|current]` | a standing job for the founder (125 = Engineer, 87 = operate machinery) |
| `arrive` | a group of drifters at the gate now |
| `gate` | who is waiting: background, top skills, stats, distance from the Hub centre |
| `accept <i>` / `turn_away <i>` | what the Roster's buttons do, by gate index |
| `people` | every player character: name, tag, squad, citizen or retinue, activity |
| `expect gate\|people\|citizens <n>` | counts |
| `expect gate_modest` / `expect gate_outside` | arrivals' stats in range; last listed arrival outside the walls |
| `safe on\|off` | remove anyone hostile to the founder within 300 m as they come (raids kill a test) |
| `nearby [metres]` | who is around the founder, by faction, hostility and activity |
| `give <count> <item name>` | items into the founder's pack (what fits) |
| `wait_task <n> [secs]` | wait for the guided start to reach task n; fails on timeout or overshoot |
| `assign <person> <profile>` | the Roster's Assign |
| `set_stat <stat number> <value>` | the founder's stored stat (2 = Melee Attack) |
| `beds` | every bed in the Hub and what it's inside |
| `icons [on\|off]` | overhead icons: found/drawn per kind, with clip-space and camera debug |
| `click_icon` | what clicking the town hall's icon does (opens the Kenshi Town window) |
| `who` | what the founder is doing, his jobs, distance to `current` |
| `construct <name\|current> [secs]` | does builders' work on it until complete (default 60 s) |
| `set_task <n>` | jumps the guided start to task n |
| `expect task <n>` | the guided start is on task n |
| `expect complete\|incomplete <name\|current>` | Kenshi's own complete flag |
| `expect exists\|missing <name>` | a Hub building by that name |
| `expect none_stuck` | nothing of ours at 100% construction but not complete |
| `scene` | every Ogre movable-object type: count, count near the Hub, samples (building parts are not Entities; only doors are) |
| `center <name\|current>` | camera centre on its position (lands somewhere odd: world and camera coordinates differ) |
| `camera_hold` | stop following what `camera_on` focused; `camera_on X` + `wait 1.5` + `camera_hold` frames X best, though not identically run to run |
| `orbit <yaw> <pitch> <dist>` | experimental: turns the camera's centre node, pulls its node back |
| `unruin <name\|current>` | the instant repair: un-ruin at 100% |
| `remove <name\|current>` | `dynamicDestroyBuilding`, as the clean-up does |
| `furniture <name\|current>` | what's furnished inside it, every function |
| `remove_all <name\|current>` | its furniture, then it |
| `cleanup` | the first-claim Hub clean-up, run now (uses the installed `hub_cleanup.txt`) |
| `clear_camps` | the squatter-camp clean-up the plugin runs after a load (debris batch + camp furniture in ruins) |
| `note <text>` | a line in the report |
| `quit` | writes DONE and ends Kenshi (also at the end of the file) |

Names match case-insensitively, nearest the camera first, and include the
research benches and beds inside buildings.

## Report

`report.txt`, one line each: `STEP`, `INFO`, `PASS`, `FAIL`, `SHOT`, `NOTE`,
ending `DONE pass=N fail=M`. The runner exits 0 only with a DONE and no FAIL.
