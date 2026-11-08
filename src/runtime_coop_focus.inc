// Dead Space 2 United by Richard Hafer.
// Frame rate with and without window focus ([Coop] BackgroundFullSpeed).
//
// The game itself does not slow down in the background: on WM_ACTIVATEAPP it
// only mutes its audio (0x0044A5B0), releases the cursor clip and minimizes in
// fullscreen; its loop has no focus check. 25.09., two instances on one PC: a
// minimized instance still ran at RTSS's 30 FPS cap. What Windows 11 does to a
// muted, occluded or minimized process is its own: timer resolution requests
// are ignored (Sleep(1) takes a whole 15.6 ms tick, which breaks sleeping frame
// limiters like RTSS's passive wait), and power throttling may lower its
// execution speed. The switch opts this process out of both.
//
// The diagnostics show frames per second split by focus state and how long a
// Sleep(1) really takes in each, so one test tells which side is at fault.
namespace camtools { bool PadTaken(); }   // runtime_coop_camtools.inc: the trailer camera flies with the pad
namespace focusrate {
bool enabled = false;
constexpr uint32_t kFrameCounterRva = 0x1C4D410, kWindowRva = 0x1C21F64;
enum State { kFocused, kBackground, kMinimized, kStates };
const char* const kStateNames[kStates] = {"focused", "background", "minimized"};
struct Bucket { uint32_t frames; double seconds, sleepMs; unsigned sleepSamples; };
Bucket buckets[kStates]{};
uint32_t lastFrames = 0;
LARGE_INTEGER lastAt{}, frequency{};
DWORD lastReport = 0;
unsigned tick = 0;

// Thread pinning ([Coop] UnpinThreads, default = BackgroundFullSpeed).
// The game pins threads to one core: SetThreadAffinityMask(GetCurrentThread(),
// 1 << [0x0205B260]) at 0x0081CD40 and 1 << [0x0205B264] at 0x0081C740, both 0
// on this PC (dumps 9608, 42844). Two copies on one PC then share core 0 and the
// unfocused one starves (09.10.: 1 to 40 fps in the background, 140 in the
// menu). With the switch every such call gets the whole process mask instead.
constexpr uint32_t kAffinitySlot = 0x1D58C88;
using SetAffinityFn = DWORD_PTR(WINAPI*)(HANDLE, DWORD_PTR);
SetAffinityFn realSetAffinity = nullptr;
bool unpin = false;
volatile LONG unpinCalls = 0, unpinSwept = 0, unpinSweepFailed = 0;
DWORD_PTR lastAskedMask = 0;
DWORD_PTR ProcessMask() {
    DWORD_PTR process = 0, system = 0;
    return GetProcessAffinityMask(GetCurrentProcess(), &process, &system) ? process : 0;
}
DWORD_PTR WINAPI SetAffinityAll(HANDLE thread, DWORD_PTR asked) {
    InterlockedIncrement(&unpinCalls);
    lastAskedMask = asked;
    const DWORD_PTR all = ProcessMask();
    return realSetAffinity(thread, all ? all : asked);
}
// NotYet while the packer has not filled the slot; true once it is ours.
bool RedirectAffinity() {
    uint32_t current = 0;
    if (!Read(base + kAffinitySlot, &current, 4) || !current) return false;
    if (current == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(SetAffinityAll))) return true;
    const auto expected = reinterpret_cast<SetAffinityFn>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadAffinityMask"));
    if (!expected || current != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(expected))) return false;
    realSetAffinity = expected;
    void* slot = reinterpret_cast<void*>(base + kAffinitySlot);
    DWORD previous = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &previous)) return false;
    *reinterpret_cast<void**>(slot) = reinterpret_cast<void*>(SetAffinityAll);
    VirtualProtect(slot, 4, previous, &previous);
    return true;
}
// From the plugin's start thread, before the game creates its threads.
void EarlyUnpin(FILE* log, const std::string& ini) {
    const int fullSpeed = GetPrivateProfileIntA("Coop", "BackgroundFullSpeed", 0, ini.c_str());
    unpin = GetPrivateProfileIntA("Coop", "UnpinThreads", fullSpeed, ini.c_str()) != 0;
    if (!unpin) return;
    const DWORD started = GetTickCount();
    while (!RedirectAffinity()) {
        if (GetTickCount() - started >= 30000) {
            std::fprintf(log, "coop_focus unpin: the SetThreadAffinityMask import never turned real in 30 s; only the sweep at install\n");
            std::fflush(log);
            return;
        }
        Sleep(2);
    }
    std::fprintf(log, "coop_focus unpin: SetThreadAffinityMask import redirected after %lu ms (every pin gets the whole process mask)\n",
                 static_cast<unsigned long>(GetTickCount() - started));
    std::fflush(log);
}
// Threads pinned before the redirect: back to the whole process mask.
void SweepThreads(FILE* log) {
    const DWORD_PTR all = ProcessMask();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!all || snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
        if (entry.th32OwnerProcessID != self) continue;
        const HANDLE thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        const DWORD_PTR before = thread ? (realSetAffinity ? realSetAffinity : SetThreadAffinityMask)(thread, all) : 0;
        if (thread) CloseHandle(thread);
        if (!before) InterlockedIncrement(&unpinSweepFailed);
        else if (before != all) InterlockedIncrement(&unpinSwept);
    }
    CloseHandle(snap);
    std::fprintf(log, "coop_focus unpin: %ld pinned thread(s) set back to the process mask %08lX, %ld failed; pins seen so far %ld\n",
                 InterlockedCompareExchange(&unpinSwept, 0, 0), static_cast<unsigned long>(all), InterlockedCompareExchange(&unpinSweepFailed, 0, 0),
                 InterlockedCompareExchange(&unpinCalls, 0, 0));
}

// Input with the window in the background ([Coop] FocusInputOnly, default =
// BackgroundFullSpeed). The game polls the keyboard with GetAsyncKeyState and
// GetKeyState, the pad with XInputGetState and the cursor with
// Get/SetPhysicalCursorPos, none of which care which window has the focus, and
// the loader's raw mouse look feeds every copy. Two copies on one PC then both
// act on every key (09.10.: the client's reload reloaded the host's Isaac, once
// UnpinThreads let the background copy run at full speed). The game's own import
// slots answer "nothing" while another window has the focus; the mouse look is
// gated in bot_aim::Dispatch.
bool focusInputOnly = false;
volatile LONG keysMuted = 0, padsMuted = 0, cursorMuted = 0, aimMuted = 0;
DWORD focusCheckedAt = 0;
bool focusCached = true;
bool InputFocused() {
    if (!focusInputOnly) return true;
    const DWORD now = GetTickCount();
    if (now - focusCheckedAt >= 50) {
        focusCheckedAt = now;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        focusCached = pid == GetCurrentProcessId();
    }
    return focusCached;
}
// The supply terminal and the debug menu own keyboard and mouse while open
// (10.10.: driven with WASD, Space and the mouse instead of the numpad). The
// slots below then answer as for a background window, the cursor is no longer
// pulled back to the centre (the panel's pointer follows it) and bot_aim stops
// the loader's mouse look. Bit 0 the shop, bit 1 the debug menu.
volatile LONG panelHolds = 0;
bool panelGate = false;   // the key, cursor and pad slots are ours
// Keys the game never sees while the panels are installed (their open keys), and
// keys still held when a panel closed, until they are let go (the Space that
// bought, the B that closed). Game thread.
bool panelKey[256]{};
bool swallowed[256]{};
volatile LONG swallowedKeys = 0;
bool GameSees() { return !InterlockedCompareExchange(&panelHolds, 0, 0) && InputFocused(); }
using KeyFn = SHORT(WINAPI*)(int);
using PadFn = DWORD(WINAPI*)(DWORD, void*);
using GetCursorFn = BOOL(WINAPI*)(LPPOINT);
using SetCursorFn = BOOL(WINAPI*)(int, int);
using ClipFn = BOOL(WINAPI*)(const RECT*);
KeyFn realAsyncKey = nullptr, realKeyState = nullptr;
PadFn realPad = nullptr;
GetCursorFn realGetCursor = nullptr;
SetCursorFn realSetCursor = nullptr;
ClipFn realClip = nullptr;
POINT lastCursorSet{};
bool cursorSetSeen = false;
bool Hidden(int vk) {
    if (vk <= 0 || vk > 255) return false;
    if (panelKey[vk]) return true;
    if (!swallowed[vk]) return false;
    if (realAsyncKey && (realAsyncKey(vk) & 0x8000)) return true;
    swallowed[vk] = false;
    return false;
}
SHORT WINAPI GateAsyncKey(int vk) {
    if (GameSees() && !Hidden(vk)) return realAsyncKey(vk);
    InterlockedIncrement(&keysMuted);
    return 0;
}
SHORT WINAPI GateKeyState(int vk) {
    if (GameSees() && !Hidden(vk)) return realKeyState(vk);
    InterlockedIncrement(&keysMuted);
    return 0;
}
// Still connected, so the game keeps its pad; only the buttons and sticks go quiet.
DWORD WINAPI GatePad(DWORD user, void* state) {
    const DWORD result = realPad(user, state);
    if (result == ERROR_SUCCESS && state && (!GameSees() || camtools::PadTaken())) {
        std::memset(static_cast<char*>(state) + 4, 0, 12);   // XINPUT_STATE.Gamepad
        InterlockedIncrement(&padsMuted);
    }
    return result;
}
// The game measures the mouse from where it last put the cursor: in the
// background, or behind an open panel, it is told the cursor is still there,
// and moving it is left out.
BOOL WINAPI GateGetCursor(LPPOINT point) {
    if (!GameSees() && cursorSetSeen && point) { *point = lastCursorSet; return TRUE; }
    return realGetCursor(point);
}
BOOL WINAPI GateSetCursor(int x, int y) {
    lastCursorSet = POINT{x, y};
    cursorSetSeen = true;
    if (GameSees()) return realSetCursor(x, y);
    InterlockedIncrement(&cursorMuted);
    return TRUE;
}
// A panel opens or closes (bit 0 the shop, bit 1 the debug menu). On the close
// the cursor goes back where the game left it, or the way the pointer travelled
// would turn the camera; held keys stay away from the game until they are let go.
void PanelHolds(LONG bit, bool on) {
    const LONG before = on ? InterlockedOr(&panelHolds, bit) : InterlockedAnd(&panelHolds, ~bit);
    if (on || !(before & bit) || InterlockedCompareExchange(&panelHolds, 0, 0)) return;
    if (realSetCursor && cursorSetSeen) realSetCursor(lastCursorSet.x, lastCursorSet.y);
    for (int vk = 1; vk < 256; ++vk)
        if (realAsyncKey && (realAsyncKey(vk) & 0x8000)) { swallowed[vk] = true; InterlockedIncrement(&swallowedKeys); }
}
void HideKeyFromGame(int vk) { if (vk > 0 && vk < 256) panelKey[vk] = true; }
BOOL WINAPI GateClip(const RECT* rect) {
    if (rect && !InputFocused()) return TRUE;
    return realClip(rect);
}
// One of the game's import slots: taken when it holds the export itself.
bool RedirectSlot(FILE* log, uint32_t rva, const char* dll, const char* name, void* stub, void** real) {
    uint32_t current = 0;
    const HMODULE module = GetModuleHandleA(dll);
    const auto expected = module ? reinterpret_cast<uint32_t>(GetProcAddress(module, name)) : 0u;
    if (!Read(base + rva, &current, 4) || !current || !expected || current != expected) {
        std::fprintf(log, "coop_focus input gate: %s slot %08X holds %08X, not %s!%s (%08X); left alone\n", name, static_cast<unsigned>(base + rva), current, dll,
                     name, expected);
        return false;
    }
    *real = reinterpret_cast<void*>(static_cast<uintptr_t>(expected));
    void* slot = reinterpret_cast<void*>(base + rva);
    DWORD previous = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &previous)) return false;
    *reinterpret_cast<void**>(slot) = stub;
    VirtualProtect(slot, 4, previous, &previous);
    return true;
}
// DirectInput: the game reads keyboard and mouse through dinput8 as well (the
// loader's DINPUT8.dll forwards to the system one), which the import slots never
// see (11.10.: Isaac walked and jumped with the supply terminal open).
// IDirectInputDevice8::GetDeviceState (slot 9) and GetDeviceData (slot 10) are
// shared by the system dinput8's devices; behind an open panel and in the
// background they hand out an empty keyboard or mouse state and no events.
struct DiGuid { uint32_t a; uint16_t b, c; uint8_t d[8]; };
constexpr DiGuid kIidDirectInput8A{0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr DiGuid kIidDirectInput8W{0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr DiGuid kGuidSysKeyboard{0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr DiGuid kGuidSysMouse{0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
safetyhook::InlineHook diStateHooks[2], diDataHooks[2];
volatile LONG diStateCalls = 0, diStateMuted = 0, diDataCalls = 0, diDataMuted = 0;
template <int N>
HRESULT __stdcall OnDiState(void* self, DWORD size, void* data) {
    const HRESULT hr = diStateHooks[N].stdcall<HRESULT>(self, size, data);
    InterlockedIncrement(&diStateCalls);
    // keyboard (256 bytes) and mouse (DIMOUSESTATE 16, DIMOUSESTATE2 20); a joystick's POV would read "up" at zero
    if (hr >= 0 && data && (size == 256 || size == 16 || size == 20) && !GameSees()) {
        std::memset(data, 0, size);
        InterlockedIncrement(&diStateMuted);
    }
    return hr;
}
template <int N>
HRESULT __stdcall OnDiData(void* self, DWORD objectSize, void* items, DWORD* count, DWORD flags) {
    const HRESULT hr = diDataHooks[N].stdcall<HRESULT>(self, objectSize, items, count, flags);
    InterlockedIncrement(&diDataCalls);
    if (hr >= 0 && items && count && *count && !GameSees()) {   // taken from the buffer and dropped
        *count = 0;
        InterlockedIncrement(&diDataMuted);
    }
    return hr;
}
void InstallDirectInputGate(FILE* log) {
    char path[MAX_PATH];
    const UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n > MAX_PATH - 16) return;
    strcat_s(path, "\\dinput8.dll");
    HMODULE di = LoadLibraryA(path);
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, const DiGuid*, void**, void*);
    const auto create = di ? reinterpret_cast<Create>(GetProcAddress(di, "DirectInput8Create")) : nullptr;
    if (!create) { std::fprintf(log, "coop_focus directinput gate: no system dinput8 (%s)\n", path); return; }
    void* stateFns[4]{};
    void* dataFns[4]{};
    unsigned states = 0, datas = 0;
    const DiGuid* interfaces[2] = {&kIidDirectInput8A, &kIidDirectInput8W};
    const DiGuid* devicesWanted[2] = {&kGuidSysKeyboard, &kGuidSysMouse};
    for (const DiGuid* iid : interfaces) {
        void* dinput = nullptr;
        if (create(GetModuleHandleA(nullptr), 0x0800, iid, &dinput, nullptr) < 0 || !dinput) continue;
        void** vt = *reinterpret_cast<void***>(dinput);
        using CreateDevice = HRESULT(__stdcall*)(void*, const DiGuid*, void**, void*);
        using Release = ULONG(__stdcall*)(void*);
        for (const DiGuid* guid : devicesWanted) {
            void* device = nullptr;
            if (reinterpret_cast<CreateDevice>(vt[3])(dinput, guid, &device, nullptr) < 0 || !device) continue;
            void** dvt = *reinterpret_cast<void***>(device);
            void* fns[2] = {dvt[9], dvt[10]};
            bool known = false;
            for (unsigned i = 0; i < states; ++i) known = known || stateFns[i] == fns[0];
            if (!known && states < 4) stateFns[states++] = fns[0];
            known = false;
            for (unsigned i = 0; i < datas; ++i) known = known || dataFns[i] == fns[1];
            if (!known && datas < 4) dataFns[datas++] = fns[1];
            reinterpret_cast<Release>(dvt[2])(device);
        }
        reinterpret_cast<Release>(vt[2])(dinput);
    }
    void* stateHandlers[2] = {reinterpret_cast<void*>(OnDiState<0>), reinterpret_cast<void*>(OnDiState<1>)};
    void* dataHandlers[2] = {reinterpret_cast<void*>(OnDiData<0>), reinterpret_cast<void*>(OnDiData<1>)};
    unsigned stateHooked = 0, dataHooked = 0;
    for (unsigned i = 0; i < states && i < 2; ++i) {
        auto result = safetyhook::InlineHook::create(stateFns[i], stateHandlers[i]);
        if (result) { diStateHooks[i] = std::move(*result); ++stateHooked; }
    }
    for (unsigned i = 0; i < datas && i < 2; ++i) {
        auto result = safetyhook::InlineHook::create(dataFns[i], dataHandlers[i]);
        if (result) { diDataHooks[i] = std::move(*result); ++dataHooked; }
    }
    std::fprintf(log, "coop_focus directinput gate: %s, GetDeviceState %u/%u hooked (%p %p), GetDeviceData %u/%u hooked (%p %p)\n", path, stateHooked, states,
                 stateFns[0], stateFns[1], dataHooked, datas, dataFns[0], dataFns[1]);
}

void InstallInputGate(FILE* log, const std::string& ini, bool fullSpeed) {
    focusInputOnly = GetPrivateProfileIntA("Coop", "FocusInputOnly", fullSpeed ? 1 : 0, ini.c_str()) != 0;
    // The supply terminal and the debug menu take the input through the same slots.
    const bool panels = GetPrivateProfileIntA("Shop", "Enabled", 0, ini.c_str()) != 0 || GetPrivateProfileIntA("Debug", "Enabled", 1, ini.c_str()) != 0;
    if (!focusInputOnly && !panels) {
        std::fprintf(log, "coop_focus input gate off (FocusInputOnly=0, no panel): the copy in the background reads input too\n");
        return;
    }
    const bool keys = RedirectSlot(log, 0x1D58EE4, "user32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(GateAsyncKey), reinterpret_cast<void**>(&realAsyncKey));
    const bool keyState = RedirectSlot(log, 0x1D58ECC, "user32.dll", "GetKeyState", reinterpret_cast<void*>(GateKeyState), reinterpret_cast<void**>(&realKeyState));
    const bool pad = RedirectSlot(log, 0x1D59184, "xinput1_3.dll", "XInputGetState", reinterpret_cast<void*>(GatePad), reinterpret_cast<void**>(&realPad));
    const bool getCursor = RedirectSlot(log, 0x1D58EBC, "user32.dll", "GetPhysicalCursorPos", reinterpret_cast<void*>(GateGetCursor), reinterpret_cast<void**>(&realGetCursor));
    const bool setCursor = RedirectSlot(log, 0x1D58EFC, "user32.dll", "SetPhysicalCursorPos", reinterpret_cast<void*>(GateSetCursor), reinterpret_cast<void**>(&realSetCursor));
    const bool clip = RedirectSlot(log, 0x1D58EE0, "user32.dll", "ClipCursor", reinterpret_cast<void*>(GateClip), reinterpret_cast<void**>(&realClip));
    panelGate = keys && keyState && getCursor && setCursor;
    InstallDirectInputGate(log);
    std::fprintf(log, "coop_focus input gate: async_key=%d key_state=%d xinput=%d get_cursor=%d set_cursor=%d clip=%d (%s; behind an open panel too: %s; mouse look in bot_aim)\n",
                 keys ? 1 : 0, keyState ? 1 : 0, pad ? 1 : 0, getCursor ? 1 : 0, setCursor ? 1 : 0, clip ? 1 : 0,
                 focusInputOnly ? "in the background the game sees no input" : "the background copy reads input (FocusInputOnly=0)",
                 panelGate ? "yes" : "NO, keys or cursor slot not taken");
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "BackgroundFullSpeed", 0, ini.c_str()) != 0;
    QueryPerformanceFrequency(&frequency);
    if (unpin) SweepThreads(log);
    InstallInputGate(log, ini, enabled);
    if (!enabled) {
        std::fprintf(log, "coop_focus background_full_speed=0 (diagnostics only)\n");
        return false;
    }
    // PROCESS_POWER_THROTTLING_EXECUTION_SPEED | _IGNORE_TIMER_RESOLUTION in the
    // control mask with a clear state mask: never throttled, timer requests
    // always honored. Loaded by name so older SDK headers do not matter.
    struct ThrottlingState { ULONG version, controlMask, stateMask; } state{1, 0x1 | 0x4, 0};
    using SetInfo = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    BOOL throttling = FALSE;
    DWORD error = 0;
    if (auto set = reinterpret_cast<SetInfo>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation"))) {
        throttling = set(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &state, sizeof(state));
        if (!throttling) error = GetLastError();
    }
    unsigned period = 0;
    if (HMODULE winmm = LoadLibraryA("winmm.dll")) {
        using BeginPeriod = UINT(WINAPI*)(UINT);
        if (auto begin = reinterpret_cast<BeginPeriod>(GetProcAddress(winmm, "timeBeginPeriod"))) period = begin(1) == 0 ? 1 : 0;
    }
    std::fprintf(log, "coop_focus background_full_speed=1 power_throttling_off=%d (error %lu) timer_1ms=%u\n", throttling ? 1 : 0, error, period);
    return true;
}

State Current() {
    uint32_t window = 0;
    Read(base + kWindowRva, &window, 4);
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(window));
    if (hwnd && IsIconic(hwnd)) return kMinimized;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() ? kFocused : kBackground;
}

// Called once a second from the diagnostics thread, never from the game's.
void WriteDiagnostics(FILE* log) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    uint32_t frames = 0;
    if (!Read(base + kFrameCounterRva, &frames, 4) || !frequency.QuadPart) return;
    const State s = Current();
    if (lastAt.QuadPart) {
        Bucket& b = buckets[s];
        b.frames += frames - lastFrames;
        b.seconds += static_cast<double>(now.QuadPart - lastAt.QuadPart) / frequency.QuadPart;
        if (++tick % 5 == 0) {
            LARGE_INTEGER a{}, c{};
            QueryPerformanceCounter(&a);
            Sleep(1);
            QueryPerformanceCounter(&c);
            b.sleepMs += 1000.0 * (c.QuadPart - a.QuadPart) / frequency.QuadPart;
            ++b.sleepSamples;
        }
    }
    lastFrames = frames;
    lastAt = now;
    const DWORD ms = GetTickCount();
    if (ms - lastReport < 30000) return;
    lastReport = ms;
    bool any = false;
    for (const auto& b : buckets) any |= b.seconds > 0.5;
    if (!any) return;
    std::fprintf(log, "coop_focus");
    for (unsigned i = 0; i < kStates; ++i) {
        const Bucket& b = buckets[i];
        if (b.seconds <= 0.5) continue;
        std::fprintf(log, " %s: %.1f fps over %.0f s, sleep1=%.1f ms;", kStateNames[i], b.frames / b.seconds, b.seconds,
                     b.sleepSamples ? b.sleepMs / b.sleepSamples : 0.0);
    }
    std::fprintf(log, " full_speed=%d unpin=%d pins=%ld (last asked %08lX) input_gate=%d panel_gate=%d panel_holds=%ld muted keys=%ld pads=%ld cursor=%ld aim=%ld "
                      "swallowed=%ld directinput state=%ld/%ld muted data=%ld/%ld muted\n",
                 enabled ? 1 : 0, unpin ? 1 : 0, InterlockedCompareExchange(&unpinCalls, 0, 0), static_cast<unsigned long>(lastAskedMask), focusInputOnly ? 1 : 0,
                 panelGate ? 1 : 0, InterlockedCompareExchange(&panelHolds, 0, 0), InterlockedCompareExchange(&keysMuted, 0, 0),
                 InterlockedCompareExchange(&padsMuted, 0, 0), InterlockedCompareExchange(&cursorMuted, 0, 0), InterlockedCompareExchange(&aimMuted, 0, 0),
                 InterlockedCompareExchange(&swallowedKeys, 0, 0), InterlockedCompareExchange(&diStateMuted, 0, 0), InterlockedCompareExchange(&diStateCalls, 0, 0),
                 InterlockedCompareExchange(&diDataMuted, 0, 0), InterlockedCompareExchange(&diDataCalls, 0, 0));
    for (auto& b : buckets) b = Bucket{};
}
}  // namespace focusrate
