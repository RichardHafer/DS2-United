// Dead Space 2 United by Hafer.
// The bot's own look input against the ASI loader's mouse fix.
//
// DINPUT8.dll in the game folder (the ASI loader with its raw mouse look)
// reroutes seven engine calls of AimingData::UpdateAim(pitch, yaw, roll, type)
// (PC 0x00F4E260, thiscall, ret 0x10): the chase camera, an aim camera, the aim
// state machine twice and three special states. Its replacement drops the stick
// deltas the engine passes and applies the raw mouse instead, for whichever
// Player the call belongs to. The bot's camera therefore turned with the human's
// mouse, only while the mouse moved, and pad 14's right stick did nothing
// (measured live 26.09.). The deltas reach those sites already mixed with the
// mouse, so the bot does not look through them at all: each call site gets a
// stub in front, the bot's calls go to the engine function with zero deltas,
// all others to whatever sat there before (the mouse fix, or the engine when
// there is none). The bot turns by Turn() instead, straight on its aim data.
// The stubs are laid in again when something rewrites a site later.
namespace bot_aim {
constexpr uint32_t kUpdateAimRva = 0xB4E260;
enum class Owner : uint8_t { CameraTarget, EsiOwner, EbxOwner, AimData };
struct Site {
    uint32_t rva;
    unsigned char before[8];   // the instructions that push the arguments
    Owner owner;
    const char* name;
};
// CameraTarget: esi is the RE4ChaseCamera, its target Player SafePtr at +0x7C.
// EsiOwner/EbxOwner: the register holds a player state machine, owner at +0x50.
// AimData: ecx is the Player's AimingData component or one of its blocks.
const Site kSites[] = {
    {0xB6E91E, {0x24, 0xB0, 0x00, 0x00, 0x00, 0xD9, 0x1C, 0x24}, Owner::CameraTarget, "chase camera"},
    {0x46EB80, {0x04, 0xD9, 0x44, 0x24, 0x28, 0xD9, 0x1C, 0x24}, Owner::EsiOwner, "aim camera"},
    {0x4990BE, {0x04, 0x8D, 0x4C, 0x24, 0x40, 0xD9, 0x1C, 0x24}, Owner::EbxOwner, "aim state 1"},
    {0x49915F, {0x00, 0xD9, 0x5C, 0x24, 0x04, 0xD9, 0x1C, 0x24}, Owner::EbxOwner, "aim state 2"},
    {0x79EA6C, {0x04, 0xD9, 0x44, 0x24, 0x30, 0xD9, 0x1C, 0x24}, Owner::AimData, "special state 1"},
    {0xA91D54, {0x00, 0x8B, 0xCE, 0xF3, 0x0F, 0x11, 0x04, 0x24}, Owner::AimData, "special state 2"},
    {0xCAE604, {0x24, 0x04, 0xD9, 0x47, 0x60, 0xD9, 0x1C, 0x24}, Owner::AimData, "special state 3"},
};
constexpr unsigned kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

bool enabled = false;
uint32_t imageEnd = 0;
uintptr_t chained[kSiteCount]{};   // what the site called before the stub
bool laid[kSiteCount]{}, refused[kSiteCount]{};
volatile LONG botCalls = 0, otherCalls = 0, relays = 0, foreignChains = 0, turns = 0, turnFaults = 0;
// Written by a stub right before it jumps to Dispatch (game thread only).
uint32_t stubEsi = 0, stubEbx = 0, stubSite = 0;

bool OwnedBy(unsigned site, uint32_t aim, uint32_t bot) {
    uint32_t value = 0;
    switch (kSites[site].owner) {
    case Owner::CameraTarget:
        return stubEsi && Read(stubEsi + 0x7C, &value, 4) && value == bot + 0x10;
    case Owner::EsiOwner:
        return stubEsi && Read(stubEsi + 0x50, &value, 4) && value == bot;
    case Owner::EbxOwner:
        return stubEbx && Read(stubEbx + 0x50, &value, 4) && value == bot;
    case Owner::AimData:
        return Read(bot + 0x6A0, &value, 4) && value && aim >= value && aim < value + 0x160;
    }
    return false;
}
bool OwnsBot(unsigned site, uint32_t aim) {
    for (uint32_t bot : botEntities) if (bot && OwnedBy(site, aim, bot)) return true;
    return false;
}

using UpdateAim = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
void __fastcall Dispatch(void* aim, void*, uint32_t pitch, uint32_t yaw, uint32_t roll, uint32_t type) {
    const unsigned site = stubSite < kSiteCount ? stubSite : 0;
    if (cutscene::running && mirror::scenechain::attempt && cutscene::leadaim::enabled) {
        static uint32_t lastAttempt[kSiteCount]{}, lastOwner[kSiteCount]{};
        static unsigned lines = 0;
        if (lines < 100 && (lastAttempt[site] != mirror::scenechain::attempt || lastOwner[site] != mirror::scenechain::owner)) {
            const uint32_t player = LocalCampaignPlayer();
            if (player && OwnedBy(site, reinterpret_cast<uint32_t>(aim), player)) {
                lastAttempt[site] = mirror::scenechain::attempt;
                lastOwner[site] = mirror::scenechain::owner;
                ++lines;
                gate::Event("scene aim: local %s root=%08X attempt=%u following=%d type=%u", kSites[site].name, mirror::scenechain::root,
                            mirror::scenechain::attempt, cutscene::following, type);
            }
        }
    }
    uintptr_t target = chained[site];
    if (OwnsBot(site, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(aim)))) {
        InterlockedIncrement(&botCalls);
        target = base + kUpdateAimRva;
        pitch = 0;   // the bot's look comes from Turn(), never from a mouse
        yaw = 0;
    } else if (!focusrate::GameSees()) {
        // another window has the focus, or a panel holds the mouse: the loader's raw mouse look would turn this copy
        InterlockedIncrement(&otherCalls);
        InterlockedIncrement(&focusrate::aimMuted);
        target = base + kUpdateAimRva;
        pitch = 0;
        yaw = 0;
    } else {
        InterlockedIncrement(&otherCalls);
    }
    if (!target) target = base + kUpdateAimRva;
    reinterpret_cast<UpdateAim>(target)(aim, pitch, yaw, roll, type);
}

// Turns one aim data block of the bot by these deltas (radians, pitch positive
// looks down), the way the camera does: SOURCE_RELATIVE. Game thread only.
__declspec(noinline) bool TurnRaw(uint32_t block, float pitch, float yaw) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, float, float, float, uint32_t)>(base + kUpdateAimRva)(
            reinterpret_cast<void*>(static_cast<uintptr_t>(block)), pitch, yaw, 0.0f, 0u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool Turn(uint32_t entity, uint32_t blockOffset, float pitch, float yaw) {
    uint32_t adc = 0;
    if (!enabled || !entity || !Read(entity + 0x6A0, &adc, 4) || !adc) return false;
    if (!TurnRaw(adc + blockOffset, pitch, yaw)) {
        if (InterlockedIncrement(&turnFaults) <= 5) gate::Event("bot_aim: turning the bot's aim data faulted (block +%X)", blockOffset);
        return false;
    }
    InterlockedIncrement(&turns);
    return true;
}

#define BOT_AIM_STUB(index)                                            \
    __declspec(naked) void Stub##index() {                             \
        __asm { mov stubEsi, esi }                                     \
        __asm { mov stubEbx, ebx }                                     \
        __asm { mov stubSite, index }                                  \
        __asm { jmp Dispatch }                                         \
    }
BOT_AIM_STUB(0)
BOT_AIM_STUB(1)
BOT_AIM_STUB(2)
BOT_AIM_STUB(3)
BOT_AIM_STUB(4)
BOT_AIM_STUB(5)
BOT_AIM_STUB(6)
#undef BOT_AIM_STUB
void (*const kStubs[kSiteCount])() = {Stub0, Stub1, Stub2, Stub3, Stub4, Stub5, Stub6};

// Zero-G flight. The zero-G machine's message handler (0x008AC420) turns a
// flying Isaac through FloatLook (0x00887720, thiscall(sm, float), ret 4), which
// reads its look rates at sm+0x7C/+0x80. The mouse fix hooks that function's
// entry and writes the raw mouse there for every Player's machine, so a flying
// bot turned with the human's mouse (27.09.). Its one call (0x008AC5F7) gets a
// stub: for a bot's machine the rates are cleared and the body runs behind its
// own copy of the prologue, past the hook; everyone else goes on as before.
constexpr uint32_t kFloatLookSiteRva = 0x4AC5F7, kFloatLookRva = 0x487720, kFloatLookBody = 6, kFloatRates = 0x7C, kFloatGain = 0x84,
                   kFloatMouseFlag = 0x194, kMachineOwner = 0x50;
uintptr_t floatChained = 0, floatBody = 0;
bool floatLaid = false, floatRefused = false;
volatile LONG floatBotCalls = 0, floatOtherCalls = 0, floatRelays = 0, floatSteered = 0;
// A flying bot's own look command (runtime_coop_bot.inc, "zero-G flight"): yaw > 0
// turns its body left, pitch > 0 down, -1..1 as the machine reads its right stick.
struct FloatCommand { uint32_t entity = 0; float yaw = 0.0f, pitch = 0.0f; DWORD at = 0; };
FloatCommand floatCommands[4];
void SetFloatLook(uint32_t entity, float yaw, float pitch, DWORD now) {
    FloatCommand* slot = nullptr;
    for (FloatCommand& c : floatCommands) if (c.entity == entity) slot = &c;
    if (!slot) for (FloatCommand& c : floatCommands) if (!c.entity || now - c.at > 5000) { slot = &c; break; }
    if (!slot) return;
    slot->entity = entity;
    slot->yaw = yaw;
    slot->pitch = pitch;
    slot->at = now;
}

// sub esp,14h / push ebx / mov ebx,ecx: the first six bytes of FloatLook
__declspec(naked) void FloatLookBody() {
    __asm {
        sub esp, 14h
        push ebx
        mov ebx, ecx
        jmp dword ptr [floatBody]
    }
}
using FloatLook = void(__thiscall*)(void*, float);
void __fastcall OnFloatLook(void* sm, void*, float step) {
    const uintptr_t machine = reinterpret_cast<uintptr_t>(sm);
    uint32_t owner = 0;
    bool bot = false;
    if (Read(machine + kMachineOwner, &owner, 4) && owner)
        for (uint32_t b : botEntities) if (b == owner) bot = true;
    if (bot && floatBody) {
        InterlockedIncrement(&floatBotCalls);
        // Its own command when one is fresh, else none. The game's own mouse filter
        // may have set the gain (+0x84) and the mouse flag (+0x194) for every Player:
        // both cleared, so the stick curve applies to the bot's numbers.
        float look[2] = {0.0f, 0.0f};
        const DWORD now = GetTickCount();
        for (const FloatCommand& c : floatCommands)
            if (c.entity == owner && now - c.at < 300) { look[0] = c.yaw; look[1] = c.pitch; InterlockedIncrement(&floatSteered); }
        const float noGain = 0.0f;
        const unsigned char stick = 0;
        WriteBytes(machine + kFloatRates, look, sizeof(look));
        WriteBytes(machine + kFloatGain, &noGain, sizeof(noGain));
        WriteBytes(machine + kFloatMouseFlag, &stick, sizeof(stick));
        reinterpret_cast<FloatLook>(reinterpret_cast<uintptr_t>(&FloatLookBody))(sm, step);
        return;
    }
    InterlockedIncrement(&floatOtherCalls);
    reinterpret_cast<FloatLook>(floatChained ? floatChained : base + kFloatLookRva)(sm, step);
}

// Lays the zero-G stub in when it is missing. True while the site carries it.
bool RefreshFloat(FILE* log) {
    if (floatRefused) return false;
    const uintptr_t site = base + kFloatLookSiteRva;
    unsigned char code[13]{};
    if (!Read(site - 8, code, sizeof(code))) return false;
    const uintptr_t stub = reinterpret_cast<uintptr_t>(&OnFloatLook);
    int32_t rel = 0;
    std::memcpy(&rel, code + 9, 4);
    const uintptr_t current = site + 5 + rel;
    if (code[8] == 0xE8 && current == stub) return true;
    // ret 14h's last byte / fld dword [ebp+0Ch] / push ecx / fstp dword [esp]
    static const unsigned char kBefore[] = {0x00, 0xD9, 0x45, 0x0C, 0x51, 0xD9, 0x1C, 0x24};
    const uintptr_t original = base + kFloatLookRva;
    const bool foreign = current < base || current >= imageEnd;
    if (std::memcmp(code, kBefore, sizeof(kBefore)) != 0 || code[8] != 0xE8 || (current != original && !foreign)) {
        floatRefused = true;
        if (log) std::fprintf(log, "bot_aim zero-G look at %08X unexpected (call byte %02X, target %08X); left alone\n", static_cast<unsigned>(site), code[8],
                              static_cast<unsigned>(current));
        else gate::Event("bot_aim: zero-G look at %08X changed into something unexpected; left alone", static_cast<unsigned>(site));
        return false;
    }
    floatChained = current;
    unsigned char call[5] = {0xE8};
    const int32_t toStub = static_cast<int32_t>(stub - (site + 5));
    std::memcpy(call + 1, &toStub, 4);
    if (!WriteBytes(site, call, sizeof(call))) return false;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(call));
    if (floatLaid) InterlockedIncrement(&floatRelays);
    floatLaid = true;
    unsigned char entry = 0;
    Read(original, &entry, 1);
    if (log) std::fprintf(log, "bot_aim zero-G look at %08X: stub in front of %s %08X (its entry %s)\n", static_cast<unsigned>(site),
                          foreign ? "the foreign hook" : "the engine", static_cast<unsigned>(current), entry == 0xE9 ? "hooked by the mouse fix" : "untouched");
    else gate::Event("bot_aim: zero-G look at %08X rewritten by someone else, stub laid in again (now in front of %08X)", static_cast<unsigned>(site),
                     static_cast<unsigned>(current));
    return true;
}

// Lays the stubs in where they are missing. Returns how many sites carry one.
unsigned Refresh(FILE* log) {
    unsigned carried = 0;
    for (unsigned i = 0; i < kSiteCount; ++i) {
        if (refused[i]) continue;
        const uintptr_t site = base + kSites[i].rva;
        unsigned char code[13]{};
        if (!Read(site - 8, code, sizeof(code))) continue;
        const uintptr_t stub = reinterpret_cast<uintptr_t>(kStubs[i]);
        int32_t rel = 0;
        std::memcpy(&rel, code + 9, 4);
        const uintptr_t current = site + 5 + rel;
        if (code[8] == 0xE8 && current == stub) { ++carried; continue; }
        const uintptr_t original = base + kUpdateAimRva;
        const bool foreign = current < base || current >= imageEnd;
        if (std::memcmp(code, kSites[i].before, 8) != 0 || code[8] != 0xE8 || (current != original && !foreign)) {
            refused[i] = true;
            if (log) std::fprintf(log, "bot_aim %s at %08X unexpected (call byte %02X, target %08X); left alone\n", kSites[i].name,
                                  static_cast<unsigned>(site), code[8], static_cast<unsigned>(current));
            else gate::Event("bot_aim: %s at %08X changed into something unexpected; left alone", kSites[i].name, static_cast<unsigned>(site));
            continue;
        }
        chained[i] = current;
        unsigned char call[5] = {0xE8};
        const int32_t toStub = static_cast<int32_t>(stub - (site + 5));
        std::memcpy(call + 1, &toStub, 4);
        if (!WriteBytes(site, call, sizeof(call))) continue;
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(call));
        if (foreign) InterlockedIncrement(&foreignChains);
        if (laid[i]) InterlockedIncrement(&relays);
        laid[i] = true;
        ++carried;
        if (log) std::fprintf(log, "bot_aim %s at %08X: stub in front of %s %08X\n", kSites[i].name, static_cast<unsigned>(site),
                              foreign ? "the foreign hook" : "the engine", static_cast<unsigned>(current));
        else gate::Event("bot_aim: %s at %08X rewritten by someone else, stub laid in again (now in front of %08X)", kSites[i].name,
                         static_cast<unsigned>(site), static_cast<unsigned>(current));
    }
    if (floatBody) RefreshFloat(log);
    return carried;
}

bool Install(FILE* log, bool wanted) {
    enabled = false;
    if (!wanted) {
        std::fprintf(log, "bot_aim off (the bot's look input goes through a mouse fix if one is installed)\n");
        return false;
    }
    // push ebp / mov ebp,esp / and esp,-16 / sub esp,54h / movss xmm0,[ebp+8] / mov eax,[ebp+14h]
    static const unsigned char kUpdateAim[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x54, 0xF3, 0x0F, 0x10, 0x45, 0x08, 0x8B, 0x45, 0x14};
    unsigned char actual[sizeof(kUpdateAim)]{};
    if (!Read(base + kUpdateAimRva, actual, sizeof(actual)) || std::memcmp(actual, kUpdateAim, sizeof(actual)) != 0) {
        std::fprintf(log, "bot_aim AimingData::UpdateAim signature mismatch at %08X; the bot's look stays with the mouse fix\n",
                     static_cast<unsigned>(base + kUpdateAimRva));
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    imageEnd = static_cast<uint32_t>(base + nt->OptionalHeader.SizeOfImage);
    // cmp byte [ebx+194h],0 / movss xmm2,[ebx+7Ch]: FloatLook right behind its prologue
    static const unsigned char kFloatBody[] = {0x80, 0xBB, 0x94, 0x01, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x53, 0x7C};
    unsigned char body[sizeof(kFloatBody)]{};
    if (Read(base + kFloatLookRva + kFloatLookBody, body, sizeof(body)) && std::memcmp(body, kFloatBody, sizeof(body)) == 0) {
        floatBody = base + kFloatLookRva + kFloatLookBody;
    } else {
        floatRefused = true;
        std::fprintf(log, "bot_aim FloatLook body mismatch at %08X; a flying bot's look stays with the mouse fix\n",
                     static_cast<unsigned>(base + kFloatLookRva + kFloatLookBody));
    }
    const unsigned carried = Refresh(log);
    enabled = true;
    std::fprintf(log, "bot_aim sites=%u/%u foreign_hooks=%ld zero_g_look=%d (the bot's camera and aim take pad 14, everyone else keeps the mouse fix)\n",
                 carried, kSiteCount, foreignChains, floatLaid ? 1 : 0);
    return true;
}

void Tick(DWORD now) {
    static DWORD last = 0;
    if (!enabled || now - last < 1000) return;
    last = now;
    Refresh(nullptr);
}

void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG n = botCalls + relays + turns / 64 + turnFaults + floatBotCalls / 16 + floatRelays;
    if (n == last) return;
    last = n;
    unsigned carried = 0, refusedCount = 0;
    for (unsigned i = 0; i < kSiteCount; ++i) { carried += laid[i] ? 1u : 0u; refusedCount += refused[i] ? 1u : 0u; }
    std::fprintf(log, "bot_aim sites=%u refused=%u foreign_hooks=%ld relays=%ld bot_calls=%ld other_calls=%ld turns=%ld turn_faults=%ld "
                      "zero_g_look=%d refused=%d relays=%ld bot_calls=%ld other_calls=%ld steered=%ld\n",
                 carried, refusedCount, foreignChains, relays, botCalls, otherCalls, turns, turnFaults, floatLaid ? 1 : 0, floatRefused ? 1 : 0,
                 floatRelays, floatBotCalls, floatOtherCalls, floatSteered);
}
}  // namespace bot_aim
