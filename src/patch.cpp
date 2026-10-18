#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "exports.inc"
#include "overlay.h"

extern "C" { void* g_exports[kExportCount] = {}; }
static HMODULE selfModule;
static HMODULE realWinmm;
static INIT_ONCE initOnce = INIT_ONCE_STATIC_INIT;
static SRWLOCK logLock = SRWLOCK_INIT;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static wchar_t iniPath[MAX_PATH];
static uintptr_t gameBase;
static volatile LONG running = 1;
namespace fps {
std::atomic<int> maxFPS{144};
std::atomic<bool> enabled{true}, disableVsync{true}, telemetry{true}, showFPS{true}, startOpen{true};
}
using fps::maxFPS;
using fps::enabled;
using fps::disableVsync;
using fps::telemetry;
static LARGE_INTEGER frequency;
static volatile LONG64 pacedFrames = 0;
using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
static QpcFn originalQpc = QueryPerformanceCounter;
static QpcFn systemQpc = QueryPerformanceCounter;

static void Log(const char* format, ...) {
    if (logFile == INVALID_HANDLE_VALUE) return;
    char line[1400]; SYSTEMTIME time{}; GetLocalTime(&time);
    int n = sprintf_s(line, "[%02u:%02u:%02u.%03u] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    va_list args; va_start(args, format);
    vsnprintf_s(line + n, sizeof(line) - static_cast<size_t>(n) - 3, _TRUNCATE, format, args);
    va_end(args); strcat_s(line, "\r\n");
    AcquireSRWLockExclusive(&logLock);
    DWORD written; WriteFile(logFile, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    ReleaseSRWLockExclusive(&logLock);
}
static void Paths() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(selfModule, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    wcscpy_s(iniPath, path); wcscat_s(iniPath, L"CTE_FPSPatch.ini");
    wcscat_s(path, L"CTE_FPSPatch.log");
    logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
namespace fps {
void SettingsChanged(const char* reason) {
    Log("%s; enabled=%d, limit=%d, VSync=%d, logging=%d", reason, enabled.load(), maxFPS.load(), !disableVsync.load(), telemetry.load());
}
bool ReloadDefaults() {
    const UINT limit = GetPrivateProfileIntW(L"FPS", L"MaxFPS", 144, iniPath);
    if (limit > 1000) { Log("Invalid saved MaxFPS: expected 0..1000; settings kept"); return false; }
    maxFPS.store(static_cast<int>(limit));
    enabled.store(GetPrivateProfileIntW(L"FPS", L"Enabled", 1, iniPath) != 0);
    disableVsync.store(GetPrivateProfileIntW(L"FPS", L"DisableVSync", 1, iniPath) != 0);
    telemetry.store(GetPrivateProfileIntW(L"FPS", L"LogMeasurements", 1, iniPath) != 0);
    showFPS.store(GetPrivateProfileIntW(L"Overlay", L"ShowFPS", 1, iniPath) != 0);
    startOpen.store(GetPrivateProfileIntW(L"Overlay", L"OpenAtLaunch", 1, iniPath) != 0);
    SettingsChanged("Defaults loaded"); return true;
}
bool SaveDefaults() {
    bool ok = true;
    auto write = [&](const wchar_t* section, const wchar_t* key, int value) {
        wchar_t buffer[32]; swprintf_s(buffer, L"%d", value);
        if (!WritePrivateProfileStringW(section, key, buffer, iniPath)) ok = false;
    };
    write(L"FPS", L"Enabled", enabled.load()); write(L"FPS", L"MaxFPS", maxFPS.load());
    write(L"FPS", L"DisableVSync", disableVsync.load()); write(L"FPS", L"LogMeasurements", telemetry.load());
    write(L"Overlay", L"ShowFPS", showFPS.load()); write(L"Overlay", L"OpenAtLaunch", startOpen.load());
    Log(ok ? "Defaults saved" : "Could not save all defaults"); return ok;
}
}
static bool Readable(const void* p, size_t length) {
    MEMORY_BASIC_INFORMATION m{};
    if (!p || !VirtualQuery(p, &m, sizeof(m))) return false;
    const auto end = reinterpret_cast<uintptr_t>(p) + length;
    return m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        end >= reinterpret_cast<uintptr_t>(p) && end <= reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
}
static bool ReplaceBytes(void* address, const void* bytes, size_t length) {
    DWORD old;
    if (!VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(address, bytes, length);
    FlushInstructionCache(GetCurrentProcess(), address, length);
    DWORD ignored; VirtualProtect(address, length, old, &ignored);
    return true;
}
static bool ExchangePointer(void** slot, void* replacement, void** original) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = InterlockedExchangePointer(slot, replacement);
    DWORD ignored; VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

// Only the scheduler's QPC call is paced. Every returned tick is the real OS tick;
// unrelated game calls are forwarded without changing their timing or values.
static BOOL WINAPI SchedulerCounter(LARGE_INTEGER* output) {
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller != gameBase + 0x32cb26) return originalQpc(output);
    const int limit = enabled.load() ? maxFPS.load() : 60;
    static DWORD ownerThread = 0;
    const DWORD thread = GetCurrentThreadId();
    if (!ownerThread) ownerThread = thread;
    if (thread != ownerThread) return originalQpc(output);
    static double next = 0.;
    static int lastLimit = -1;
    if (limit != lastLimit) { next = 0.; lastLimit = limit; }
    if (!limit) return originalQpc(output);
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_MODIFY_STATE | SYNCHRONIZE);
    LARGE_INTEGER now{};
    if (!originalQpc(&now)) return FALSE;
    const double interval = static_cast<double>(frequency.QuadPart) / limit;
    if (!next || static_cast<double>(now.QuadPart) - next > interval) next = static_cast<double>(now.QuadPart);
    while (static_cast<double>(now.QuadPart) < next) {
        const double seconds = (next - static_cast<double>(now.QuadPart)) / static_cast<double>(frequency.QuadPart);
        if (seconds > 0.0003 && timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>((seconds - 0.00015) * 10000000.);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
            else Sleep(0);
        } else if (seconds > 0.002) {
            Sleep(static_cast<DWORD>(seconds * 1000.) - 1);
        } else {
            YieldProcessor();
        }
        if (!originalQpc(&now)) return FALSE;
    }
    next += interval;
    if (output) *output = now;
    InterlockedIncrement64(&pacedFrames);
    return TRUE;
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
struct PresentHook { void** table; PresentFn original; };
static PresentHook hooks[16]{};
static LONG hookCount;
static SRWLOCK hookLock = SRWLOCK_INIT;
static SRWLOCK statsLock = SRWLOCK_INIT;
static LARGE_INTEGER sampleStart{};
static UINT64 sampleFrames;
static double sampleSeconds;
static double sampleFrameUnits;
static UINT64 sampleClocks;
static IDXGISwapChain* measuredSwap;

static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    PresentFn original = nullptr;
    void** table = *reinterpret_cast<void***>(swap);
    AcquireSRWLockShared(&hookLock);
    for (LONG i = 0; i < hookCount; ++i) if (hooks[i].table == table) { original = hooks[i].original; break; }
    ReleaseSRWLockShared(&hookLock);
    if (!original) return DXGI_ERROR_INVALID_CALL;
    if (flags & DXGI_PRESENT_TEST) return original(swap, sync, flags);
    fps::ClockInfo info{};
    const auto clock = *reinterpret_cast<uintptr_t*>(gameBase + 0xbcc5e0);
    float dt = 0.f, units = 0.f;
    if (clock >= gameBase && clock + 0x48 < gameBase + 0xc7f000 && Readable(reinterpret_cast<void*>(clock), 0x48)) {
        dt = *reinterpret_cast<volatile float*>(clock + 0x40);
        units = *reinterpret_cast<volatile float*>(clock + 0x44);
        info.nominal = *reinterpret_cast<volatile int*>(clock + 0x10);
        if (isfinite(dt) && isfinite(units) && dt > 0.f && dt < 1.f) info.unitsPerSecond = units / dt;
        else { dt = 0.f; units = 0.f; }
    }
    fps::DrawOverlay(swap, info);
    const UINT effectiveSync = enabled.load() ? (disableVsync.load() ? 0u : 1u) : sync;
    LARGE_INTEGER now{}; systemQpc(&now);
    AcquireSRWLockExclusive(&statsLock);
    if (measuredSwap != swap || !telemetry.load()) {
        measuredSwap = swap; sampleStart = now; sampleFrames = 0;
        sampleSeconds = 0.; sampleFrameUnits = 0.; sampleClocks = 0;
    }
    if (telemetry.load()) {
        ++sampleFrames;
        if (dt > 0.f) { sampleSeconds += dt; sampleFrameUnits += units; ++sampleClocks; }
        const double elapsed = static_cast<double>(now.QuadPart - sampleStart.QuadPart) / static_cast<double>(frequency.QuadPart);
        if (elapsed >= 5.) {
            Log("Measured %.2f FPS; render scheduler target=%d; engine nominal=%d; simulation normalization=%.4f units/sec; sampled dt=%.6f sec; paced frames=%lld; requested Present sync=%u, effective=%u",
                static_cast<double>(sampleFrames) / elapsed, enabled.load() ? maxFPS.load() : 60, info.nominal,
                sampleSeconds > 0. ? sampleFrameUnits / sampleSeconds : 0., sampleClocks ? sampleSeconds / static_cast<double>(sampleClocks) : 0.,
                InterlockedCompareExchange64(&pacedFrames, 0, 0), sync, effectiveSync);
            sampleStart = now; sampleFrames = 0; sampleSeconds = 0.; sampleFrameUnits = 0.; sampleClocks = 0;
        }
    }
    ReleaseSRWLockExclusive(&statsLock);
    return original(swap, effectiveSync, flags);
}
static void HookSwap(uintptr_t pointer) {
    if (!Readable(reinterpret_cast<void*>(pointer), sizeof(void*))) return;
    auto table = *reinterpret_cast<void***>(pointer);
    if (!Readable(table, 18 * sizeof(void*))) return;
    AcquireSRWLockExclusive(&hookLock);
    for (LONG i = 0; i < hookCount; ++i) if (hooks[i].table == table) { ReleaseSRWLockExclusive(&hookLock); return; }
    if (hookCount < 16) {
        // Publish the original before redirecting the aligned, atomic vtable slot.
        auto& h = hooks[hookCount]; h.table = table; h.original = reinterpret_cast<PresentFn>(table[8]);
        ++hookCount;
        void* previous = nullptr;
        if (ExchangePointer(table + 8, reinterpret_cast<void*>(&Present), &previous)) {
            h.original = reinterpret_cast<PresentFn>(previous);
            Log("Present hooked at swapchain=%p, table=%p; existing graphics proxy retained", reinterpret_cast<void*>(pointer), table);
        } else { --hookCount; Log("Could not hook Present: Windows error %lu", GetLastError()); }
    }
    ReleaseSRWLockExclusive(&hookLock);
}
using CreateSwapFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
struct FactoryHook { void** table; CreateSwapFn original; };
static FactoryHook factoryHooks[16]{};
static LONG factoryCount;
static HRESULT STDMETHODCALLTYPE CreateSwap(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** output) {
    CreateSwapFn original = nullptr;
    void** table = *reinterpret_cast<void***>(factory);
    AcquireSRWLockShared(&hookLock);
    for (LONG i=0; i<factoryCount; ++i) if (factoryHooks[i].table == table) { original = factoryHooks[i].original; break; }
    ReleaseSRWLockShared(&hookLock);
    if (!original) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = original(factory, device, desc, output);
    if (SUCCEEDED(hr) && output && *output) HookSwap(reinterpret_cast<uintptr_t>(*output));
    return hr;
}
static bool HookFactory(IDXGISwapChain* swap) {
    IDXGIFactory* factory = nullptr;
    if (FAILED(swap->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) return false;
    void** table = *reinterpret_cast<void***>(factory);
    bool result = false;
    AcquireSRWLockExclusive(&hookLock);
    for (LONG i=0; i<factoryCount; ++i) if (factoryHooks[i].table == table) result = true;
    if (!result && factoryCount < 16) {
        auto& h = factoryHooks[factoryCount]; h.table = table; h.original = reinterpret_cast<CreateSwapFn>(table[10]); ++factoryCount;
        void* previous = nullptr;
        if (ExchangePointer(table+10, reinterpret_cast<void*>(&CreateSwap), &previous)) {
            h.original = reinterpret_cast<CreateSwapFn>(previous); result = true;
        } else --factoryCount;
    }
    ReleaseSRWLockExclusive(&hookLock);
    factory->Release();
    if (result) Log("Swapchain factory hooked; future recreation will be handled without polling released objects");
    return result;
}
static DWORD WINAPI WatchSwapchains(void*) {
    // The game stores at most TWO swapchains here. The next slot is its D3D device.
    // After discovery, use the factory callback for future object lifetimes.
    for (int attempts=0; attempts<240 && InterlockedCompareExchange(&running, 1, 1); ++attempts) {
        const UINT count = *reinterpret_cast<volatile UINT*>(gameBase + 0xb3c888);
        bool found = false;
        for (size_t i = 0; i < count && i < 2; ++i) {
            const uintptr_t pointer = *reinterpret_cast<volatile uintptr_t*>(gameBase + 0xb3c910 + i * sizeof(uintptr_t));
            if (!Readable(reinterpret_cast<void*>(pointer), sizeof(void*))) continue;
            auto unknown = reinterpret_cast<IUnknown*>(pointer);
            IDXGISwapChain* swap = nullptr;
            if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&swap)))) {
                HookSwap(reinterpret_cast<uintptr_t>(swap));
                found = HookFactory(swap) || found;
                swap->Release();
            }
        }
        if (found) return 0;
        Sleep(250);
    }
    Log("Swapchain discovery timed out; inspect the measured frame rate and VSync settings");
    return 0;
}
static bool ValidGame() {
    wchar_t name[MAX_PATH]{}; GetModuleFileNameW(nullptr, name, MAX_PATH);
    const wchar_t* leaf = wcsrchr(name, L'\\'); leaf = leaf ? leaf + 1 : name;
    if (_wcsicmp(leaf, L"CALL to EXIST.exe")) { Log("Pass-through mode: host is not CALL to EXIST.exe"); return false; }
    auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return false;
    auto pe = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe->FileHeader.TimeDateStamp != 0x5df36448 ||
        (pe->OptionalHeader.SizeOfImage != 0xc7f000 && pe->OptionalHeader.SizeOfImage != 0xcac000)) {
        Log("Unsupported executable version: patch refused; WINMM remains pass-through"); return false;
    }
    const BYTE load[] = {0x48,0x8b,0x05,0xe1,0xfa,0x89,0x00,0x48,0x63,0x48,0x10,0x48,0x8b,0x84,0x24,0x88,0,0,0};
    const BYTE wait[] = {0x0f,0x8c,0x44,0xff,0xff,0xff,0xe8,0xbf,0xcf,0x04,0x00};
    const BYTE clock[] = {0x48,0x8d,0x05,0xf1,0x2f,0x6d,0x00,0xc7,0x05,0x0f,0x92,0x8b,0,0x3c,0,0,0};
    const BYTE swap[] = {0x48,0x8d,0x3d,0xde,0x36,0x7c,0};
    const BYTE counter[] = {0xff,0x15,0xfa,0x06,0x1e,0x00};
    if (memcmp(base+0x32caf8,load,sizeof(load)) || memcmp(base+0x32cb66,wait,sizeof(wait)) ||
        memcmp(base+0x3133e0,clock,sizeof(clock)) || memcmp(base+0x37922b,swap,sizeof(swap)) || memcmp(base+0x32cb20,counter,sizeof(counter))) {
        Log("Timing/graphics signatures differ: patch refused; executable and other DLLs left untouched"); return false;
    }
    gameBase = reinterpret_cast<uintptr_t>(base);
    Log("Validated executable build 5DF36448 at %p; rendering limiter located; 60-based simulation clock will remain unchanged", base);
    return true;
}
static void Apply() {
    Paths(); Log("--- CALL to EXIST FPS proxy 1.3 with Dear ImGui (x64) ---");
    if (!ValidGame()) return;
    fps::ReloadDefaults();
    QueryPerformanceFrequency(&frequency);
    systemQpc = reinterpret_cast<QpcFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryPerformanceCounter"));
    const BYTE nops[6] = {0x90,0x90,0x90,0x90,0x90,0x90};
    void* previous = nullptr;
    // This code runs on the game's first WINMM call, before the scheduler enters its wait loop.
    if (!ExchangePointer(reinterpret_cast<void**>(gameBase + 0x50d220), reinterpret_cast<void*>(&SchedulerCounter), &previous)) {
        Log("Could not install scheduler counter: Windows error %lu. Patch refused.", GetLastError()); return;
    }
    originalQpc = reinterpret_cast<QpcFn>(previous);
    if (!ReplaceBytes(reinterpret_cast<void*>(gameBase + 0x32cb66), nops, sizeof(nops))) {
        void* ignored; ExchangePointer(reinterpret_cast<void**>(gameBase + 0x50d220), previous, &ignored);
        Log("Could not remove scheduler wait: Windows error %lu. Original counter restored.", GetLastError()); return;
    }
    Log("Scheduler patched in memory only; MaxFPS=%d (0=uncapped), DisableVSync=%d; QPC values remain real; game clock divisor remains 60", maxFPS.load(), disableVsync.load());
    // Keep our function pointers valid until process exit, even if a client releases WINMM.
    HMODULE pinned{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&SchedulerCounter), &pinned);
    HANDLE thread = CreateThread(nullptr, 0, &WatchSwapchains, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread); else Log("Could not start graphics watcher: Windows error %lu", GetLastError());
}
static BOOL CALLBACK Initialize(PINIT_ONCE, void*, void**) {
    wchar_t systemPath[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(systemPath, MAX_PATH);
    if (!n || n > MAX_PATH - 12) return FALSE;
    wcscat_s(systemPath, L"\\winmm.dll");
    realWinmm = LoadLibraryExW(systemPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!realWinmm || realWinmm == selfModule) return FALSE;
    for (size_t i=0; i<kExportCount; ++i) {
        const auto& e = kExports[i];
        g_exports[i] = reinterpret_cast<void*>(GetProcAddress(realWinmm, e.name ? e.name : MAKEINTRESOURCEA(e.ordinal)));
    }
    Apply();
    return TRUE;
}
extern "C" void* ResolveExport(size_t index) {
    if (!InitOnceExecuteOnce(&initOnce, Initialize, nullptr, nullptr) || index >= kExportCount || !g_exports[index]) {
        OutputDebugStringA("CTE FPS proxy could not forward a required system WINMM export\n");
        ExitProcess(ERROR_PROC_NOT_FOUND);
    }
    return g_exports[index];
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    // No file I/O, module loading, waiting, or patching under the loader lock.
    if (reason == DLL_PROCESS_ATTACH) selfModule = module;
    else if (reason == DLL_PROCESS_DETACH) InterlockedExchange(&running, 0);
    return TRUE;
}
