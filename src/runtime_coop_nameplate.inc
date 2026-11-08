// Dead Space 2 United by Richard Hafer.
// Names over the partners' and the bots' heads with the game's own multiplayer
// nameplate ([Nameplate] in DS2United.ini; analysis/DS2_NATIVE_NAMEPLATES_2026-10-10.md).
//
// Player::CreateGhostNameVFX (0x0086B640) wants MPLevelSettings and Blaze for the
// name, and without Blaze it left the VFX without a text context, which crashed
// the width measurement at 0x0086AEDE. So its steps are done here, with our own
// name, once per Player (a partner's ghost, a host bot's ghost, a local bot):
//   1. the helper FXObject 58CA14A4-D14CE79C (GetSpawnInfo 0x00776440 + SpawnEntity
//      0x00776480, the MP block lifted for this one spawn);
//   2. mp_name_text.vfb 70E8FA33-8C1BF09B on it (Alchemy::Create_ByGuid 0x006A04A0,
//      cdecl, hidden return) into Player+0x16EC (VFXHandle::operator= 0x006D2130,
//      ~VFXHandle 0x0069F700);
//   3. a VFXContextFXText (alloc 0x006DD790, vtable 0x01B15AD0, colour and size
//      1.0) with SetString 0x006F4230 (copies a char*), SetContext 0x0069ED80;
//   4. the helper in the SafePtr at Player+0x16EC+0x2C (=+0x1718) as onGhostAdd links it.
// A ghost then gets the rest natively from PlayerUpdateAimingVFX (0x00891A70): the
// helper follows the head, 2 s in view shows it, 5 s out of view hides it; the
// Player destructor kills it. A local bot is no ghost: UpdateGhostNameEntity
// (0x0087A820) and UpdateNameVis (0x008619A0) run for it from here every tick.
// Before a level change every plate goes the way HandleMPEndOfGame (0x0086C7E0)
// takes it: the VFX killed, the helper deleted, the SafePtr cleared.
//
// The VFX and the helper live only in global_mp_assets: plates on the MP maps
// (survival), none in the campaign (logged once per level).
//
// Names: each machine sends its own ([Nameplate] Name, "PLAYER n" when empty)
// and, as the host, its bots' names in a 132-byte 'NAME' packet every 2 s. A
// ghost's owner is the machine byte of its sub-key (MachineOf: 0..15 the humans,
// 0xF0 + n the host's bots).
namespace nameplate {
bool enabled = true, bots = true, installed = false;
char ownName[24] = "";   // [Nameplate] Name, cleaned; empty: "PLAYER n" by the machine index of the moment

constexpr uint32_t kHelperGuid[4] = {0x58CA14A4u, 0xD14CE79Cu, 0x3532554Cu, 0x58583337u};
constexpr uint32_t kNameVfxGuid[4] = {0x70E8FA33u, 0x70E8FA33u, 0x70E8FA33u, 0x8C1BF09Bu};
constexpr uint32_t kCreateByGuidRva = 0x2A04A0, kHandleAssignRva = 0x2D2130, kHandleDtorRva = 0x29F700, kAllocRva = 0x2DD790;
constexpr uint32_t kSetStringRva = 0x2F4230, kSetContextRva = 0x29ED80, kGetContextRva = 0x29E120, kKillRva = 0x29DF60;
constexpr uint32_t kShowRva = 0x29DFE0, kHideRva = 0x29E020, kUnlinkRva = 0x3429F0, kUpdateEntityRva = 0x47A820, kUpdateNameVisRva = 0x4619A0;
constexpr uint32_t kTextVtableRva = 0x1715AD0, kPlayerSettingsRva = 0x1C87C38, kShowAfter = 0xB44, kHideAfter = 0xB48;
constexpr uint32_t kNameVfx = 0x16EC, kNameWidth = 0x16F8, kHelperPtr = 0x1718, kFirstSeen = 0x172C, kLastSeen = 0x1730, kIsDeadSlot = 0x200;
// A partner's NecroPlayer has a plate of its own (13.10.: none showed, the module named
// Players only): NecroPlayer::onGhostAdd builds it with the same steps, and
// NecroPlayer::UpdateVFX (0x010D0290, its ghosts only) moves and shows it from these
// fields: +0x520 the name VFX, +0x52C the measured width, +0x540 the helper SafePtr.
constexpr uint32_t kNecroNameVfx = 0x520, kNecroNameWidth = 0x52C, kNecroHelperPtr = 0x540;
// Which text a plate gets (CreateGhostNameVFX step 6): a human sees every name in
// mp_name_text.vfb; a necro sees his allies in mp_name_text_necro.vfb and the humans in
// mp_name_text_ns.vfb. All three live in global_mp_assets.
constexpr uint32_t kNecroNameVfxGuid[4] = {0x3A90BA3Du, 0x3A90BA3Du, 0x3A90BA3Du, 0xB9E40266u};
constexpr uint32_t kEnemyNameVfxGuid[4] = {0x312C73A5u, 0x312C73A5u, 0x312C73A5u, 0x72C756C2u};
constexpr uint32_t kGameManagerRva = 0x1C213FC;   // DGM; +0xF2 the local team, 1 = necro
uint32_t NameVfxOf(uint32_t body) { return IsNecroEntity(body) ? kNecroNameVfx : kNameVfx; }
uint32_t NameWidthOf(uint32_t body) { return IsNecroEntity(body) ? kNecroNameWidth : kNameWidth; }
uint32_t HelperPtrOf(uint32_t body) { return IsNecroEntity(body) ? kNecroHelperPtr : kHelperPtr; }
const uint32_t* VfxGuidFor(uint32_t body) {
    uint32_t dgm = 0;
    uint8_t localTeam = 0;
    if (!Read(base + kGameManagerRva, &dgm, 4) || !dgm || !Read(dgm + 0xF2, &localTeam, 1) || localTeam != 1) return kNameVfxGuid;
    return IsNecroEntity(body) ? kNecroNameVfxGuid : kEnemyNameVfxGuid;
}
constexpr uint32_t kEdmFilterByte = 0x28;

struct Site { const char* name; uint32_t rva; unsigned char bytes[12]; unsigned size; };
const Site kSites[] = {
    {"Alchemy::Create_ByGuid", kCreateByGuidRva, {0x83, 0xEC, 0x10, 0x8B, 0x44, 0x24, 0x28, 0x8B, 0x4C, 0x24, 0x30, 0x8B}, 12},
    {"VFXHandle::operator=", kHandleAssignRva, {0x8B, 0xC1, 0x8B, 0x08, 0x85, 0xC9, 0x74, 0x0D, 0xC7, 0x41, 0x08, 0x00}, 12},
    {"VFXHandle::~VFXHandle", kHandleDtorRva, {0x8B, 0x01, 0x85, 0xC0, 0x74, 0x0D, 0xC7, 0x40, 0x08, 0x00, 0x00, 0x00}, 12},
    {"VFX alloc", kAllocRva, {0x83, 0xEC, 0x0C, 0xE8}, 4},
    {"VFXContextFXText::SetString", kSetStringRva, {0x53, 0x55, 0x8B, 0xD9, 0x8B, 0x53, 0x20, 0x56, 0x57, 0x85, 0xD2, 0x74}, 12},
    {"VFXHandle::SetContext", kSetContextRva, {0x56, 0x8B, 0x31, 0x85, 0xF6, 0x74, 0x2F, 0x8B, 0x46, 0x20, 0x85, 0xC0}, 12},
    {"VFXHandle::GetContext", kGetContextRva, {0x8B, 0x01, 0x85, 0xC0, 0x74, 0x04, 0x8B, 0x40, 0x20, 0xC3}, 10},
    {"VFXHandle::Kill", kKillRva, {0x8B, 0x09, 0x85, 0xC9, 0x74, 0x05, 0xE9}, 7},
    {"VFXHandle::Show", kShowRva, {0x8B, 0x09, 0x85, 0xC9, 0x74, 0x05, 0xE9}, 7},
    {"VFXHandle::Hide", kHideRva, {0x8B, 0x09, 0x85, 0xC9, 0x74, 0x05, 0xE9}, 7},
    {"SafePtr unlink", kUnlinkRva, {0x8B, 0x41, 0x04, 0x8B, 0x54, 0x24, 0x04, 0x3B, 0xC2, 0x75, 0x09}, 11},
    {"UpdateGhostNameEntity", kUpdateEntityRva, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x14, 0x81, 0xEC, 0x04}, 12},
    {"UpdateNameVis", kUpdateNameVisRva, {0x80, 0x7C, 0x24, 0x04, 0x00, 0xA1}, 6},
};

// --- names ------------------------------------------------------------------------------------
constexpr uint32_t kNameMagic = 0x454D414Eu;   // 'NAME'
constexpr uint8_t kNameVersion = 1;
#pragma pack(push, 1)
struct NameEntry { uint8_t key; char name[23]; };
struct NamePacket { uint32_t magic; uint8_t version, count; uint16_t reserved; uint32_t token; NameEntry entries[5]; };
#pragma pack(pop)
static_assert(sizeof(NamePacket) == 132, "name packet size");
char names[256][24]{};   // by machine byte; game thread
DWORD namesSentAt = 0;
volatile LONG namePacketsSent = 0, namePacketsIn = 0, namesChanged = 0;
// Printable ASCII only (the font has no more), at most 20 characters, never empty.
void Clean(const char* in, char (&out)[24], const char* fallback) {
    size_t n = 0;
    for (const char* c = in; c && *c && n < 20; ++c)
        if (*c >= 32 && *c <= 126) out[n++] = *c;
    while (n && out[n - 1] == ' ') --n;
    out[n] = 0;
    if (!n) std::snprintf(out, sizeof(out), "%s", fallback);
}
void SetName(unsigned key, const char* name) {
    if (key > 255 || !name) return;
    char clean[24];
    Clean(name, clean, names[key][0] ? names[key] : "?");
    if (std::strcmp(clean, names[key]) == 0) return;
    std::snprintf(names[key], sizeof(names[key]), "%s", clean);
    InterlockedIncrement(&namesChanged);
}
const char* OwnName(char (&buffer)[24]) {
    if (ownName[0]) return ownName;
    std::snprintf(buffer, sizeof(buffer), "PLAYER %u", machineIndex + 1);
    return buffer;
}
void SendNames(DWORD now) {
    if (!network || now - namesSentAt < 2000) return;
    namesSentAt = now;
    NamePacket p{};
    p.magic = kNameMagic;
    p.version = kNameVersion;
    p.token = peerReadiness.token;
    char own[24];
    p.entries[p.count].key = static_cast<uint8_t>(machineIndex);
    std::snprintf(p.entries[p.count].name, sizeof(p.entries[0].name), "%s", OwnName(own));
    ++p.count;
    if (IsHost())
        for (unsigned i = 0; i < bot::wantedBots && i < kMaxBots && p.count < 5; ++i) {
            if (!bot::units[i].entity) continue;
            p.entries[p.count].key = static_cast<uint8_t>(0xF0 + i);
            std::snprintf(p.entries[p.count].name, sizeof(p.entries[0].name), "%s", bot::units[i].name);
            ++p.count;
        }
    if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&namePacketsSent);
}
// From the side channel's receive loop. True when it was ours.
bool OnNamePacket(const NamePacket& p);
bool OnNameBytes(const void* data, int size) {
    if (size != static_cast<int>(sizeof(NamePacket))) return false;
    NamePacket p{};
    std::memcpy(&p, data, sizeof(p));
    return p.magic == kNameMagic && OnNamePacket(p);
}
bool OnNamePacket(const NamePacket& p) {
    if (p.version != kNameVersion || !KnownPeerToken(p.token)) return false;
    InterlockedIncrement(&namePacketsIn);
    for (unsigned i = 0; i < p.count && i < 5; ++i) {
        char name[24]{};
        std::memcpy(name, p.entries[i].name, sizeof(p.entries[i].name));
        if (p.entries[i].key != (machineIndex & 0xFF)) SetName(p.entries[i].key, name);
    }
    return true;
}

// --- engine calls -------------------------------------------------------------------------
struct alignas(16) Matrix { float m[16]; };
volatile LONG faults = 0;
__declspec(noinline) uint32_t SpawnHelperRaw(uint32_t sim, unsigned char* info) {
    __try {
        using GetInfo = void*(__thiscall*)(void*, void*, const uint32_t*, bool);
        using Spawn = uint32_t(__thiscall*)(void*, void*, uint32_t);
        reinterpret_cast<GetInfo>(base + kGetSpawnInfoRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(sim)), info, kHelperGuid, false);
        uint32_t instance = 0;
        std::memcpy(&instance, info, 4);
        if (!instance) return 0;   // the template is not resident here
        return reinterpret_cast<Spawn>(base + kSpawnEntityRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(sim)), info, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
// The VFX on the helper; the handle is moved into the Player's slot. 0 when not resident.
__declspec(noinline) uint32_t CreateVfxRaw(uint32_t player, uint32_t helper, const uint32_t* vfxGuid) {
    __try {
        Matrix identity{};
        identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
        uint32_t tmp = 0;
        using Create = uint32_t*(__cdecl*)(uint32_t*, const uint32_t*, const Matrix*, uint32_t, uint32_t, uint32_t, int, const void*);
        reinterpret_cast<Create>(base + kCreateByGuidRva)(&tmp, vfxGuid, &identity, 0xFFFFu, 0u, helper, -1, nullptr);
        if (!tmp) return 0;
        reinterpret_cast<void(__thiscall*)(void*, uint32_t*)>(base + kHandleAssignRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player + NameVfxOf(player))), &tmp);
        reinterpret_cast<void(__thiscall*)(uint32_t*)>(base + kHandleDtorRva)(&tmp);
        uint32_t handle = 0;
        Read(player + NameVfxOf(player), &handle, 4);
        return handle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
// The text context, built as CreateGhostNameVFX's inline ctor does, then attached.
__declspec(noinline) uint32_t AttachTextRaw(uint32_t player, const char* name) {
    __try {
        using Alloc = uint32_t(__cdecl*)(uint32_t, uint32_t);
        const uint32_t ctx = reinterpret_cast<Alloc>(base + kAllocRva)(0x30, 0x10);
        if (!ctx) return 0;
        unsigned char* c = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(ctx));
        const uint32_t vtable = static_cast<uint32_t>(base + kTextVtableRva), zero = 0;
        const float one = 1.0f;
        std::memcpy(c, &vtable, 4);
        std::memcpy(c + 4, &zero, 4);
        for (unsigned k = 0; k < 4; ++k) std::memcpy(c + 0x10 + 4 * k, &one, 4);
        std::memcpy(c + 0x20, &zero, 4);
        std::memcpy(c + 0x24, &one, 4);
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(base + kSetStringRva)(c, name);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kSetContextRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player + NameVfxOf(player))), ctx);
        return ctx;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
__declspec(noinline) bool HandleCallRaw(uint32_t rva, uint32_t handleAt) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + rva)(reinterpret_cast<void*>(static_cast<uintptr_t>(handleAt)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
__declspec(noinline) uint32_t ContextRaw(uint32_t handleAt) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + kGetContextRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(handleAt)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return 0;
    }
}
__declspec(noinline) bool RenameRaw(uint32_t ctx, const char* name) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(base + kSetStringRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(ctx)), name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
// The SafePtr at Player+0x1718: {target = entity+0x10, next link}; the old target
// is unlinked first (0x007429F0, thiscall(target, link)), as onGhostAdd does.
__declspec(noinline) bool LinkRaw(uint32_t link, uint32_t entity) {
    __try {
        const uint32_t target = entity ? entity + 0x10 : 0;
        uint32_t current = 0;
        Read(link, &current, 4);
        if (current == target) return true;
        if (current) reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kUnlinkRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(current)), link);
        WriteBytes(link, &target, 4);
        if (target) {
            uint32_t head = 0;
            Read(target + 4, &head, 4);
            WriteBytes(link + 4, &head, 4);
            WriteBytes(target + 4, &link, 4);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
__declspec(noinline) bool UpdateBotRaw(uint32_t helper, uint32_t player, bool& updated, bool& clear, bool& inView) {
    __try {
        bool u = false, c = false;
        float size = 1.0f;
        using Update = bool(__cdecl*)(uint32_t, uint32_t, bool, bool*, bool*, float*);
        inView = reinterpret_cast<Update>(base + kUpdateEntityRva)(helper, player, false, &u, &c, &size);
        updated = u;
        clear = c;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
__declspec(noinline) bool NameVisRaw(bool inView, uint32_t showMs, uint32_t hideMs, uint32_t player, bool& shown) {
    __try {
        using Vis = void(__cdecl*)(bool, uint32_t, uint32_t, uint32_t*, uint32_t*, bool*);
        reinterpret_cast<Vis>(base + kUpdateNameVisRva)(inView, showMs, hideMs, reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(player + kFirstSeen)),
                                                        reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(player + kLastSeen)), &shown);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}

// --- plates ---------------------------------------------------------------------------------
struct Plate {
    uint32_t player = 0, helper = 0, ctx = 0;
    bool bot = false, shown = true;
    unsigned key = 0;
    char name[24] = "";
};
constexpr unsigned kMaxPlates = 8;
Plate plates[kMaxPlates];
volatile LONG created = 0, notResident = 0, helperFailed = 0, vfxFailed = 0, textFailed = 0, killed = 0, renamed = 0, botUpdates = 0, lines = 0;
bool sitesOk = false, residentLogged = false;
DWORD lastScan = 0;
uint32_t levelSeen = 0;

void Line(const char* format, ...) {
    if (InterlockedIncrement(&lines) > 120) return;
    char buffer[300];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("nameplate: %s", buffer);
}
bool IsPlayer(uint32_t entity) {
    uint32_t vtable = 0;
    return entity && Read(entity, &vtable, 4) && vtable == base + placement::kPlayerPrimaryVtableRva;
}
bool Dead(uint32_t player) {
    uint32_t vtable = 0, fn = 0;
    if (!Read(player, &vtable, 4) || !Read(vtable + kIsDeadSlot, &fn, 4) || fn < base + 0x1000) return true;
    return bot::ThisBool0(fn, player) != 0;
}
bool IsBody(uint32_t entity) { return IsPlayer(entity) || IsNecroEntity(entity); }
// The name a Player gets, and whether it is one of our local bots.
bool NameFor(uint32_t player, char (&out)[24], bool& isBot, unsigned& key) {
    isBot = false;
    if (IsGhostNecro(player)) {
        const uint32_t machine = MachineOf(player);
        if (machine > 255) return false;
        key = machine;
        if (names[machine][0]) { std::snprintf(out, sizeof(out), "%s", names[machine]); return true; }
        if (machine < 0xF0) { std::snprintf(out, sizeof(out), "PLAYER %u", machine + 1); return true; }
        return false;
    }
    for (unsigned i = 0; i < bot::wantedBots && i < kMaxBots; ++i)
        if (bot::units[i].entity == player) {
            isBot = true;
            key = 0xF0 + i;
            Clean(bot::units[i].name, out, "BOT");
            return bots;
        }
    if (!IsGhostPlayer(player)) return false;
    const uint32_t machine = MachineOf(player);
    if (machine > 255) return false;
    key = machine;
    if (machine >= 0xF0 && !bots) return false;
    if (names[machine][0]) { std::snprintf(out, sizeof(out), "%s", names[machine]); return true; }
    if (machine < 0xF0) { std::snprintf(out, sizeof(out), "PLAYER %u", machine + 1); return true; }
    return false;   // a host bot whose name has not come yet
}
void Kill(Plate& p, const char* why) {
    const uint32_t player = p.player;
    if (player && IsBody(player)) {
        HandleCallRaw(kKillRva, player + NameVfxOf(player));
        uint32_t target = 0;
        Read(player + HelperPtrOf(player), &target, 4);
        if (target && target - 0x10 == p.helper && p.helper) turret::DeleteRaw(p.helper);
        LinkRaw(player + HelperPtrOf(player), 0);
    }
    InterlockedIncrement(&killed);
    Line("plate of %08X ('%s') removed: %s", player, p.name, why);
    p = Plate{};
}
void KillAll(const char* why) {
    for (Plate& p : plates) if (p.player) Kill(p, why);
}
Plate* Find(uint32_t player) {
    for (Plate& p : plates) if (p.player == player) return &p;
    return nullptr;
}
// Steps 1 to 4 for one Player.
void Create(uint32_t player, const char* name, bool isBot, unsigned key) {
    Plate* slot = nullptr;
    for (Plate& p : plates) if (!p.player) { slot = &p; break; }
    if (!slot) return;
    uint32_t sim = 0, edm = 0, existing = 0;
    Read(player + NameVfxOf(player), &existing, 4);
    if (existing || !Read(base + kSimManagerPtr, &sim, 4) || !sim) return;
    alignas(16) unsigned char info[0x40]{};
    Read(base + kSpawnFilterOwner, &edm, 4);
    const unsigned char off = 0;
    unsigned char was = 1;
    if (edm) { Read(edm + kEdmFilterByte, &was, 1); WriteBytes(edm + kEdmFilterByte, &off, 1); }
    mpmap::ownSpawn = true;
    necro::allowMp = true;
    const uint32_t helper = SpawnHelperRaw(sim, info);
    necro::allowMp = false;
    mpmap::ownSpawn = false;
    if (edm) WriteBytes(edm + kEdmFilterByte, &was, 1);
    uint32_t instance = 0;
    std::memcpy(&instance, info, 4);
    if (!instance) {
        InterlockedIncrement(&notResident);
        if (!residentLogged) {
            residentLogged = true;
            Line("the helper FXObject 58CA14A4-D14CE79C is not resident on this level (global_mp_assets is loaded on the MP maps only): no plates here");
        }
        return;
    }
    if (!helper) { InterlockedIncrement(&helperFailed); Line("helper spawn for %08X ('%s') refused", player, name); return; }
    const uint32_t handle = CreateVfxRaw(player, helper, VfxGuidFor(player));
    if (!handle) {
        InterlockedIncrement(&vfxFailed);
        turret::DeleteRaw(helper);
        Line("mp_name_text.vfb for %08X ('%s') gave no handle (not resident?); helper %08X deleted", player, name, helper);
        return;
    }
    const uint32_t ctx = AttachTextRaw(player, name);
    if (!ctx || ContextRaw(player + NameVfxOf(player)) != ctx) {
        // Never leave a name VFX without its text: that is the 0x0086AEDE crash.
        InterlockedIncrement(&textFailed);
        HandleCallRaw(kKillRva, player + NameVfxOf(player));
        turret::DeleteRaw(helper);
        Line("text context for %08X ('%s') failed (ctx %08X); VFX killed, helper deleted", player, name, ctx);
        return;
    }
    const float unmeasured = -1.0f;
    WriteBytes(player + NameWidthOf(player), &unmeasured, 4);
    LinkRaw(player + HelperPtrOf(player), helper);
    slot->player = player;
    slot->helper = helper;
    slot->ctx = ctx;
    slot->bot = isBot;
    slot->key = key;
    slot->shown = true;
    std::snprintf(slot->name, sizeof(slot->name), "%s", name);
    if (isBot) HandleCallRaw(kShowRva, player + kNameVfx);
    InterlockedIncrement(&created);
    Line("plate '%s' on %08X (%s, key %02X): helper %08X, VFX %08X, text %08X", name, player, isBot ? "local bot" : IsNecroEntity(player) ? "necro ghost" : "ghost", key,
         helper, handle, ctx);
}

// The Players to name: every PlayerManager slot but the local Isaac.
unsigned Targets(uint32_t* out, unsigned cap) {
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots || !Read(manager + 8, &capacity, 4) || !capacity ||
        capacity > 8)
        return 0;
    const uint32_t local = LocalSlotZeroEntity();
    unsigned n = 0;
    for (uint32_t i = 0; i < capacity && n < cap; ++i) {
        uint32_t bp = 0;
        if (!Read(slots + 4 * i, &bp, 4) || bp < 0x103C0) continue;
        const uint32_t entity = bp - 0x3C0;
        if (entity != local && (IsPlayer(entity) || IsGhostNecro(entity))) out[n++] = entity;
    }
    return n;
}

// A local bot: the helper follows its head and the visibility runs as for a ghost.
void UpdateBot(Plate& p, uint32_t showMs, uint32_t hideMs) {
    uint32_t target = 0;
    if (!Read(p.player + kHelperPtr, &target, 4) || target != p.helper + 0x10) return;
    bool updated = false, clear = false, inView = false;
    if (!UpdateBotRaw(p.helper, p.player, updated, clear, inView)) return;
    InterlockedIncrement(&botUpdates);
    if (!updated) return;
    bool shown = p.shown;
    if (showMs || hideMs) NameVisRaw(inView, showMs, hideMs, p.player, shown);
    else shown = inView;
    if (shown != p.shown) {
        HandleCallRaw(shown ? kShowRva : kHideRva, p.player + kNameVfx);
        p.shown = shown;
    }
}

void Tick(DWORD now) {
    if (!installed) return;
    const bool running = inLevel && mpmap::LmState(nullptr) == coop_mpmap::kRunning && !LevelChangePending();
    if (!running) {
        // Before the level goes, the plates the way HandleMPEndOfGame removes them;
        // once it is gone the Players' destructors took them, so they are only forgotten.
        bool any = false;
        for (const Plate& p : plates) any = any || p.player;
        if (any && inLevel) KillAll("the level is changing");
        else if (any) for (Plate& p : plates) p = Plate{};
        residentLogged = false;
        return;
    }
    SendNames(now);
    uint32_t settings = 0, attrs = 0, showMs = 0, hideMs = 0;
    if (Read(base + kPlayerSettingsRva, &settings, 4) && settings && Read(settings + 0xC, &attrs, 4) && attrs) {
        Read(attrs + kShowAfter, &showMs, 4);
        Read(attrs + kHideAfter, &hideMs, 4);
        if (showMs > 60000) showMs = 2000;
        if (hideMs > 60000) hideMs = 5000;
    }
    if (play::StoryCoop()) {
        bool any = false;
        for (const Plate& p : plates) any = any || p.player;
        if (any) KillAll("the story draws names with the overlay");
    }
    for (Plate& p : plates)
        if (p.player && p.bot) UpdateBot(p, showMs, hideMs);
    if (now - lastScan < 250) return;
    lastScan = now;
    // The native creator stays off: with MPLevelSettings present and no Blaze it
    // would leave a name VFX without its text (0x0086AEDE).
    necro::KeepNameplatesOff();
    uint32_t targets[8];
    const unsigned n = Targets(targets, 8);
    // Plates whose Player left the slots, died or changed hands.
    for (Plate& p : plates) {
        if (!p.player) continue;
        bool still = false;
        for (unsigned i = 0; i < n; ++i) still = still || targets[i] == p.player;
        uint32_t handle = 0;
        if (!still || !IsBody(p.player)) { p = Plate{}; continue; }   // the Player's dtor took its plate
        if (!Read(p.player + NameVfxOf(p.player), &handle, 4) || !handle) { Line("plate of %08X ('%s') gone (the game killed it)", p.player, p.name); p = Plate{}; continue; }
        if (Dead(p.player)) { Kill(p, "dead"); continue; }
        char name[24];
        bool isBot = false;
        unsigned key = 0;
        if (NameFor(p.player, name, isBot, key) && std::strcmp(name, p.name) != 0) {
            const uint32_t ctx = ContextRaw(p.player + NameVfxOf(p.player));
            if (ctx && RenameRaw(ctx, name)) {
                const float unmeasured = -1.0f;
                WriteBytes(p.player + NameWidthOf(p.player), &unmeasured, 4);
                Line("plate of %08X renamed '%s' -> '%s'", p.player, p.name, name);
                std::snprintf(p.name, sizeof(p.name), "%s", name);
                InterlockedIncrement(&renamed);
            }
        }
    }
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t player = targets[i];
        if (Find(player) || Dead(player)) continue;
        char name[24];
        bool isBot = false;
        unsigned key = 0;
        if (!NameFor(player, name, isBot, key)) continue;
        if (residentLogged) continue;   // not on this level
        // The story keeps the overlay's names even when a necro player brought global_mp_assets
        // in (16.10.: native plates showed up after a checkpoint restart).
        if (play::StoryCoop()) continue;
        Create(player, name, isBot, key);
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Nameplate", "Enabled", 1, ini.c_str()) != 0;
    bots = GetPrivateProfileIntA("Nameplate", "Bots", 1, ini.c_str()) != 0;
    char text[64]{};
    GetPrivateProfileStringA("Nameplate", "Name", "", text, sizeof(text), ini.c_str());
    Clean(text, ownName, "");
    if (!enabled) { std::fprintf(log, "nameplate off ([Nameplate] Enabled=0)\n"); return; }
    sitesOk = true;
    for (const Site& s : kSites) {
        unsigned char seen[12]{};
        const bool ok = Read(base + s.rva, seen, s.size) && std::memcmp(seen, s.bytes, s.size) == 0;
        if (!ok) { sitesOk = false; std::fprintf(log, "nameplate sig %-30s %08X MISMATCH\n", s.name, static_cast<unsigned>(base + s.rva)); }
    }
    uint32_t textSlot = 0;
    const bool vtableOk = Read(base + kTextVtableRva, &textSlot, 4) && textSlot >= base + 0x1000 && textSlot < base + hits::kCodeEnd;
    installed = sitesOk && vtableOk;
    std::fprintf(log, "nameplate %s: our name '%s', bots %s; the game's MP nameplate (mp_name_text.vfb) built without MPLevelSettings or Blaze, "
                      "only where global_mp_assets is resident (the MP maps)%s\n",
                 installed ? "on" : "OFF (a function did not match)", ownName[0] ? ownName : "PLAYER n", bots ? "named" : "not named",
                 vtableOk ? "" : "; text vtable MISMATCH");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = created + notResident * 3 + helperFailed * 5 + vfxFailed * 7 + textFailed * 11 + killed * 13 + renamed * 17 + namePacketsIn * 19 +
                     namesChanged * 23 + faults * 29;
    if (sum == last) return;
    last = sum;
    unsigned live = 0;
    for (const Plate& p : plates) live += p.player ? 1u : 0u;
    std::fprintf(log, "nameplate installed=%d live=%u created=%ld not_resident=%ld helper_failed=%ld vfx_failed=%ld text_failed=%ld killed=%ld renamed=%ld "
                      "bot_updates=%ld names_sent=%ld names_in=%ld names_changed=%ld faults=%ld own='%s'\n",
                 installed ? 1 : 0, live, InterlockedCompareExchange(&created, 0, 0), InterlockedCompareExchange(&notResident, 0, 0),
                 InterlockedCompareExchange(&helperFailed, 0, 0), InterlockedCompareExchange(&vfxFailed, 0, 0), InterlockedCompareExchange(&textFailed, 0, 0),
                 InterlockedCompareExchange(&killed, 0, 0), InterlockedCompareExchange(&renamed, 0, 0), InterlockedCompareExchange(&botUpdates, 0, 0),
                 InterlockedCompareExchange(&namePacketsSent, 0, 0), InterlockedCompareExchange(&namePacketsIn, 0, 0), InterlockedCompareExchange(&namesChanged, 0, 0),
                 InterlockedCompareExchange(&faults, 0, 0), ownName[0] ? ownName : "PLAYER n");
}
}  // namespace nameplate
