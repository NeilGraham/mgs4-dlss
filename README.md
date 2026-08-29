# mgs4-dlss

Work toward real DLSS (DLAA first) in the PC port of *Metal Gear Solid 4* (Master Collection Vol. 2), so the game can be hooked by NGX-based add-ons such as `renodx-dlss5`.

## Status

| Step | State |
|---|---|
| Route A — run the port on bgfx's built-in Direct3D 12 backend | **Done** — `d3d12-switch/` |
| Phase 0 — map the frame (scene target, depth, composite draw) | **Done** — see docs |
| Phase 1a — NGX DLSS (DLAA) created + evaluated every frame, NGX add-ons can hook it | **Done** — `dlss-addon/` (v1: zero jitter / zero motion vectors) |
| Phase 1b — camera jitter + camera-only motion vectors | next |
| Phase 2 — per-object motion vectors | planned |
| Phase 3 — real upscaling (internal res < output res) | maybe |

See [docs/renderer-notes.md](docs/renderer-notes.md) for what we know about the port and the full plan.

## dlss-addon (`mgs4_dlss.addon64`)

A ReShade add-on (API 20, D3D12 only) that creates a real NGX DLSS Super Resolution feature (DLAA, preset K) and evaluates it every frame:

1. Tracks what bgfx binds per command list (RT/DS, viewport, root descriptor tables, root CBVs, root signature, PSO).
2. Mirrors bgfx's `CopyDescriptors` traffic (`copy_descriptor_tables` event) into its own slot→resource map, because ReShade does not register copied CBV/SRV/UAV descriptors in its view map. That makes the SRVs of any draw resolvable.
3. At the one draw per frame into the backbuffer-sized target (the 1:1 composite of the finished 3840x2160 frame, UI included), resolves the texture that draw samples — a double-buffered final texture, *not* the render target with the most draws — pairs it with the depth buffer that was bound with it, and calls `NGX_D3D12_EVALUATE_DLSS_EXT` with color/depth/(zero) motion vectors.
4. Copies the DLSS output over the sampled texture, then natively re-applies bgfx's descriptor heaps, root signature, PSO and root parameters so the composite draw is unaffected.
5. Re-creates the feature once after 120 evaluations: NGX-hooking add-ons install their hooks when `nvngx_dlss.dll` loads (inside our first `CreateFeature`), so they would otherwise never see the create call that carries the "DLSS contract".

Because the NGX calls go through the standard `_nvngx.dll` exports, NGX-hooking add-ons see them: `renodx-dlss5` reports `DLSSNR ACTIVE`, creates its NR feature after ours and evaluates it every frame.

Verified 2026-08-28: NGX init OK on RTX 5090 / 616.56, `CreateFeature` OK, ~120 evaluations/s (≈80 with DLSS 5 NR active), HUD/UI intact, and `DebugMode=1` paints the displayed image magenta (proves the insertion path).

### Build / install

Requirements: MSVC Build Tools (the `build.bat` calls `vcvars64.bat` from VS 18 BuildTools — adjust the path if yours differs), ReShade 6.8 installed as `MGS4\dxgi.dll`, the D3D12 switch above, and `nvngx_dlss.dll` in `MGS4\` (copy `third_party/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll` or let the NVIDIA app override supply it).

```bat
dlss-addon\build.bat                       :: -> build\mgs4_dlss.addon64
copy build\mgs4_dlss.addon64 "<game>\MGS4\"
```

Optional `MGS4\mgs4_dlss.ini`:

```ini
[DLSS]
Enabled=1        ; live-reloaded every ~second
Preset=11        ; NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer). 10 = J
Sharpness=0      ; 0..100 (live)
LogEveryN=600
RecreateAfter=120
DebugMode=0      ; live: 1 = paint the displayed texture magenta (path test), 2 = bypass DLSS (A/B), 3 = trace 3 frames
```

Log: `MGS4\logs\mgs4_dlss.log`.

### Known limitations (v1)

- No sub-pixel jitter and zero motion vectors: DLSS behaves as a temporal filter with no sub-pixel information, so static shots converge cleanly but camera/object motion ghosts. Phase 1b adds Halton jitter via the projection upload and camera-only motion vectors reconstructed from depth.
- DLAA only (render = output resolution). Real upscaling needs Phase 3.

## d3d12-switch (`MGS4_D3D12.asi`)

The port renders through [bgfx](https://github.com/bkaradzic/bgfx) and picks Direct3D 11. bgfx tries backends in score order (D3D11, then D3D12, …) and moves on when one fails to create a device. The ASI hooks the system `d3d11.dll`'s `D3D11CreateDevice` / `D3D11CreateDeviceAndSwapChain` and returns `E_FAIL` for calls that carry `D3D11_CREATE_DEVICE_SINGLETHREADED` (0x1 — bgfx passes 0x21), so bgfx falls through to its D3D12 backend. Nothing else is patched.

Verified 2026-08-28: game boots and renders the title/intro on D3D12 (`ReShade.log` shows `D3D12CreateDevice`, `D3D12Core.dll` loaded); MGSFPSUnlock, ReShade and `renodx-dlss5.addon64` load alongside.

### Build / install

Requirements: mingw-w64 `gcc` on PATH (Git Bash), [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `MGS4/d3d11.dll`.

```sh
sh d3d12-switch/build.sh      # -> build/MGS4_D3D12.asi
sh d3d12-switch/install.sh    # copies asi + ini to MGS4/scripts/ (override with MGS4_DIR=...)
```

Revert to stock D3D11: set `Enabled = 0` in `MGS4/scripts/MGS4_D3D12.ini`. Log: `MGS4/logs/MGS4_D3D12.log`.

## Layout

```
d3d12-switch/          mgs4_d3d12.c, MGS4_D3D12.ini, build.sh, install.sh
dlss-addon/            src/mgs4_dlss.cpp, build.bat, mgs4_dlss.ini (sample)
third_party/minhook/   MinHook (BSD-2), vendored
third_party/reshade/   ReShade add-on API headers (v6.8.0, BSD-3)
third_party/DLSS/      NVIDIA DLSS SDK headers + nvsdk_ngx_s.lib (DLLs git-ignored)
docs/                  reverse-engineering notes and the DLSS plan
```
