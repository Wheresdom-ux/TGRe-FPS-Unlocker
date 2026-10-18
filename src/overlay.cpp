#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <stdio.h>
#include "overlay.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM, LPARAM);
using Microsoft::WRL::ComPtr;
namespace fps {
static SRWLOCK uiLock = SRWLOCK_INIT;
static ImGuiContext* context;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> immediate;
static HWND window;
static WNDPROC originalProc;
static std::atomic<bool> menuOpen{true};
static bool initialized;
static int draftLimit = 144;
static int rememberedLimit = 144;
static char status[160]{};
static ImFont* bodyFont;
static ImFont* headingFont;
static ImFont* numberFont;
static ImFont* smallFont;
static ImFont* LoadWindowsFont(const wchar_t* filename, float size) {
    wchar_t path[MAX_PATH]{};
    if (!GetWindowsDirectoryW(path, MAX_PATH)) return nullptr;
    wcscat_s(path, L"\\Fonts\\"); wcscat_s(path, filename);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    char utf8[MAX_PATH * 3]{};
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr)) return nullptr;
    ImFontConfig config{}; config.OversampleH = 3; config.OversampleV = 2;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(utf8, size, &config);
}
static void SmallText(const char* text) {
    ImGui::PushFont(smallFont); ImGui::TextDisabled("%s", text); ImGui::PopFont();
}
static void SectionLabel(const char* text) {
    ImGui::Spacing(); ImGui::PushFont(headingFont); ImGui::TextUnformatted(text); ImGui::PopFont();
    ImGui::Spacing();
}

// Only the render thread touches ImGui. Window messages use a separate short-lived
// queue lock, so rendering can never discard an event or block the window thread.
struct InputMessage { HWND hwnd; UINT message; WPARAM wparam; LPARAM lparam; POINT point; unsigned modifiers; };
static SRWLOCK inputLock = SRWLOCK_INIT;
static InputMessage inputQueue[1024]{};
static size_t inputCount;
static bool inputOverflow;
static std::atomic<bool> captureMouse{false}, captureKeyboard{false};
static std::atomic<LONG> panelLeft{0}, panelTop{0}, panelRight{0}, panelBottom{0};
static std::atomic<WNDPROC> gameProc{nullptr};
static bool OverPanel(HWND hwnd) {
    if (!menuOpen.load() || GetForegroundWindow() != hwnd) return false;
    POINT point{};
    if (!GetCursorPos(&point) || !ScreenToClient(hwnd, &point)) return false;
    return point.x >= panelLeft.load() && point.x < panelRight.load() &&
        point.y >= panelTop.load() && point.y < panelBottom.load();
}
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto oldProc = gameProc.load();
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F10) {
        menuOpen.store(!menuOpen.load()); return 0;
    }
    if ((message == WM_SYSKEYDOWN || message == WM_KEYDOWN) && wparam == VK_F10) return 0;
    const bool mouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    const bool keyboard = message >= WM_KEYFIRST && message <= WM_KEYLAST;
    const bool focus = message == WM_SETFOCUS || message == WM_KILLFOCUS;
    // Preserve pointer motion as well as click edges, wheel and text.
    if (mouse || keyboard || focus) {
        InputMessage event{hwnd, message, wparam, lparam, {}, 0};
        // Preserve modifiers at receipt: a complete Ctrl+V chord may arrive
        // between frames, after Windows already reports that Ctrl is released.
        if (GetKeyState(VK_CONTROL) & 0x8000) event.modifiers |= 1;
        if (GetKeyState(VK_SHIFT) & 0x8000) event.modifiers |= 2;
        if (GetKeyState(VK_MENU) & 0x8000) event.modifiers |= 4;
        if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) event.modifiers |= 8;
        GetCursorPos(&event.point); ScreenToClient(hwnd, &event.point);
        if (mouse && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) {
            event.point.x = static_cast<short>(LOWORD(lparam));
            event.point.y = static_cast<short>(HIWORD(lparam));
        }
        AcquireSRWLockExclusive(&inputLock);
        if (inputCount < _countof(inputQueue)) inputQueue[inputCount++] = event;
        else inputOverflow = true;
        ReleaseSRWLockExclusive(&inputLock);
    }
    if (message == WM_SYSKEYDOWN && wparam == VK_F4)
        return oldProc ? CallWindowProcW(oldProc, hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    const bool over = OverPanel(hwnd);
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && over) { SetCursor(nullptr); return TRUE; }
    if (menuOpen.load() && ((mouse && (over || captureMouse.load())) || (keyboard && captureKeyboard.load()))) return 0;
    return oldProc ? CallWindowProcW(oldProc, hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}
static void UpdateInput(const D3D11_TEXTURE2D_DESC& buffer) {
    ImGuiIO& io = ImGui::GetIO();
    RECT client{}; GetClientRect(window, &client);
    const float sx = client.right > 0 ? static_cast<float>(buffer.Width) / static_cast<float>(client.right) : 1.f;
    const float sy = client.bottom > 0 ? static_cast<float>(buffer.Height) / static_cast<float>(client.bottom) : 1.f;
    // Draw and hit-test in the same backbuffer coordinates, including resized windows.
    io.DisplaySize = ImVec2(static_cast<float>(buffer.Width), static_cast<float>(buffer.Height));
    InputMessage events[1024]; size_t count; bool overflow;
    AcquireSRWLockExclusive(&inputLock);
    count = inputCount; memcpy(events, inputQueue, count * sizeof(InputMessage)); inputCount = 0;
    overflow = inputOverflow; inputOverflow = false;
    ReleaseSRWLockExclusive(&inputLock);
    if (overflow) { io.ClearInputKeys(); io.ClearInputMouse(); }
    for (size_t i = 0; i < count; ++i) {
        const auto& event = events[i];
        if (event.hwnd != window) continue;
        if (event.message >= WM_MOUSEFIRST && event.message <= WM_MOUSELAST)
            io.AddMousePosEvent(static_cast<float>(event.point.x) * sx, static_cast<float>(event.point.y) * sy);
        if (event.message == WM_MOUSEMOVE) continue; // Position already queued in backbuffer coordinates.
        if (event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN || event.message == WM_KEYUP || event.message == WM_SYSKEYUP) {
            io.AddKeyEvent(ImGuiMod_Ctrl, (event.modifiers & 1) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (event.modifiers & 2) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (event.modifiers & 4) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (event.modifiers & 8) != 0);
            const auto key = ImGui_ImplWin32_KeyEventToImGuiKey(event.wparam, event.lparam);
            if (key != ImGuiKey_None) io.AddKeyEvent(key, event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN);
        } else ImGui_ImplWin32_WndProcHandler(event.hwnd, event.message, event.wparam, event.lparam);
    }
    const bool focused = GetForegroundWindow() == window;
    io.AddFocusEvent(focused);
    POINT point{};
    if (focused && GetCursorPos(&point) && ScreenToClient(window, &point))
        io.AddMousePosEvent(static_cast<float>(point.x) * sx, static_cast<float>(point.y) * sy);
    else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    const int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int i = 0; i < 5; ++i) io.AddMouseButtonEvent(i, focused && (GetAsyncKeyState(buttons[i]) & 0x8000) != 0);
    // Fallback for games that consume keyboard messages before dispatching them.
    for (int vk = VK_BACK; vk <= VK_OEM_102; ++vk) {
        if (vk == VK_F10 || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
        const auto key = ImGui_ImplWin32_KeyEventToImGuiKey(static_cast<WPARAM>(vk), static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, focused && (GetAsyncKeyState(vk) & 0x8000) != 0);
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, focused && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, focused && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
}
static void DrawMenuCursor() {
    ImGuiIO& io = ImGui::GetIO();
    const bool hover = menuOpen.load() && GetForegroundWindow() == window &&
        (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || io.WantCaptureMouse);
    captureMouse.store(hover);
    captureKeyboard.store(menuOpen.load() && io.WantCaptureKeyboard);
    static HCURSOR previousCursor;
    static bool wasHover;
    if (hover) {
        const auto previous = SetCursor(nullptr);
        if (!wasHover && previous) previousCursor = previous;
        const ImVec2 p = io.MousePos;
        auto* draw = ImGui::GetForegroundDrawList();
        // A circle centered exactly on the click position, rendered by the GPU.
        draw->AddCircleFilled(ImVec2(p.x+1.5f, p.y+2.f), 9.f, IM_COL32(0,0,0,180), 32);
        draw->AddCircleFilled(p, 7.f, IM_COL32(240,55,85,255), 32);
        draw->AddCircle(p, 7.f, IM_COL32(255,245,248,255), 32, 1.5f);
    } else if (wasHover) SetCursor(previousCursor ? previousCursor : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
    wasHover = hover;
}
static void DetachWindow() {
    if (window && IsWindow(window) && originalProc &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == WindowProc) {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProc));
    }
}
static bool Initialize(IDXGISwapChain* swap, const DXGI_SWAP_CHAIN_DESC& desc) {
    ComPtr<ID3D11Device> current;
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&current)))) return false;
    if (initialized && current.Get() == device.Get() && desc.OutputWindow == window) return true;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    if (initialized) {
        DetachWindow();
        ImGui::SetCurrentContext(context);
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown();
        immediate.Reset(); device.Reset(); initialized = false;
    }
    if (!context) {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NoMouseCursorChange;
        io.FontGlobalScale = 1.f;
        bodyFont = LoadWindowsFont(L"segoeui.ttf", 18.f);
        if (!bodyFont) bodyFont = LoadWindowsFont(L"tahoma.ttf", 18.f);
        if (!bodyFont) { ImFontConfig fallback{}; fallback.SizePixels = 18.f; bodyFont = io.Fonts->AddFontDefault(&fallback); }
        io.FontDefault = bodyFont;
        headingFont = LoadWindowsFont(L"segoeuib.ttf", 21.f);
        numberFont = LoadWindowsFont(L"segoeui.ttf", 34.f);
        smallFont = LoadWindowsFont(L"segoeui.ttf", 15.f);
        if (!headingFont) headingFont = bodyFont;
        if (!numberFont) numberFont = bodyFont;
        if (!smallFont) smallFont = bodyFont;
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowRounding = 7.f; style.FrameRounding = 4.f;
        style.WindowBorderSize = 1.f; style.FrameBorderSize = 1.f;
        style.WindowPadding = ImVec2(24.f, 20.f); style.ItemSpacing = ImVec2(8.f, 10.f);
        style.FramePadding = ImVec2(10.f, 7.f); style.ScrollbarSize = 10.f;
        style.GrabRounding = 3.f; style.ScrollbarRounding = 4.f;
        style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.93f, 1.f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.61f, 0.62f, 0.65f, 1.f);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.08f, 0.09f, 0.99f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.25f, 1.f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.12f, 0.135f, 1.f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.18f, 0.20f, 1.f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.21f, 0.23f, 1.f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.14f, 0.15f, 0.17f, 1.f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.23f, 0.25f, 1.f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.29f, 0.31f, 1.f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.85f, 0.32f, 0.35f, 1.f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.21f, 0.23f, 1.f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.26f, 0.28f, 1.f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.20f, 0.22f, 1.f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.21f, 0.23f, 1.f);
        style.Colors[ImGuiCol_NavCursor] = ImVec4(0.85f, 0.32f, 0.35f, 1.f);
        menuOpen.store(startOpen.load());
        draftLimit = rememberedLimit = maxFPS.load() > 0 ? maxFPS.load() : 144;
    }
    ImGui::SetCurrentContext(context);
    device = current; current->GetImmediateContext(&immediate);
    window = desc.OutputWindow;
    if (!window || !immediate || !ImGui_ImplWin32_Init(window)) { ImGui::SetCurrentContext(previous); return false; }
    if (!ImGui_ImplDX11_Init(device.Get(), immediate.Get())) {
        ImGui_ImplWin32_Shutdown(); ImGui::SetCurrentContext(previous); return false;
    }
    SetLastError(0);
    const auto old = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc));
    if (!old && GetLastError()) {
        ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown();
        ImGui::SetCurrentContext(previous); return false;
    }
    originalProc = reinterpret_cast<WNDPROC>(old);
    gameProc.store(originalProc);
    initialized = true;
    SettingsChanged("ImGui overlay initialized; F10 toggles the panel");
    ImGui::SetCurrentContext(previous);
    return true;
}
static void SetLimit(int limit) {
    if (limit < 0) limit = 0;
    if (limit > 1000) limit = 1000;
    maxFPS.store(limit);
    if (limit) draftLimit = rememberedLimit = limit;
    SettingsChanged("FPS limit changed live");
}
static void DrawPanel(const ClockInfo& clock) {
    ImGuiIO& io = ImGui::GetIO();
    bool open = menuOpen.load();
    const float width = io.DisplaySize.x < 520.f ? io.DisplaySize.x - 24.f : 460.f;
    ImGui::SetNextWindowSize(ImVec2(width, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, io.DisplaySize.y - 36.f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - width - 24.f, 24.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Performance##CTE_Settings", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse)) {
        SmallText("CALL to EXIST");
        ImGui::PushFont(headingFont); ImGui::TextUnformatted("Performance"); ImGui::PopFont();
        ImGui::Spacing();
        if (ImGui::BeginTable("##readings", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn(); SmallText("Frame rate");
            ImGui::PushFont(numberFont); ImGui::Text("%.0f", io.Framerate); ImGui::PopFont();
            ImGui::SameLine(0.f, 6.f); SmallText("FPS");
            ImGui::TableNextColumn(); SmallText("Frame time");
            ImGui::PushFont(numberFont); ImGui::Text("%.2f", io.Framerate > 0.f ? 1000.f / io.Framerate : 0.f); ImGui::PopFont();
            ImGui::SameLine(0.f, 6.f); SmallText("ms");
            ImGui::EndTable();
        }
        ImGui::Spacing(); ImGui::Separator();
        SectionLabel("Frame rate");
        bool unlocked = enabled.load();
        if (ImGui::Checkbox("Unlock frame rate", &unlocked)) { enabled.store(unlocked); SettingsChanged("FPS unlock toggled live"); }
        ImGui::BeginDisabled(!unlocked);
        bool uncapped = maxFPS.load() == 0;
        if (ImGui::Checkbox("Unlimited", &uncapped)) SetLimit(uncapped ? 0 : rememberedLimit);
        ImGui::BeginDisabled(uncapped);
        const int presets[] = {60, 120, 144, 165, 240, 360};
        const float presetWidth = (ImGui::GetContentRegionAvail().x - 5.f * ImGui::GetStyle().ItemSpacing.x) / 6.f;
        for (int i=0; i<6; ++i) {
            if (i) ImGui::SameLine();
            const bool selected = maxFPS.load() == presets[i];
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.17f, 0.20f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.70f, 0.30f, 0.34f, 1.f));
            }
            char label[20]; sprintf_s(label, "%d", presets[i]);
            if (ImGui::Button(label, ImVec2(presetWidth, 34.f))) SetLimit(presets[i]);
            if (selected) ImGui::PopStyleColor(2);
        }
        SmallText("Custom limit (FPS)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 94.f);
        ImGui::InputInt("##CustomFPS", &draftLimit, 0, 0, ImGuiInputTextFlags_AutoSelectAll);
        const bool enter = ImGui::IsItemDeactivatedAfterEdit() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
        ImGui::SameLine();
        const bool apply = ImGui::Button("Apply", ImVec2(86.f, 0.f));
        if (enter || apply) SetLimit(draftLimit < 1 ? 1 : draftLimit);
        ImGui::EndDisabled();
        bool vsync = !disableVsync.load();
        if (ImGui::Checkbox("VSync", &vsync)) { disableVsync.store(!vsync); SettingsChanged("VSync changed live"); }
        SmallText("Sync frames to your display's refresh rate.");
        ImGui::EndDisabled();
        ImGui::Spacing(); ImGui::Separator();
        SectionLabel("Overlay");
        bool hud = showFPS.load();
        if (ImGui::Checkbox("Show FPS counter", &hud)) showFPS.store(hud);
        bool startup = startOpen.load();
        if (ImGui::Checkbox("Open menu at launch", &startup)) startOpen.store(startup);
        ImGui::Spacing();
        if (ImGui::TreeNodeEx("Diagnostics", ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
            bool logging = telemetry.load();
            if (ImGui::Checkbox("Write performance log", &logging)) { telemetry.store(logging); SettingsChanged("Measurement logging changed live"); }
            if (clock.nominal > 0) {
                ImGui::PushFont(smallFont);
                ImGui::TextDisabled("Engine clock: %d / %.1f units per second", clock.nominal, clock.unitsPerSecond);
                ImGui::PopFont();
            }
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        const float buttonWidth = (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.17f, 0.20f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.53f, 0.22f, 0.25f, 1.f));
        if (ImGui::Button("Save settings", ImVec2(buttonWidth, 37.f))) {
            sprintf_s(status, "%s", SaveDefaults() ? "Settings saved." : "Unable to save. Check folder permissions.");
        }
        ImGui::PopStyleColor(2); ImGui::SameLine();
        if (ImGui::Button("Reload saved", ImVec2(buttonWidth, 37.f))) {
            const bool loaded = ReloadDefaults();
            if (loaded) draftLimit = rememberedLimit = maxFPS.load() > 0 ? maxFPS.load() : 144;
            sprintf_s(status, "%s", loaded ? "Saved settings restored." : "Invalid saved limit. Settings kept.");
        }
        SmallText(status[0] ? status : "Changes apply immediately. Save to keep them.");
    }
    const ImVec2 position = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    RECT client{}; GetClientRect(window, &client);
    const float sx = io.DisplaySize.x > 0.f ? static_cast<float>(client.right) / io.DisplaySize.x : 1.f;
    const float sy = io.DisplaySize.y > 0.f ? static_cast<float>(client.bottom) / io.DisplaySize.y : 1.f;
    panelLeft.store(static_cast<LONG>(position.x*sx)); panelTop.store(static_cast<LONG>(position.y*sy));
    panelRight.store(static_cast<LONG>((position.x+size.x)*sx)); panelBottom.store(static_cast<LONG>((position.y+size.y)*sy));
    ImGui::End(); menuOpen.store(open);
}
void DrawOverlay(IDXGISwapChain* swap, const ClockInfo& clock) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap->GetDesc(&desc)) || !IsWindow(desc.OutputWindow)) return;
    AcquireSRWLockExclusive(&uiLock);
    if (!Initialize(swap, desc)) { ReleaseSRWLockExclusive(&uiLock); return; }

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context);
    ComPtr<ID3D11Texture2D> backbuffer;
    ComPtr<ID3D11RenderTargetView> target;
    // Hold no backbuffer references between frames: ResizeBuffers remains free to run.
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &target))) {
        ImGui::SetCurrentContext(previous); ReleaseSRWLockExclusive(&uiLock); return;
    }
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
    D3D11_TEXTURE2D_DESC buffer{}; backbuffer->GetDesc(&buffer);
    UpdateInput(buffer); ImGui::NewFrame();
    if (menuOpen.load()) DrawPanel(clock);
    if (showFPS.load()) {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(14.f, 14.f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.80f);
        if (ImGui::Begin("##CTE_FPS_HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::Text("%.0f FPS", io.Framerate);
            SmallText("F10  Performance");
        }
        ImGui::End();
    }
    DrawMenuCursor();
    ImGui::Render();
    ID3D11RenderTargetView* oldTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* oldDepth = nullptr;
    immediate->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, &oldDepth);
    ID3D11RenderTargetView* ourTarget = target.Get();
    immediate->OMSetRenderTargets(1, &ourTarget, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    immediate->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, oldDepth);
    for (auto* view : oldTargets) if (view) view->Release();
    if (oldDepth) oldDepth->Release();
    ImGui::SetCurrentContext(previous);
    ReleaseSRWLockExclusive(&uiLock);
}
}

