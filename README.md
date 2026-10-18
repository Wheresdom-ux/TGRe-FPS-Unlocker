# CALL to EXIST FPS proxy 1.3

The installed `winmm.dll` proxy unlocks the game's internal 60 FPS scheduler. **Launch the game through Steam.** No injector, Cheat Engine table or model viewer is required.

The default target is **144 FPS**. The tested Steam-launched session measured approximately 143–144 FPS, while retaining the game's nominal clock of 60 and a normalization of 60 simulation units per real second. The ModelRipper proxy has been removed; the overlay is built into this WINMM proxy.

Version 1.2 fixes dropped input by queuing window messages independently of rendering and polling mouse/key state as a fallback. Keyboard modifier states are captured with each event so quick shortcuts remain ordered. Mouse coordinates are mapped to the actual backbuffer after window resizing. A GPU-rendered red circle cursor with a white outline appears while hovering or dragging the panel, and disappears when the panel is closed or the mouse leaves it.

Version 1.3 refreshes the interface with Segoe UI, larger performance readings, selected-preset highlighting, grouped controls, and collapsible Diagnostics. The circle cursor is retained. Fonts load from the Windows Fonts folder, with Tahoma and a readable built-in font as fallbacks; Windows font files are not redistributed. The panel height is constrained to the display, with scrolling for smaller windows.

## Configuration

Press **F10** in the game to show or hide the Dear ImGui panel. Change the cap using a preset, or enter a custom value and press **Apply** / Enter. **Unlimited**, **VSync**, the FPS counter and measurement logging can all be changed without restarting. Click **Save settings** to keep the settings for the next launch. **Reload saved** applies the INI file immediately if you edit it externally.

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

* `MaxFPS` accepts 1 through 1000, or 0 for uncapped. Actual FPS depends on hardware, scene complexity and driver limits.
* `DisableVSync=1` requests Present sync interval 0. Setting it to 0 (Use VSync checked) requests interval 1. No driver or monitor settings are changed.
* `Enabled=0` switches the owned scheduler to a 60 FPS limit and restores the game's requested presentation sync. The panel remains available so you can enable the unlock again live. Uninstalling restores the original scheduler on the next launch.
* Logging records measured presentation rate and the unchanged simulation clock every five seconds in `CTE_FPSPatch.log`.
* `ShowFPS` controls the small HUD. `OpenAtLaunch` controls whether the panel starts visible; F10 always opens it.

The panel does not pause gameplay. Use it in a menu or while paused. Mouse/keyboard input over the panel is captured when available; game controllers and direct input polling can still reach the game.

The INI supplied in this project folder is the installation default. After installation, use the copy in the main game folder.

## Installation and removal

Place this project folder directly inside the CALL to EXIST game folder (next to `CALL to EXIST.exe`). To install, close the game and copy `build/winmm.dll` and `CTE_FPSPatch.ini` into the main game folder. No `d3d11.dll` proxy is needed. Do not overwrite a different WINMM proxy from another mod.

Alternatively, run `Install.ps1` from PowerShell. It refuses a running game or an unrelated existing `winmm.dll`. It checks its own installed DLL's hash before moving or replacing it and keeps old patch binaries in `backups/`. It does not overwrite existing INI settings.

To uninstall, close the game and run this from PowerShell inside this folder:

```powershell
& '.\Install.ps1' -Uninstall
```

This moves this patch's hash-verified DLL out of the game folder. Settings and logs are retained. You can also move this specific `winmm.dll` out manually while the game is closed. Restarting without it restores the original in-memory scheduler code.

## What changes

The executable file is never edited. On the first WINMM call, the proxy checks the executable's identity, x64 architecture, build timestamp, image size and five separate timing/graphics code signatures. Unsupported or already-modified timing code is left unpatched; system WINMM calls still pass through.

For the validated build, it replaces only the six-byte scheduler wait branch in process memory. It then paces the scheduler's own QueryPerformanceCounter call using the selected target, a high-resolution waitable timer and a short final spin. The counter still returns the **real operating-system time**. Other QueryPerformanceCounter callers pass through unchanged. The game's nominal 60-based simulation clock, original clock-update routine, physics constants, animation constants and global time values are not modified.

The game's real swapchains are identified from its CreateSwapChain output array. Their Present entry is hooked to remove the VSync wait when requested and measure actual presentation rate. The previous Present function remains in the call chain. Dear ImGui renders through the game's actual D3D11 device and immediate context. Render state is restored, and backbuffer references are released before Present so they do not block resizing. A factory callback handles subsequent legacy swapchain recreation. There is no dummy swapchain or driver-wide detour.

The x64 forwarding stubs preserve integer, floating-point and stack arguments. The proxy matches all **181 exports**, including names and ordinals, of the installed Windows x64 WINMM library. Real functions are loaded from the absolute Windows System32 path; no Windows DLL is redistributed or replaced. DllMain does not load modules, patch memory or wait.

## Validation and limits

Version 1.1 was launched through Steam and tested in one running session: F10 show/hide, 60/144/240/360 FPS presets, a custom 200 FPS limit applied with Enter, uncapped mode, VSync interval 0/1, live enable/disable, HUD with logging disabled, Save defaults, and Reload defaults. The original saved 360 FPS setting was restored. The clock remained nominally 60 with 60.0000 simulation units/sec. All 181 WINMM names/ordinals resolve and timer calls match the system DLL. Startup window resizing worked; fullscreen/device-loss stress testing was not performed.


Verified here:

* Earlier 1.0 verification: real Steam startup and the forwarding/scheduler checks below. Version 1.1 adds the live ImGui panel; see `research/overlay-validation.json` for its separate results.
* On-screen counter showing 144 FPS and sustained logged measurements around 143–144 FPS.
* Engine nominal rate remains 60; logged simulation normalization remains 60.0000 units/sec, with a frame delta near 0.006944 seconds at 144 FPS.
* All 181 exported names/ordinals match and resolve. Time readings, timer-period calls, timer capabilities and a six-argument audio-format query match the real system DLL. The query opens no audio stream.
* The game executable retains its original SHA256 hash. ModelRipper's `d3d11.dll` is absent.

These checks verify the scheduler and startup/menu presentation. They do not establish that every combat mechanic, cutscene or multiplayer mode is free of frame-dependent behavior. Live limits are documented in the separate overlay validation report; exhaustive combat and multiplayer validation has not been performed.

The validated executable build timestamp is `5DF36448`; the active executable's SHA256 is `57c866f551b86622e107ceeac7346519a2ffec252804bedbc292bfa5921ab4ea`. Both measured image layouts (sizes `0xC7F000` and `0xCAC000`) are recognized only if all required loaded-code signatures match. An updated game may require revisiting the patch. Other mods that replace the scheduler or these exact COM vtable entries can conflict.

Research and test results are in `research/`. `CTE_FPSPatch.log` in the game folder is the live diagnostic log.

## Build

Run `build.cmd` with Visual Studio's Desktop C++ tools installed. It finds the x64 MSVC/MASM environment, compiles the patch with warnings treated as errors, and produces `build/winmm.dll`. Sources, the export definition and assembly forwarding stubs are included in `src/`. No third-party hooking library is required.

Dear ImGui v1.91.9b is vendored in `third_party/imgui`, including its official Win32/D3D11 backends and MIT license. `SOURCE.json` records download URLs and hashes. The overlay uses no additional injection or hooking library.

Version 1.2 input/cursor checks are in `research/input-cursor-validation.json`. Clicks, custom text entry, Enter/Apply, reload and cursor visibility were checked through Steam. Automated title dragging did not move the panel; dragging is not validated.
