# CALL to EXIST FPS Unlocker

An FPS unlocker for **CALL to EXIST** on Steam. The game's internal scheduler is locked to 60 FPS; this tool lifts that limit and lets you choose your own frame rate, with a small in-game panel to change settings live.

The game's executable is never modified. The tool is a drop-in `winmm.dll` proxy that the game loads on startup.

## Features

- Unlocks the game's 60 FPS cap. The default target is **144 FPS**, and you can set any value from 1 to 1000, or 0 for uncapped.
- Optional **VSync removal**. This only changes how the game presents frames; your monitor's refresh rate and driver settings are not touched.
- **In-game settings panel** (press **F10** to show or hide) with presets (60 / 144 / 240 / 360), a custom FPS box, an Unlimited toggle, a VSync toggle, an FPS counter and logging toggle.
- **Save settings** writes your choices to the INI so they apply on the next launch. **Reload saved** re-reads the INI without restarting.
- Optional on-screen FPS counter and a diagnostic log (`CTE_FPSPatch.log`) that records measured FPS every five seconds.
- The game's simulation clock stays at its nominal 60, so game speed is unchanged. In testing, the measured rate was about 143-144 FPS at the 144 FPS target.

## Installation

1. Place this folder directly inside your CALL to EXIST game folder, next to `CALL to EXIST.exe`.
2. Build `winmm.dll` (see [Building](#building)).
3. Close the game, then run `Install.ps1` from PowerShell in this folder. It copies `winmm.dll` and `CTE_FPSPatch.ini` into the game folder.
4. **Launch the game through Steam.**

You can also install manually by copying `build/winmm.dll` and `CTE_FPSPatch.ini` into the game folder.

`Install.ps1` refuses to run while the game is open and refuses to overwrite a different `winmm.dll` (for example, one from another mod). It verifies its own installed DLL by hash before replacing it, keeps old copies in `backups/`, and never overwrites an existing INI.

### Uninstalling

Close the game and run this from PowerShell in this folder:

```powershell
.\Install.ps1 -Uninstall
```

This moves the hash-verified `winmm.dll` out of the game folder. Your settings and logs are kept. You can also just remove that `winmm.dll` by hand while the game is closed.

## Configuration

Press **F10** in game for the live panel, or edit `CTE_FPSPatch.ini` in the game folder:

```ini
[FPS]
Enabled=1
MaxFPS=144
DisableVSync=1
LogMeasurements=1

[Overlay]
ShowFPS=1
OpenAtLaunch=1
```

| Setting | Meaning |
| --- | --- |
| `Enabled` | `1` unlocks the FPS cap. `0` returns to a 60 FPS limit and restores the game's own VSync behavior. The panel stays available so you can turn the unlock back on. |
| `MaxFPS` | Target frame rate, 1 to 1000, or `0` for uncapped. Actual FPS still depends on your hardware and the scene. |
| `DisableVSync` | `1` removes the VSync wait. `0` keeps VSync on. |
| `LogMeasurements` | Writes measured FPS to `CTE_FPSPatch.log` every five seconds. |
| `ShowFPS` | Shows the small FPS counter in the corner. |
| `OpenAtLaunch` | Whether the panel is visible at startup. F10 always toggles it. |

The panel does not pause the game, so it is best used in a menu or while paused.

## How it works

- **Loading:** Windows loads the proxy `winmm.dll` in place of the system one. It forwards all 181 of the real WINMM exports (names and ordinals) to the genuine library in System32, so audio and timer calls behave normally. No Windows DLL is replaced or redistributed.
- **Safety checks:** on the first WINMM call it verifies the game executable's identity, x64 architecture, build timestamp, image size and five separate code signatures. If anything doesn't match, the game is left unpatched and WINMM calls simply pass through.
- **Unlocking the cap:** it replaces only the six-byte scheduler wait branch, in memory. It then paces the scheduler using your target FPS, a high-resolution waitable timer and a short final spin. The counter still returns real operating-system time, and other timing callers are unaffected.
- **VSync and measurement:** the game's swapchain `Present` call is hooked to remove the VSync wait when requested and to measure the real presentation rate. The original function stays in the call chain.
- **Overlay:** the panel is drawn with Dear ImGui through the game's own Direct3D 11 device. Render state is restored afterwards and backbuffer references are released before each `Present` so window resizing keeps working.

## Compatibility and limits

- Validated against one game build: executable build timestamp `5DF36448`, SHA256 `57c866f551b86622e107ceeac7346519a2ffec252804bedbc292bfa5921ab4ea`. Both measured image layouts (sizes `0xC7F000` and `0xCAC000`) are recognized only if every code signature matches.
- A game update may change the code and require the patch to be updated.
- Tested through Steam: the panel, FPS presets, custom values, uncapped mode, VSync on/off, live enable/disable, saving and reloading settings. The simulation clock stayed at its nominal rate.
- Not tested: every combat mechanic, cutscene and multiplayer mode, so frame-rate-dependent behavior there is unverified. Fullscreen and device-loss stress cases were also not tested.
- Other mods that replace the same scheduler code or the same Direct3D entry points can conflict with this one.
- This is an unofficial community tool, not affiliated with the game's developers. Use it at your own risk.

## Building

Run `build.cmd` with Visual Studio's **Desktop development with C++** tools installed. It finds the x64 MSVC and MASM environment, compiles the sources with warnings treated as errors, and produces `build/winmm.dll`.

| Path | Contents |
| --- | --- |
| `src/` | Patch and overlay sources, export definitions and assembly forwarding stubs |
| `third_party/imgui/` | Vendored Dear ImGui (with its Win32 and D3D11 backends) |
| `build.cmd` | Build script |
| `Install.ps1` | Install and uninstall script |
| `CTE_FPSPatch.ini` | Default settings |

No third-party hooking library is used.

## Credits

The in-game panel is built with [**Dear ImGui**](https://github.com/ocornut/imgui) by Omar Cornut and contributors (version 1.91.9b), used under the MIT License. It is vendored in `third_party/imgui/` along with its license, and `SOURCE.json` records the download URLs and file hashes.
