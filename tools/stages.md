# Stage ids for `mgs4.exe --stage <id>`

Extracted from the stage table in `mgs4.exe` (75 base ids). A base id boots the stage; `<id>_D<n>` entries are its
cutscenes ("demos") and `<id>_<n>` its gameplay sections — they can be passed to `--stage` as well. Names are not
stored in the executable; the descriptions below are from the game's structure and from what the test runs showed
(marked "seen"); unmarked ones are educated guesses — update them as you verify.

| Act | Ids | Notes |
|---|---|---|
| Prologue | `s00a00l` (seen: cemetery, Snake & Otacon at the grave — in-game cutscene), `s00a10l` (`_D`) | |
| 1 Middle East | `s01a00l` (`_D`; seen: Act 1 opening street, HUD gameplay after the intro), `s01a05l_D`, `s01a10l` (`_D1`, `_1`, `_D2`), `s01a14a`, `s01a20l` (`_D`), `s01a30l` (`_1`, `_D2`, `_2`), `s01a40l` (`_1`, `_D2`, `_2`, `_D3`), `s01a50l`, `s01a55l_D`, `s01a57l`, `s01a60l` (`_D1`, `_D2`) | |
| 2 South America | `s02a10l` (`_D1`, `_1`, `_D2`, `_2`, `_3`), `s02a20l` (`_1`..`_11`), `s02a25l` (`_D1`..`_D3`), `s02a30l` (`_1`..`_12`), `s02a40l` (`_1`..`_3`), `s02a50l` (`_D1`..`_D8`, `_4`), `s02a60l` (`_1`..`_3`), `s02a70l_D`, `s02a73l`, `s02a75l`, `s02a78l`, `s02a80l`, `s02a85l_D`, `s02a90l`, `s02a95l_D` | **`s02a50l_D1` = Research Lab (Naomi) cutscene: rose garden, Gekko (seen)**; `s02a50l` starts on the same shot; `s02a40l` = HUD gameplay, jungle path with PMC (seen); `s02a60l` = cutscene, Snake along the lab building — ran with dynamic resolution active (seen); `s02a70l`..`s02a95l` the Laughing Octopus / escape chapters |
| 3 Eastern Europe | `s03a00l` (`_D1`), `s03a10l` (`_1`..`_6`), `s03a15l` (`_1`..`_3`), `s03a16l` (`_1`, `_2`), `s03a20l` (`_1`..`_3`), `s03a25l` (`_1`, `_2`), `s03a30l` (`_D1`..`_D8`), `s03a35l`, `s03a40l` (bike chase), `s03a50l_D`, `s03a60l`, `s03a65l`, `s03a70l` (`_D1`, `_1`, `_D2`, `_2`, `_D4`, `_D5`), `s03a80l`, `s03a90l` (`_D1`..`_D5`) | |
| 4 Shadow Moses | `s04a05l`, `s04a10l` (`_D1`, `_D2`, `_1`..`_12`), `s04a20l` (`_1`..`_7`, `_D`), `s04a30l` (`_1`, `_D1`..`_D7`), `s04a40l`, `s04a50l` (`_D`), `s04a60l` (`_D1`..`_D3`, `_1`, `_2`), `s04a65l`, `s04a68l` (`_D`), `s04a70l`, `s04a75l` (`_D1`..`_D3`) | |
| 5 Outer Haven | `s05a10l` (`_D`), `s05a20l` (`_1`..`_12`, `_D1`..`_D12`), `s05a21a`, `s05a30l`, `s05a40l_D`, `s05a45l` (`_D`), `s05a50l` (`_D`), `s05a55l_D` | |
| other | `s10a20l_D1/_D2`, `s10a30l_D1/_D2`, `s10a40l_D/_D2`, `s20a00l_D1..D3`, `s30a00l_D..D4` | interludes / mission briefings |

Rotation runs: `tools\test_stages.ps1 -Stages "s00a00l,s02a50l_D1,s03a30l_D1,s04a10l_D1" -MvVis`.
