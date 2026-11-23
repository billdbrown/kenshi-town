# Autotest scenarios

    python scripts/autotest.py scripts/autotest/<scenario>.txt [--no-install] [--timeout N]

The runner installs the build, writes the scenario to
`Kenshi/mods/TownLedger/autotest/scenario.txt`, launches Kenshi, waits for the
plugin's report, ends Kenshi and removes the scenario (normal play never sees
it). Results, screenshots included, land in `build/autotest/<scenario>-<time>/`.
It takes over the screen while it runs, and refuses to start if Kenshi is open.

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
| `camera_on <name>` | camera to the nearest Hub building whose name contains `<name>`; it becomes `current` |
| `pick <name>` | makes it `current` without moving the camera |
| `screenshot <name>` | `<name>.png` of the game window |
| `dump` | the Hub's building list (`hub_buildings.txt`) |
| `repair <name>` | our Repair on the nearest ruin by that name; it becomes `current` |
| `place <stringId> <dx> <dz>` | a construction site near the camera; it becomes `current` |
| `construct <name\|current> [secs]` | does builders' work on it until complete (default 60 s) |
| `set_task <n>` | jumps the guided start to task n |
| `expect task <n>` | the guided start is on task n |
| `expect complete\|incomplete <name\|current>` | Kenshi's own complete flag |
| `expect exists\|missing <name>` | a Hub building by that name |
| `expect none_stuck` | nothing of ours at 100% construction but not complete |
| `note <text>` | a line in the report |
| `quit` | writes DONE and ends Kenshi (also at the end of the file) |

Names match case-insensitively, nearest the camera first, and include the
research benches and beds inside buildings.

## Report

`report.txt`, one line each: `STEP`, `INFO`, `PASS`, `FAIL`, `SHOT`, `NOTE`,
ending `DONE pass=N fail=M`. The runner exits 0 only with a DONE and no FAIL.
