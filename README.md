# lr_core (TAB only)

MetaMod CS2 plugin that applies **Levels Ranks icons in the scoreboard (TAB)**.

XP, MySQL, menus, wallet, and chat commands live in the CounterStrikeSharp plugin **LevelsRanks** (`D:\projects\csharp\LevelsRanks`). C# pushes level/exp here via console:

- `lr_tab_set <slot|steamid64> <level> [exp]`
- `lr_tab_refresh <slot|steamid64>`
- `lr_tab_test <steamid64> <badge>`
- `lr_customround <0|1>` — VIP custom-round gate for C#
- `lr_status` / `lr_reload`

## Config

Only `configs/tab.ini` is required (workshop skillgroup addon id, competitive values, delays).

## Build (Linux x86_64)

```bash
bash build_cmake.sh
# or AMBuild:
bash build_linux.sh
```

Output: `build/addons/lr_core/` (+ `addons/metamod/lr_core.vdf`).

No MariaDB. Build still generates minimal HL2SDK protobuf stubs (`network_connection` etc.) required by SDK headers; TAB reveal uses `FindNetworkMessagePartial` at runtime.

## Install

1. Copy packaged `addons/lr_core/` and `addons/metamod/lr_core.vdf` into `game/csgo/`.
2. Set `workshop_id` in `tab.ini` and ensure MultiAddonManager mounts that addon (`mm_extra_addons`).
3. Run LevelsRanks (C#) on the same server so it calls `lr_tab_set` after XP changes.
