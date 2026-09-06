# Third-party notices

The MIT licence in `LICENSE` covers this project's own code: `dlss-addon/src`, `launcher/src`, `tools/` and the
documentation. The pieces below are other people's, vendored under `third_party/`, and each stays under its own
licence, reproduced in full in the file named.

| component | what is vendored | licence |
| --- | --- | --- |
| [ReShade](https://reshade.me/) add-on API | headers | BSD-3-Clause - `third_party/reshade/LICENSE.md` |
| [MinHook](https://github.com/TsudaKageyu/minhook) | source, including HDE32/HDE64 by Vyacheslav Patkov | BSD-2-Clause - `third_party/minhook/LICENSE.txt` |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | headers and the `nvsdk_ngx_s.lib` static library | NVIDIA RTX SDKs licence - `third_party/DLSS/LICENSE.txt` |
| [NVIDIA Streamline](https://github.com/NVIDIAGameWorks/Streamline) | headers | MIT - `third_party/streamline/license.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) | `imgui.h`, `imconfig.h` | MIT - `third_party/imgui/LICENSE.txt` |

The NVIDIA DLSS and Streamline runtimes (`nvngx_dlss.dll`, `nvngx_dlssg.dll`, `sl.*.dll` and the rest), ReShade
itself and RenoDX's `renodx-dlss5` add-on are not in this repository or its releases; the launcher's Setup tab guides
the download of each from its own source, and each is used under its own terms.

The scene thumbnails in `tools/thumbs` and the header banner in `tools/art` are screenshots of *Metal Gear Solid 4*
taken while playing. Metal Gear Solid is a trademark of Konami Digital Entertainment, and the game's artwork belongs
to Konami; the launcher reads the game's key art, icon and music from the player's own installation and copies none
of them.
