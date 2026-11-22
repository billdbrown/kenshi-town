# kenshi-town

A town-management game mode for Kenshi, built as a native plugin on
[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) and KenshiLib.

The goal: run a settlement instead of a squad. People arrive and leave, work is assigned by
role rather than per-character job queues, and a management screen replaces most of the
micromanagement.

## Status

**Milestone 1: Town Ledger.** Press **F8** in game to open a panel listing everyone in your
faction and what they are doing. It is read-only. It exists to prove the whole loop: build,
load, hook the main loop, draw a MyGUI window, read live game state.

## Requirements

- Kenshi on Steam, 1.0.68. GOG is not supported by RE_Kenshi.
- RE_Kenshi 0.3.4, which ships KenshiLib 0.4.0.

## Building

KenshiLib plugins must be compiled with the **VC++ 2010 (v100) x64** compiler. You do not need
full Visual Studio 2010:

1. Any Visual Studio 2019 or newer, or the 2022 Build Tools, for MSBuild.
2. The Windows SDK 7.1 ISO and the VC++ 2010 SP1 compiler update (KB2519277). Both are
   still on Microsoft's servers. `scripts/install-v100-toolchain.ps1` installs the
   compiler MSIs straight from the ISO, which avoids the SDK installer's well-known failure
   when a newer VC++ 2010 redistributable is present. The URLs are in the script.
3. Dependencies:

       git clone https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps third_party/KenshiLib_deps
       tar -xf third_party/KenshiLib_deps/boost_1_60_0/boost.zip --directory third_party/KenshiLib_deps/boost_1_60_0

Then:

    scripts\build.cmd            build to build\TownLedger.dll
    scripts\build.cmd install    build and copy mod\TownLedger into Kenshi\mods

Set `KENSHI_DIR` if Kenshi is not in the default Steam location. Enable **TownLedger** in the
launcher's Mods tab.

## Notes

- `third_party/vc10_compat/ammintrin.h` is an empty stub. The SDK 7.1 compiler's `intrin.h`
  includes a header that the SDK does not ship.
- KenshiLib 0.4.0's `AI.h` and `Character.h` disagree about `CharacterMessage`. See the
  macro around the include in `src/TownLedger.cpp`.
- The build approach (SDK 7.1 + `UseEnv=true`) follows
  [KenshiCoop](https://github.com/nhoral/KenshiCoop).

## License

GPL-3.0, as required by KenshiLib and RE_Kenshi.
