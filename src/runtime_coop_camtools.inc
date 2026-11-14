// Dead Space 2 United by Richard Hafer.
// Trailer camera ([CameraTools], off by default; analysis/DS2_TRAILER_CAMERA_2026-10-16.md):
// a free camera flown with the gamepad while Isaac answers keyboard and mouse, slow
// motion, and a camera path that is recorded and replayed.
// - Render only: RWS::MainLoop::Render::Poll 0x00770C00 (cdecl(int), plain ret) is
//   wrapped. Inside the wrapper blender 0 of the CameraManager ([0x0202131C]+0x10) holds
//   the trailer pose and SetRenderCamera 0x00793470 (thiscall(vp, pos*, rot*, fov, near,
//   far), ret 0x18, pos 16-byte aligned) gets it, so occlusion, culling and LOD follow
//   it; afterwards both get the gameplay camera back. CombineBlends keeps running, so
//   aim, laser, flashlight and movement stay on the gameplay camera.
// - The pad: the game's XInputGetState slot (focusrate::GatePad) answers an idle pad
//   while the camera flies; the plugin reads the real one.
// - Slow motion: the game speed float 0x01EC51F0 (read only by the frame timer, at
//   0x0076FA14), never below 0.01 (unguarded divisions by the sim dt).
// - Flying and the path run on the real clock, so slow motion does not slow them.
namespace camtools {
using coop_camtools::Pose;
bool enabled = false, installed = false, speedOk = false;
int freeKey = VK_F6, slowKey = VK_F5, freezeKey = VK_F4, recordKey = VK_F2, playKey = VK_F3, hideKey = VK_F10;
// Stepping and steering keys (0 = unbound). Insert is the debug menu's, the numpad the bots'.
int speedUpKey = VK_PRIOR, speedDownKey = VK_NEXT, timeFasterKey = VK_HOME, timeSlowerKey = VK_END;
int rollLeftKey = VK_OEM_COMMA, rollRightKey = VK_OEM_PERIOD, zoomInKey = VK_OEM_PLUS, zoomOutKey = VK_OEM_MINUS, resetKey = 0;
int deadZone = 7849;
float slowFactor = 0.25f;
// The pad: one binding per action, read from [CameraTools] Pad<Action>.
enum PadAction { kPadFreeCam, kPadSlow, kPadFreeze, kPadRecord, kPadPlay, kPadReset, kPadRollLeft, kPadRollRight, kPadZoomIn, kPadZoomOut,
                 kPadSpeedUp, kPadSpeedDown, kPadTimeFaster, kPadTimeSlower, kPadActions };
struct PadActionInfo { const char* key; const char* fallback; };
const PadActionInfo kPadActionInfo[kPadActions] = {
    {"PadFreeCam", "L3+R3"}, {"PadSlow", "A"}, {"PadFreeze", "Back"}, {"PadRecord", "X"}, {"PadPlay", "Y"}, {"PadReset", "B"},
    {"PadRollLeft", "LB"}, {"PadRollRight", "RB"}, {"PadZoomIn", "Start+RB"}, {"PadZoomOut", "Start+LB"},
    {"PadSpeedUp", "Up"}, {"PadSpeedDown", "Down"}, {"PadTimeFaster", "Right"}, {"PadTimeSlower", "Left"}};
coop_camtools::PadBind padBind[kPadActions];
bool padHeldBefore[kPadActions]{};
// 0: no notes, 1: a small note in the corner (never during playback), 2: the big banners.
int notices = 1;
bool hideHudInFreeCam = true, loopPath = false;
coop_camtools::Tuning tuning;
std::string pathFolder;

constexpr uint32_t kRenderPollRva = 0x370C00, kSetRenderCameraRva = 0x393470, kCameraManager = 0x1C2131C, kScreenAspect = 0x1D1D15C;
constexpr uint32_t kSpeedReadRva = 0x36FA14, kSpeedRva = 0x1AC51F0;
// No engine draw is skipped to hide the HUD (16.10.). 0x007706F0 is not only the 2D overlay:
// Render::Poll calls it for the 3D world pass (0x00770DA8, argument 0: layer 0x1F, then
// layers 0..0x1D) and for the full-screen 2D pass (0x00770DF7, argument 1: layers 0..0x1D).
// The old patches on its display list draws (0x0077073B, 0x00770756) blanked the world (the
// black free camera), and a 2D pass that never ran left Apt sequences that later called a dead
// pointer (crash 16.10. 13:56 in AptNewSequence::Run, as on 15.10. at the wave notice).
// push ebp / mov ebp,esp / and esp,-16 / sub esp,0x24 / push ebx / push esi / push edi
const unsigned char kRenderPollBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x24, 0x53, 0x56, 0x57};
const unsigned char kSpeedReadBytes[] = {0xF3, 0x0F, 0x10, 0x05, 0xF0, 0x51, 0xEC, 0x01};   // movss xmm0,[0x01EC51F0]
constexpr float kMinSpeed = 0.01f;
safetyhook::InlineHook pollHook;

// --- state (game thread: Render::Poll) ---
bool freeCam = false, recording = false, playing = false, slow = false, frozen = false, hudHidden = false, hudPatched = false, speedTouched = false;
Pose pose;
coop_camtools::Flight flight;
float speedMul = 1.0f, gameFov = 1.2217305f, timeScale = 0.25f;
coop_camtools::Path path;
LARGE_INTEGER frequency{};
LONGLONG lastTick = 0, recordStart = 0, playStart = 0, lastLog = 0;
bool keyDown[256]{};
volatile LONG padTaken = 0;
volatile LONG polls = 0, swaps = 0, skipsNoManager = 0, faults = 0, padReads = 0, padFails = 0, speedWrites = 0, hudToggles = 0;
volatile LONG freeCamToggles = 0, recordings = 0, plays = 0, samples = 0;

bool PadTaken() { return InterlockedCompareExchange(&padTaken, 0, 0) != 0; }
double Seconds(LONGLONG from, LONGLONG to) { return frequency.QuadPart ? static_cast<double>(to - from) / static_cast<double>(frequency.QuadPart) : 0.0; }
// Notes stay out of a playing path, which is what gets filmed.
void Say(const char* title, const char* line) {
    if (notices <= 0 || playing) return;
    if (notices >= 2) { overlay::ShowBanner(title, line, 1500); return; }
    char text[64];
    std::snprintf(text, sizeof(text), "%s%s%s", title, line && line[0] ? "  " : "", line ? line : "");
    overlay::ShowNote(text, 900);
}
coop_camtools::Repeater speedUpRep, speedDownRep, timeFasterRep, timeSlowerRep;

// --- the game's camera ---
uint32_t Blender0(uint32_t& manager) {
    manager = 0;
    uint32_t count = 0, blenders = 0, context = 0;
    if (!Read(base + kCameraManager, &manager, 4) || !manager || !Read(manager + 0x164, &count, 4) || !count || count > 4 || !Read(manager + 0x10, &blenders, 4) ||
        !Read(manager + 0x14, &context, 4) || !context)
        return 0;
    return blenders;
}
bool ReadPose(uint32_t b, Pose& out) {
    float pos[3]{}, angles[3]{}, fov = 0.0f;
    if (!Read(b, pos, 12) || !Read(b + 0x20, angles, 12) || !Read(b + 0x30, &fov, 4)) return false;
    for (float v : pos) if (!std::isfinite(v)) return false;
    std::memcpy(out.pos, pos, 12);
    out.pitch = angles[0];
    out.yaw = angles[1];
    out.roll = angles[2];
    out.fov = fov > 0.05f && fov < 3.0f ? fov : 1.2217305f;
    return true;
}
__declspec(noinline) bool CopyBlender(void* to, const void* from) {
    __try {
        std::memcpy(to, from, 0x90);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
// The pose into blender 0: position, angles, FOV and the cached camera-to-world matrix.
void WritePose(uint8_t* b, const Pose& p) {
    const float pos[4] = {p.pos[0], p.pos[1], p.pos[2], 1.0f};
    const float angles[3] = {p.pitch, p.yaw, p.roll};
    float matrix[16];
    coop_camtools::CameraToWorld(p, matrix);
    std::memcpy(b, pos, sizeof(pos));
    std::memcpy(b + 0x20, angles, sizeof(angles));
    std::memcpy(b + 0x30, &p.fov, 4);
    std::memcpy(b + 0x50, matrix, sizeof(matrix));
}
// SetRenderCamera for viewport 0 from a blender, the way CameraManager::Update calls it.
__declspec(noinline) bool ApplyRender(uint32_t manager, const uint8_t* b) {
    __try {
        alignas(16) float pos[4], rot[4];
        std::memcpy(pos, b, 16);
        std::memcpy(rot, b + 0x20, 16);
        float fov = 0.0f, nearPlane = 0.0f, farPlane = 0.0f, aspect = 0.0f;
        int authored = 0, conversion = 0;
        std::memcpy(&fov, b + 0x30, 4);
        std::memcpy(&nearPlane, b + 0x34, 4);
        std::memcpy(&farPlane, b + 0x38, 4);
        std::memcpy(&authored, b + 0x3C, 4);
        std::memcpy(&conversion, b + 0x40, 4);
        std::memcpy(&aspect, reinterpret_cast<const void*>(base + kScreenAspect), 4);
        const float shown = coop_camtools::RenderFov(fov, authored, conversion, aspect);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, const float*, const float*, float, float, float)>(base + kSetRenderCameraRva)(
            reinterpret_cast<void*>(static_cast<uintptr_t>(manager)), 0u, pos, rot, shown, nearPlane, farPlane);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}

// --- input ---
#pragma pack(push, 1)
struct PadState { DWORD packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
#pragma pack(pop)
static_assert(sizeof(PadState) == 16, "XINPUT_STATE");
focusrate::PadFn padFn = nullptr;
int padUser = -1;            // the XInput user that answered last
DWORD padSearchAt = 0, padError = 0;
bool padReported = false;
bool ReadPad(coop_camtools::Pad& out) {
    PadState s{};
    if (!padFn) return false;
    DWORD result = padUser >= 0 ? padFn(static_cast<DWORD>(padUser), &s) : ERROR_DEVICE_NOT_CONNECTED;
    // another user index once a second while none answers (15.10.: user 0 never did)
    if (result != ERROR_SUCCESS && GetTickCount() - padSearchAt >= 1000) {
        padSearchAt = GetTickCount();
        const int before = padUser;
        padUser = -1;
        for (DWORD user = 0; user < 4 && padUser < 0; ++user) {
            result = padFn(user, &s);
            if (result == ERROR_SUCCESS) padUser = static_cast<int>(user);
            else padError = result;
        }
        if (padUser != before || !padReported) {
            padReported = true;
            gate::Event("camtools: %s", padUser >= 0 ? "an XInput pad answers" : "no XInput pad answers on users 0..3");
            if (padUser >= 0) gate::Event("camtools: pad on XInput user %d", padUser);
            else gate::Event("camtools: last XInputGetState error %lu (1167 = not connected; a non-XInput pad needs Steam Input or a wrapper)", padError);
        }
    }
    if (result != ERROR_SUCCESS) { InterlockedIncrement(&padFails); return false; }
    InterlockedIncrement(&padReads);
    out.lx = coop_camtools::Stick(s.lx, deadZone);
    out.ly = coop_camtools::Stick(s.ly, deadZone);
    out.rx = coop_camtools::Stick(s.rx, deadZone);
    out.ry = coop_camtools::Stick(s.ry, deadZone);
    out.lt = coop_camtools::Trigger(s.lt, 30);
    out.rt = coop_camtools::Trigger(s.rt, 30);
    out.buttons = s.buttons;
    return true;
}
bool Pressed(int vk, bool focused) {
    if (vk <= 0 || vk > 255) return false;
    const bool down = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !keyDown[vk];
    keyDown[vk] = down;
    return edge;
}
bool KeyHeld(int vk, bool focused) { return vk > 0 && vk <= 255 && focused && (GetAsyncKeyState(vk) & 0x8000) != 0; }
// Pad actions this frame: held (with the modifier rule) and newly pressed.
bool padHeld[kPadActions]{};
void ReadPadActions(bool havePad, uint16_t buttons) {
    for (int i = 0; i < kPadActions; ++i) {
        padHeldBefore[i] = padHeld[i];
        padHeld[i] = havePad && coop_camtools::HeldAlone(padBind[i], buttons, padBind, kPadActions);
    }
}
bool PadPressed(PadAction a) { return padHeld[a] && !padHeldBefore[a]; }

// --- the HUD and the speed ---
// Only our own overlay hides; the game's HUD stays (see the draws above).
void SetHud(bool hide) {
    if (hide == hudPatched) return;
    hudPatched = hide;
    InterlockedExchange(&overlay::cameraHidden, hide ? 1 : 0);
    InterlockedIncrement(&hudToggles);
    gate::Event("camtools: our overlay %s (the game's HUD stays: no engine draw is skipped)", hide ? "hidden" : "shown");
}
void SetSpeed(float want) {
    if (!speedOk) return;
    if (want < kMinSpeed) want = kMinSpeed;
    float now = 1.0f;
    Read(base + kSpeedRva, &now, 4);
    if (now == want) return;
    if (WriteBytes(base + kSpeedRva, &want, 4)) InterlockedIncrement(&speedWrites);
}

// --- the path ---
void SavePath() {
    if (path.samples.empty()) return;
    CreateDirectoryA(pathFolder.c_str(), nullptr);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char name[MAX_PATH];
    std::snprintf(name, sizeof(name), "%s\\path_%04u%02u%02u_%02u%02u%02u.txt", pathFolder.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::string text = coop_camtools::Save(path);
    for (const std::string& file : {std::string(name), pathFolder + "\\last.txt"}) {
        FILE* f = nullptr;
        if (fopen_s(&f, file.c_str(), "wb") == 0 && f) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        } else {
            gate::Event("camtools: path NOT written to %s", file.c_str());
        }
    }
    gate::Event("camtools: path of %.1f s, %zu samples saved to %s and last.txt", path.Duration(), path.samples.size(), name);
}
bool LoadLastPath() {
    std::ifstream file(pathFolder + "\\last.txt", std::ios::binary);
    if (!file) return false;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    unsigned skipped = 0;
    path = coop_camtools::Load(text, skipped);
    gate::Event("camtools: last.txt loaded: %zu samples, %.1f s, %u lines skipped", path.samples.size(), path.Duration(), skipped);
    return !path.samples.empty();
}

// --- one rendered frame ---
void StopAll(const char* why) {
    if (recording) { recording = false; SavePath(); }
    if (freeCam || playing || slow || frozen) gate::Event("camtools: off (%s)", why);
    freeCam = playing = slow = frozen = false;
    InterlockedExchange(&padTaken, 0);
    if (speedTouched) { SetSpeed(1.0f); speedTouched = false; }
    SetHud(false);
}
void EnterFreeCam(uint32_t blender) {
    Pose start;
    if (blender && ReadPose(blender, start)) pose = start;
    gameFov = pose.fov;
    flight = coop_camtools::Flight{};
    freeCam = true;
    InterlockedIncrement(&freeCamToggles);
    gate::Event("camtools: free camera ON at (%.2f %.2f %.2f) pitch %.2f yaw %.2f fov %.2f", pose.pos[0], pose.pos[1], pose.pos[2], pose.pitch, pose.yaw, pose.fov);
}
void Frame(uint32_t blender, LONGLONG now) {
    const float dt = lastTick ? static_cast<float>(Seconds(lastTick, now)) : 0.0f;
    lastTick = now;
    if (!inLevel || !blender) { StopAll("no level"); return; }
    const bool focused = necro::GameWindowFocused();
    coop_camtools::Pad pad;
    const bool havePad = ReadPad(pad);
    ReadPadActions(havePad, havePad ? pad.buttons : 0);
    const bool flying = freeCam || playing;
    if (Pressed(freeKey, focused) || PadPressed(kPadFreeCam)) {
        if (freeCam || playing) {
            if (recording) { recording = false; SavePath(); }
            freeCam = playing = false;
            gate::Event("camtools: free camera OFF");
            Say("FREE CAMERA OFF", "");
        } else {
            EnterFreeCam(blender);
            Say("FREE CAMERA", "");
        }
    }
    if (Pressed(slowKey, focused) || (flying && PadPressed(kPadSlow))) {
        slow = !slow;
        char value[32];
        std::snprintf(value, sizeof(value), "%.2fx", timeScale);
        Say(slow ? "SLOW MOTION" : "NORMAL SPEED", slow ? value : "");
        gate::Event("camtools: slow motion %s (%.2f)", slow ? "on" : "off", timeScale);
    }
    // game speed in steps, never faster than normal: the keys any time, the pad while flying
    {
        const bool faster = coop_camtools::Repeat(timeFasterRep, KeyHeld(timeFasterKey, focused) || (flying && padHeld[kPadTimeFaster]), dt);
        const bool slower = coop_camtools::Repeat(timeSlowerRep, KeyHeld(timeSlowerKey, focused) || (flying && padHeld[kPadTimeSlower]), dt);
        if (faster != slower) {
            const float step = coop_camtools::TimeStep(slow ? timeScale : 1.0f, faster ? 1 : -1);
            slow = step < 1.0f;
            if (slow) timeScale = step;
            frozen = false;
            char value[32];
            std::snprintf(value, sizeof(value), "%.2fx", step);
            Say("GAME SPEED", slow ? value : "NORMAL");
            gate::Event("camtools: game speed %.2f", step);
        }
    }
    if (Pressed(freezeKey, focused) || (flying && PadPressed(kPadFreeze))) {
        frozen = !frozen;
        Say(frozen ? "TIME STOPPED" : "TIME RUNS", "");
        gate::Event("camtools: time %s", frozen ? "stopped (speed 0.01)" : "runs again");
    }
    if (Pressed(hideKey, focused)) {
        hudHidden = !hudHidden;
        gate::Event("camtools: HUD key: %s", hudHidden ? "hidden" : "shown");
    }
    if (Pressed(recordKey, focused) || (flying && !playing && PadPressed(kPadRecord))) {
        if (recording) {
            recording = false;
            SavePath();
            char line[64];
            std::snprintf(line, sizeof(line), "%.1f S SAVED", path.Duration());
            Say("PATH RECORDED", line);
        } else {
            if (playing) playing = false;
            if (!freeCam) EnterFreeCam(blender);
            path = coop_camtools::Path{};
            recording = true;
            recordStart = now;
            InterlockedIncrement(&recordings);
            gate::Event("camtools: recording a path");
            Say("RECORDING PATH", "");
        }
    }
    if (Pressed(playKey, focused) || (flying && PadPressed(kPadPlay))) {
        if (playing) {
            playing = false;
            gate::Event("camtools: path playback stopped");
            if (notices >= 2) Say("PLAYBACK STOPPED", "");   // stop and play stay silent with the small notes
        } else if (!recording && (!path.samples.empty() || LoadLastPath())) {
            playing = true;
            playStart = now;
            if (!freeCam) EnterFreeCam(blender);
            InterlockedIncrement(&plays);
            gate::Event("camtools: playing the path (%.1f s)", path.Duration());
        } else if (!recording) {
            Say("NO CAMERA PATH", "RECORD ONE FIRST");
        }
    }
    if (freeCam && !playing) {
        const bool up = coop_camtools::Repeat(speedUpRep, KeyHeld(speedUpKey, focused) || padHeld[kPadSpeedUp], dt);
        const bool down = coop_camtools::Repeat(speedDownRep, KeyHeld(speedDownKey, focused) || padHeld[kPadSpeedDown], dt);
        if (up != down) {
            speedMul = coop_camtools::SpeedStep(speedMul, up ? 1 : -1);
            char value[32];
            std::snprintf(value, sizeof(value), "%.2fx", speedMul);
            Say("CAMERA SPEED", value);
        }
        if (PadPressed(kPadReset) || Pressed(resetKey, focused)) { pose.roll = 0.0f; pose.fov = gameFov; Say("VIEW RESET", ""); }
        const float roll = ((padHeld[kPadRollRight] || KeyHeld(rollRightKey, focused)) ? 1.0f : 0.0f) - ((padHeld[kPadRollLeft] || KeyHeld(rollLeftKey, focused)) ? 1.0f : 0.0f);
        const float zoom = ((padHeld[kPadZoomIn] || KeyHeld(zoomInKey, focused)) ? 1.0f : 0.0f) - ((padHeld[kPadZoomOut] || KeyHeld(zoomOutKey, focused)) ? 1.0f : 0.0f);
        coop_camtools::Pad idle;
        coop_camtools::Fly(pose, flight, havePad ? pad : idle, speedMul, dt, tuning, roll, zoom);
    }
    if (playing) {
        const float t = static_cast<float>(Seconds(playStart, now));
        pose = coop_camtools::At(path, t);
        if (t > path.Duration()) {
            if (loopPath) playStart = now;
            else { playing = false; gate::Event("camtools: path playback done"); }
        }
    }
    if (recording && coop_camtools::Add(path, static_cast<float>(Seconds(recordStart, now)), pose)) InterlockedIncrement(&samples);
    InterlockedExchange(&padTaken, freeCam || playing ? 1 : 0);
    const bool timeChanged = slow || frozen;
    if (timeChanged) { SetSpeed(frozen ? kMinSpeed : timeScale); speedTouched = true; }
    else if (speedTouched) { SetSpeed(1.0f); speedTouched = false; }
    SetHud(hudHidden || (hideHudInFreeCam && (freeCam || playing)));
    if ((freeCam || playing) && Seconds(lastLog, now) >= 1.0) {
        lastLog = now;
        Pose game;
        ReadPose(blender, game);
        static unsigned logLines = 0;
        if (++logLines <= 120)
            gate::Event("camtools: trailer (%.1f %.1f %.1f p%.2f y%.2f r%.2f f%.2f) game (%.1f %.1f %.1f p%.2f y%.2f) speed x%.2f%s%s", pose.pos[0], pose.pos[1], pose.pos[2],
                        pose.pitch, pose.yaw, pose.roll, pose.fov, game.pos[0], game.pos[1], game.pos[2], game.pitch, game.yaw, speedMul, recording ? " REC" : "",
                        playing ? " PLAY" : "");
    }
}
__declspec(noinline) void FrameSafe(uint32_t blender, LONGLONG now) {
    __try {
        Frame(blender, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

void __cdecl OnRenderPoll(int arg) {
    InterlockedIncrement(&polls);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    uint32_t manager = 0;
    const uint32_t blender = Blender0(manager);
    FrameSafe(blender, now.QuadPart);
    if (!(freeCam || playing) || !blender) {
        if (!blender) InterlockedIncrement(&skipsNoManager);
        pollHook.ccall<void>(arg);
        return;
    }
    alignas(16) uint8_t saved[0x90];
    uint8_t* b = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(blender));
    if (!CopyBlender(saved, b)) { pollHook.ccall<void>(arg); return; }
    alignas(16) uint8_t mine[0x90];
    std::memcpy(mine, saved, sizeof(mine));
    WritePose(mine, pose);
    const bool swapped = CopyBlender(b, mine) && ApplyRender(manager, b);
    if (swapped) InterlockedIncrement(&swaps);
    pollHook.ccall<void>(arg);
    // the gameplay camera back for the next logic tick
    CopyBlender(b, saved);
    ApplyRender(manager, b);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("CameraTools", "Enabled", 0, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "camtools off ([CameraTools] Enabled=0)\n"); return; }
    auto key = [&](const char* name, int fallback) {
        const int v = GetPrivateProfileIntA("CameraTools", name, fallback, ini.c_str());
        return v >= 0 && v <= 255 ? v : 0;
    };
    freeKey = key("FreeCamKey", VK_F6);
    slowKey = key("SlowKey", VK_F5);
    freezeKey = key("FreezeKey", VK_F4);
    recordKey = key("RecordKey", VK_F2);
    playKey = key("PlayKey", VK_F3);
    hideKey = key("HideHudKey", VK_F10);
    speedUpKey = key("SpeedUpKey", VK_PRIOR);
    speedDownKey = key("SpeedDownKey", VK_NEXT);
    timeFasterKey = key("TimeFasterKey", VK_HOME);
    timeSlowerKey = key("TimeSlowerKey", VK_END);
    rollLeftKey = key("RollLeftKey", VK_OEM_COMMA);
    rollRightKey = key("RollRightKey", VK_OEM_PERIOD);
    zoomInKey = key("ZoomInKey", VK_OEM_PLUS);
    zoomOutKey = key("ZoomOutKey", VK_OEM_MINUS);
    resetKey = key("ResetViewKey", 0);
    std::string padText;
    unsigned badBinds = 0;
    for (int i = 0; i < kPadActions; ++i) {
        char bind[48]{};
        GetPrivateProfileStringA("CameraTools", kPadActionInfo[i].key, kPadActionInfo[i].fallback, bind, sizeof(bind), ini.c_str());
        if (!coop_camtools::ParseBind(bind, padBind[i])) {
            ++badBinds;
            std::fprintf(log, "camtools: [CameraTools] %s=%s is no button name; the default %s is used\n", kPadActionInfo[i].key, bind, kPadActionInfo[i].fallback);
            coop_camtools::ParseBind(kPadActionInfo[i].fallback, padBind[i]);
            std::snprintf(bind, sizeof(bind), "%s", kPadActionInfo[i].fallback);
        }
        padText += std::string(padText.empty() ? "" : " ") + (kPadActionInfo[i].key + 3) + "=" + bind;
    }
    notices = GetPrivateProfileIntA("CameraTools", "Notices", 1, ini.c_str());
    if (notices < 0 || notices > 2) notices = 1;
    char text[32]{};
    GetPrivateProfileStringA("CameraTools", "SlowFactor", "0.25", text, sizeof(text), ini.c_str());
    slowFactor = static_cast<float>(std::atof(text));
    if (!(slowFactor >= kMinSpeed && slowFactor <= 1.0f)) slowFactor = 0.25f;
    timeScale = coop_camtools::TimeStep(slowFactor, 0);
    if (timeScale >= 1.0f) timeScale = 0.75f;
    GetPrivateProfileStringA("CameraTools", "MoveSpeed", "3.0", text, sizeof(text), ini.c_str());
    tuning.moveSpeed = static_cast<float>(std::atof(text));
    if (!(tuning.moveSpeed > 0.1f && tuning.moveSpeed < 50.0f)) tuning.moveSpeed = 3.0f;
    GetPrivateProfileStringA("CameraTools", "LookSpeed", "1.4", text, sizeof(text), ini.c_str());
    tuning.lookSpeed = static_cast<float>(std::atof(text));
    if (!(tuning.lookSpeed > 0.1f && tuning.lookSpeed < 10.0f)) tuning.lookSpeed = 1.4f;
    GetPrivateProfileStringA("CameraTools", "Smoothing", "0.25", text, sizeof(text), ini.c_str());
    tuning.smoothing = static_cast<float>(std::atof(text));
    if (!(tuning.smoothing >= 0.0f && tuning.smoothing <= 2.0f)) tuning.smoothing = 0.25f;
    tuning.invertLook = GetPrivateProfileIntA("CameraTools", "InvertLook", 0, ini.c_str()) != 0;
    deadZone = GetPrivateProfileIntA("CameraTools", "DeadZone", 7849, ini.c_str());
    if (deadZone < 0 || deadZone > 30000) deadZone = 7849;
    hideHudInFreeCam = GetPrivateProfileIntA("CameraTools", "HideHudInFreeCam", 1, ini.c_str()) != 0;
    loopPath = GetPrivateProfileIntA("CameraTools", "LoopPath", 0, ini.c_str()) != 0;
    const size_t slash = ini.find_last_of("\\/");
    pathFolder = (slash == std::string::npos ? std::string(".") : ini.substr(0, slash)) + "\\CameraPaths";
    QueryPerformanceFrequency(&frequency);
    unsigned char seen[16]{};
    speedOk = Read(base + kSpeedReadRva, seen, sizeof(kSpeedReadBytes)) && std::memcmp(seen, kSpeedReadBytes, sizeof(kSpeedReadBytes)) == 0;
    // the game's pad slot answers an idle pad while the camera flies (focusrate::GatePad)
    if (!focusrate::realPad)
        focusrate::RedirectSlot(log, 0x1D59184, "xinput1_3.dll", "XInputGetState", reinterpret_cast<void*>(focusrate::GatePad), reinterpret_cast<void**>(&focusrate::realPad));
    padFn = focusrate::realPad;
    installed = hook_site::Install(log, "camtools Render::Poll", base + kRenderPollRva, kRenderPollBytes, sizeof(kRenderPollBytes), reinterpret_cast<void*>(OnRenderPoll),
                                   pollHook);
    std::fprintf(log, "camtools %s: keys free %d slow %d freeze %d record %d play %d hud %d; speed float %s, HUD: %s, pad %s (game muted while flying: %s); "
                      "slow x%.2f move %.1f m/s look %.1f rad/s smoothing %.2f s; paths in %s\n",
                 installed ? "installed" : "NOT installed", freeKey, slowKey, freezeKey, recordKey, playKey, hideKey, speedOk ? "ok" : "MISMATCH (no slow motion)",
                 "our overlay only", padFn ? "read through the original XInputGetState" : "MISSING (no pad flying)", padFn ? "yes" : "no",
                 timeScale, tuning.moveSpeed, tuning.lookSpeed, tuning.smoothing, pathFolder.c_str());
    std::fprintf(log, "camtools keys camera speed %d/%d, game speed %d/%d, tilt %d/%d, zoom %d/%d, reset %d; pad %s%s; notes %s\n", speedUpKey, speedDownKey,
                 timeFasterKey, timeSlowerKey, rollLeftKey, rollRightKey, zoomInKey, zoomOutKey, resetKey, padText.c_str(), badBinds ? " (some defaults, see above)" : "",
                 notices == 0 ? "off" : notices == 1 ? "small, in the corner, none during playback" : "banners");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&swaps, 0, 0) / 600 + InterlockedCompareExchange(&faults, 0, 0) * 7 + InterlockedCompareExchange(&freeCamToggles, 0, 0) * 11 +
                     InterlockedCompareExchange(&recordings, 0, 0) * 13 + InterlockedCompareExchange(&plays, 0, 0) * 17 + InterlockedCompareExchange(&hudToggles, 0, 0) * 19 +
                     InterlockedCompareExchange(&speedWrites, 0, 0) * 23;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "camtools polls=%ld swaps=%ld no_camera=%ld faults=%ld pad reads=%ld fails=%ld free_cam=%ld recordings=%ld samples=%ld plays=%ld speed_writes=%ld "
                      "hud_toggles=%ld\n",
                 InterlockedCompareExchange(&polls, 0, 0), InterlockedCompareExchange(&swaps, 0, 0), InterlockedCompareExchange(&skipsNoManager, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0), InterlockedCompareExchange(&padReads, 0, 0), InterlockedCompareExchange(&padFails, 0, 0),
                 InterlockedCompareExchange(&freeCamToggles, 0, 0), InterlockedCompareExchange(&recordings, 0, 0), InterlockedCompareExchange(&samples, 0, 0),
                 InterlockedCompareExchange(&plays, 0, 0), InterlockedCompareExchange(&speedWrites, 0, 0), InterlockedCompareExchange(&hudToggles, 0, 0));
}
}  // namespace camtools
