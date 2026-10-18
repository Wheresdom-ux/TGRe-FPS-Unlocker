#pragma once
#include <atomic>
#include <dxgi.h>

namespace fps {
extern std::atomic<int> maxFPS;
extern std::atomic<bool> enabled, disableVsync, telemetry, showFPS, startOpen;
struct ClockInfo { int nominal = 0; double unitsPerSecond = 0.; };
bool SaveDefaults();
bool ReloadDefaults();
void SettingsChanged(const char* reason);
// The panel owns its ImGui context, backend resources and window-input handler.
void DrawOverlay(IDXGISwapChain* swap, const ClockInfo& clock);
}
