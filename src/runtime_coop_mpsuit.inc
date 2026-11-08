// Dead Space 2 United by Richard Hafer.
// The MP suits of the suit select in Versus and Mercenaries ([Loadout] ApplySuit;
// analysis/MP_SUITS_APPLY_2026-10-15.md). Until 15.10. every MP row wore the
// campaign Security suit as a stand-in.
//
// The MP security suit 45CAC626 is the only uniform of the MP UniformRegistry, which
// stays blocked with the rest of the MP player setup. Its record is kept (creator
// gate) and its run-time Uniform entity passes the spawn filter; RegisterUniform
// 0x00ED5DD0 adds it by hand once per visit. ChangeToUniform never registers: an
// unregistered GUID strips the worn suit and leaves the body bare.
//
// The skin (camo pattern and colour) is baked into the model when it is built:
// Player::GetMaterialSwapList (slot 0x01B29484 of the client vtable at player+0x58)
// runs SetupSkinOnPlayer only with a Blaze game. Our slot fills the swap pairs and the
// tint ourselves. The row's base stream (polivedrab, urbancamo1, desertcamo, red,
// black) must be loaded before the dress, or the body stays plain until its next one.
namespace mpsuit {
bool enabled = true;
const uint32_t kMpUniform[4] = {0x45CAC626, 0xB9BA135D, 0x43534147, 0x4E47494F};
constexpr uint32_t kUniformManager = 0x1C1C0DC, kSkinDbRva = 0x1C21428, kStreamManagerRva = 0x1C1C098, kSimManagerRva = 0x1C1C0A0;
constexpr uint32_t kRegisterUniform = 0xAD5DD0, kFindUniform = 0xAA0B90, kIsWaiting = 0xAA0B40, kLoadSkin = 0xAA0CD0, kStreamStatus = 0xDAA0;
constexpr uint32_t kGetComponent = 0x3CF080, kMscGetList = 0xAAEEF0, kAddHelper = 0xA9B640, kFindMaterial = 0x425160, kSetTint = 0xAC1180, kUnload = 0x2C1E0;
constexpr uint32_t kSwapSlot = 0x1729484, kSwapOriginal = 0x47D460, kMscId = 0x1C9EF62, kTintId = 0x1C9F5E2, kEmptyString = 0x1C46870;
// PlayerSkinDB entries (0xD0): +0x10 name hash, +0x34 RGB, +0x44 pairs (stride 0x20: from String, to String at +0x10), +0xC4 pairs, +0xC8 stream handle
constexpr uint32_t kSkinArray = 0x14, kSkinCount = 0x18, kEntrySize = 0xD0, kEntryHash = 0x10, kEntryRgb = 0x34, kEntryPairs = 0x44, kEntryPairCount = 0xC4,
                   kEntryHandle = 0xC8, kEntryTintOnly = 0xCC, kPlayerUniform = 0x1688, kStockRows = 36;

struct Sig { const char* name; uint32_t rva; unsigned char bytes[16]; unsigned size; };
const Sig kSigs[] = {
    {"RegisterUniform", kRegisterUniform, {0x83, 0xEC, 0x24, 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x34, 0x57, 0x8B, 0xF1, 0xB3, 0x01, 0xE8}, 16},
    {"FindUniform", kFindUniform, {0x56, 0x8B, 0x71, 0x14, 0x83, 0xC8, 0xFF, 0x33, 0xD2, 0x85, 0xF6, 0x76, 0x43, 0x8B, 0x49, 0x10}, 16},
    {"IsPlayerWaitingForStream", kIsWaiting, {0x57, 0x8B, 0x79, 0x20, 0x32, 0xC0, 0x33, 0xD2, 0x85, 0xFF, 0x76, 0x3D, 0x53, 0x8B, 0x59, 0x1C}, 16},
    {"PlayerSkinDB::LoadSkin", kLoadSkin, {0x56, 0x8B, 0x71, 0x18, 0x33, 0xC0, 0x33, 0xD2, 0x85, 0xF6, 0x76, 0x42, 0x8B, 0x49, 0x14, 0x57}, 16},
    {"stream status", kStreamStatus, {0x8B, 0x44, 0x24, 0x04, 0x50, 0xE8, 0x91, 0x68, 0xFF, 0xFF, 0x85, 0xC0, 0x74, 0x06, 0x8B, 0x40}, 16},
    {"Base::GetComponent", kGetComponent, {0x8B, 0x51, 0x48, 0x33, 0xC0, 0x85, 0xD2, 0x74, 0x0F, 0x8B, 0x41, 0x44, 0x8B, 0x4C, 0x24, 0x04}, 16},
    {"MSC::GetMaterialSwapList", kMscGetList, {0x55, 0x57, 0x8B, 0xF9, 0x66, 0x83, 0x7F, 0x14, 0x00, 0x8D, 0x6F, 0x14, 0x75, 0x38, 0x53, 0x56}, 16},
    {"MSC::AddHelper", kAddHelper, {0x8B, 0x44, 0x24, 0x04, 0x53, 0x32, 0xDB, 0x55, 0x8B, 0xE9, 0x85, 0xC0, 0x74, 0x4D, 0x57, 0x8B}, 16},
    {"FindMaterialByName", kFindMaterial, {0xE9, 0x2B, 0x22, 0x03, 0x00}, 5},
    {"ShaderTint::SetTintColor", kSetTint, {0x0F, 0x57, 0xC9, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04, 0x0F, 0x2F, 0xC8, 0xF3, 0x0F, 0x10, 0x15}, 16},
    {"StreamManager::Unload", kUnload, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x50, 0x8B, 0xF1, 0xE8, 0x4E, 0x81, 0xFD, 0xFF, 0x85, 0xC0, 0x74}, 16},
    {"Player::GetMaterialSwapList", kSwapOriginal, {0x51, 0x53, 0x55, 0x8B, 0xD9, 0x56, 0x8D, 0x73, 0xA8, 0x33, 0xED, 0x85, 0xF6, 0x0F, 0x84, 0x8B}, 16},
};
bool sigsOk = false, slotIn = false;
uint32_t Ptr(uint32_t address) {
    uint32_t v = 0;
    return Read(address, &v, 4) ? v : 0;
}
uint32_t GuidArg() { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kMpUniform)); }

// The base entry whose stream a stock MP row needs (-1: none, war_torn), rows 0..35
// (dump 52736 entries, the stock lua and TOC).
int BaseRow(int row) {
    static const signed char kBase[kStockRows] = {0,  0,  0,  0,  0,  5,  5,  5,  5,  5,  10, 10, 10, 10, -1, 15, 16, 0,
                                                  5,  10, 0,  5,  10, 0,  5,  10, 0,  5,  10, 5,  10, 0,  0,  0,  10, 10};
    return row >= 0 && row < static_cast<int>(kStockRows) ? kBase[row] : -1;
}

// --- calls ---
__declspec(noinline) int CallInt1(uint32_t self, uint32_t fn, uint32_t arg) {
    __try {
        return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(base + fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(self)), arg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
__declspec(noinline) bool CallAddHelper(uint32_t msc, const char* from, const char* to) {
    __try {
        return reinterpret_cast<bool(__thiscall*)(void*, const char*, const char*)>(base + kAddHelper)(reinterpret_cast<void*>(static_cast<uintptr_t>(msc)), from, to);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) uint32_t CallFindMaterial(const char* name) {
    __try {
        return reinterpret_cast<uint32_t(__cdecl*)(const char*)>(base + kFindMaterial)(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
__declspec(noinline) bool CallSetTint(uint32_t tint, float r, float g, float b, bool noApply) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, float, float, float, bool)>(base + kSetTint)(reinterpret_cast<void*>(static_cast<uintptr_t>(tint)), r, g, b, noApply);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) uint32_t CallNoArg(uint32_t self, uint32_t fn) {
    __try {
        return reinterpret_cast<uint32_t(__thiscall*)(void*)>(base + fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(self)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uint32_t Component(uint32_t entity, uint32_t idByteRva) {
    uint8_t id = 0;
    if (!Read(base + idByteRva, &id, 1)) return 0;
    const int c = CallInt1(entity, kGetComponent, id);
    return c > 0 ? static_cast<uint32_t>(c) : 0;
}
int StreamStatus(uint32_t handle) {
    const uint32_t sm = Ptr(base + kStreamManagerRva);
    return sm && handle ? CallInt1(sm, kStreamStatus, handle) : -1;
}
uint32_t SkinEntry(int row) {
    const uint32_t db = Ptr(base + kSkinDbRva), array = db ? Ptr(db + kSkinArray) : 0, count = db ? Ptr(db + kSkinCount) : 0;
    return array && row >= 0 && static_cast<uint32_t>(row) < count ? array + static_cast<uint32_t>(row) * kEntrySize : 0;
}
const char* StringAt(uint32_t stringObject) {
    uint32_t text = 0;
    return Read(stringObject, &text, 4) && text ? reinterpret_cast<const char*>(static_cast<uintptr_t>(text)) : reinterpret_cast<const char*>(base + kEmptyString);
}

// --- the visit ---
volatile LONG registrations = 0, registerFailures = 0, skinLoads = 0, skinReady = 0, dressed = 0, dressFallbacks = 0, deferrals = 0;
volatile LONG swapCalls = 0, swapsFilled = 0, swapsLocal = 0, swapsGhost = 0, swapsNoRow = 0, swapsAlready = 0, swapsMissingMaterial = 0, helpersFailed = 0;
volatile LONG tintsSet = 0, ghostRefusals = 0, cancelled = 0, leftovers = 0;
uint32_t visitEpoch = 0;
int uniformIndex = -1;   // in the uniform manager, this visit
bool wantedSeen = false;
uint32_t handles[kStockRows]{};   // base streams we asked for this visit
unsigned lines = 0;
void Line(const char* format, ...) {
    if (++lines > 300) return;
    char buffer[260];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("mpsuit %s", buffer);
}
bool VisitWanted() {
    if (!enabled || !sigsOk || !loadout::enabled || !mpmap::InVisit() || arena::RunningNow()) return false;
    const LONG mode = InterlockedCompareExchange(&gamemode::mode, 0, 0);
    return mode == gamemode::Versus || mode == gamemode::Mercenaries;
}
// A campaign arena playing Mercenaries ([Loadout] ArenaSuit, user 16.10.). Everything the
// MP suit needs lives in global_mp_assets, which no campaign level loads, so the necro
// module loads that pool root for it (analysis/MPSUIT_ARENA_2026-10-16.md).
bool arenaSuit = true;
bool ArenaWanted() {
    return enabled && sigsOk && arenaSuit && loadout::enabled && arena::RunningNow() && loadout::ApplyMode() == 1 && necro::enabled && necro::poolMode != 0;
}
bool Wanted() { return VisitWanted() || ArenaWanted(); }
bool KeepRecord(const uint32_t* guid) { return guid && VisitWanted() && std::memcmp(guid, kMpUniform, 16) == 0; }
bool IsMpSuit(const uint32_t* guid) { return guid && std::memcmp(guid, kMpUniform, 16) == 0; }
// Registered in this level's uniform manager (it is rebuilt with every level).
bool Registered() {
    if (uniformIndex < 0) return false;
    const uint32_t mgr = Ptr(base + kUniformManager);
    return mgr && CallInt1(mgr, kFindUniform, GuidArg()) == uniformIndex;
}

// The local pick, when it is a stock MP row (0..35), else -1.
int LocalRow() {
    const uint32_t dgm = Ptr(base + menu::kGameManager);
    uint8_t f0 = 0xFF;
    return dgm && Read(dgm + loadout::kDgmSkin, &f0, 1) && f0 < kStockRows ? f0 : -1;
}
int RowOfMachine(uint32_t machine) {
    if (machine == machineIndex) return LocalRow();
    AcquireSRWLockShared(&mpnative::lock);
    const coop_mpnative::Entry* e = mpnative::roster.Find(machine);
    const int row = e && e->skin < kStockRows ? e->skin : -1;
    ReleaseSRWLockShared(&mpnative::lock);
    return row;
}
// The base stream of a row: loaded (3), loading, or asked for now.
bool BaseReady(int row) {
    if (row < 0) return false;
    const int baseRow = BaseRow(row);
    if (baseRow < 0) return true;   // war_torn: the plain suit and a tint
    const uint32_t entry = SkinEntry(baseRow);
    if (!entry) return false;
    uint32_t handle = 0;
    Read(entry + kEntryHandle, &handle, 4);
    int status = handle ? StreamStatus(handle) : -1;
    if (status == 3) return true;
    if (status < 0) {
        uint32_t hash = 0;
        const uint32_t db = Ptr(base + kSkinDbRva);
        if (!db || !Read(entry + kEntryHash, &hash, 4) || !hash) return false;
        const int h = CallInt1(db, kLoadSkin, hash);
        if (h <= 0) { Line("skin stream for row %d (base %d, hash %08X): LoadSkin gave %d", row, baseRow, hash, h); return false; }
        handle = static_cast<uint32_t>(h);
        WriteBytes(entry + kEntryHandle, &handle, 4);   // where LoadAllSkinStreams keeps it; UnloadSkinDB frees it
        handles[baseRow] = handle;
        InterlockedIncrement(&skinLoads);
        Line("skin stream for row %d: base row %d hash %08X, handle %08X asked", row, baseRow, hash, handle);
        status = StreamStatus(handle);
    }
    if (status == 3) InterlockedIncrement(&skinReady);
    return status == 3;
}
// Every partner's MP row has its base stream here (a ghost takes its skin at the model build).
bool PartnersReady() {
    if (!Registered()) return false;
    int rows[coop_mpnative::kMaxPlayers];
    unsigned n = 0;
    AcquireSRWLockShared(&mpnative::lock);
    for (unsigned i = 0; i < mpnative::roster.count && n < coop_mpnative::kMaxPlayers; ++i)
        if (mpnative::roster.entries[i].machine != machineIndex && mpnative::roster.entries[i].skin < kStockRows) rows[n++] = mpnative::roster.entries[i].skin;
    ReleaseSRWLockShared(&mpnative::lock);
    bool ready = true;
    for (unsigned i = 0; i < n; ++i) ready = BaseReady(rows[i]) && ready;   // asks for each missing one
    return ready;
}

void Register() {
    const uint32_t mgr = Ptr(base + kUniformManager), sim = Ptr(base + kSimManagerRva);
    if (!mgr || !sim) return;
    visitEpoch = mpmap::visit.epochs;
    std::memset(handles, 0, sizeof(handles));
    necro::SpawnInfo info{};
    const bool record = necro::GetSpawnInfo(sim, info, kMpUniform);
    const int ok = record ? CallInt1(mgr, kRegisterUniform, GuidArg()) & 0xFF : -1;
    uniformIndex = record ? CallInt1(mgr, kFindUniform, GuidArg()) : -1;
    InterlockedIncrement(uniformIndex >= 0 ? &registrations : &registerFailures);
    Line("visit %u: MP suit record %s, RegisterUniform %d, index %d", visitEpoch, record ? "kept" : "MISSING (the creator gate refused it?)", ok, uniformIndex);
    // a handle of the last visit that the level's UnloadSkinDB left behind
    const uint32_t db = Ptr(base + kSkinDbRva), array = db ? Ptr(db + kSkinArray) : 0, count = db ? Ptr(db + kSkinCount) : 0;
    for (uint32_t i = 0; array && i < count && i < kStockRows; ++i) {
        uint32_t h = 0;
        if (Read(array + i * kEntrySize + kEntryHandle, &h, 4) && h && StreamStatus(h) < 0) {
            const uint32_t zero = 0;
            WriteBytes(array + i * kEntrySize + kEntryHandle, &zero, 4);
        }
    }
}

// The local dress, asked by loadout::Suit through mpmap::GiveLoadout.
uint32_t pendingPlayer = 0;
DWORD pendingSince = 0;
// False when it could not dress yet (no manager, or the body still streams its last suit in).
bool DressNow(uint32_t player, const char* why) {
    const uint32_t mgr = Ptr(base + kUniformManager);
    if (!mgr) return false;
    if (CallInt1(mgr, kIsWaiting, player) & 0xFF) return false;
    const bool ok = coop_uniform::BotDress(player, kMpUniform);
    uint32_t uniform = 0;
    Read(player + kPlayerUniform, &uniform, 4);
    const bool waiting = (CallInt1(mgr, kIsWaiting, player) & 0xFF) != 0;
    if (!ok || (!uniform && !waiting)) {
        InterlockedIncrement(&dressFallbacks);
        const bool stand = coop_uniform::BotDress(player, loadout::kMpSuitStandIn);
        Line("dress %s: ChangeToUniform %s, uniform %08X waiting %d: BARE, the stand-in %s", why, ok ? "called" : "FAULTED", uniform, waiting ? 1 : 0,
             stand ? "goes on" : "FAILED too");
        return true;
    }
    InterlockedIncrement(&dressed);
    Line("dress %s: MP suit on player %08X (row %d), uniform %08X, %s", why, player, LocalRow(), uniform, waiting ? "streaming in" : "at once");
    return true;
}
// From loadout::Suit: true when it handled the MP row (dressed now or later).
bool AskDress(uint32_t player) {
    if (!Wanted() || !player) return false;
    if (ArenaWanted()) {
        // the arena's kit suit stays on until the pool, the record and the skin stream are in
        necro::DemandForSuit();
        if (!Registered() && necro::PoolStatus() == 3) Register();
        if (Registered() && BaseReady(LocalRow()) && DressNow(player, "at the arena kit")) return true;
        pendingPlayer = player;
        pendingSince = GetTickCount();
        InterlockedIncrement(&deferrals);
        Line("dress deferred (arena): pool status %d, registered %d, row %d; the kit's suit stays", necro::PoolStatus(), Registered() ? 1 : 0, LocalRow());
        return true;
    }
    if (!Registered() || mpmap::visit.epochs != visitEpoch) Register();
    if (!Registered()) return false;
    if (BaseReady(LocalRow()) && DressNow(player, "at the loadout")) return true;
    pendingPlayer = player;
    pendingSince = GetTickCount();
    InterlockedIncrement(&deferrals);
    Line("dress deferred: the skin stream of row %d is not in yet; the current suit stays", LocalRow());
    return true;
}

// --- the swap list ---
using SwapFn = uint32_t(__thiscall*)(void*);
uint32_t __fastcall OnGetMaterialSwapList(void* client, void*) {
    const uint32_t player = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(client)) - 0x58;
    InterlockedIncrement(&swapCalls);
    uint32_t worn[4]{};
    if (Wanted() && botnet::WornSuit(player, worn) && IsMpSuit(worn)) {
        const uint32_t msc = Component(player, kMscId);
        if (msc && !CallNoArg(msc, kMscGetList)) {
            const bool local = player == LocalCampaignPlayer() || player == LocalSlotZeroEntity();
            const int row = local ? LocalRow() : RowOfMachine(MachineOf(player));
            const uint32_t entry = SkinEntry(row);
            uint32_t pairs = 0;
            if (row < 0 || !entry || !Read(entry + kEntryPairCount, &pairs, 4)) {
                InterlockedIncrement(&swapsNoRow);
            } else {
                if (pairs > 4) pairs = 4;
                bool all = true;
                for (uint32_t i = 0; i < pairs && all; ++i)
                    all = CallFindMaterial(StringAt(entry + kEntryPairs + 0x20 * i)) && CallFindMaterial(StringAt(entry + kEntryPairs + 0x20 * i + 0x10));
                if (!all) {
                    InterlockedIncrement(&swapsMissingMaterial);   // the next model build tries again
                } else {
                    for (uint32_t i = 0; i < pairs; ++i)
                        if (!CallAddHelper(msc, StringAt(entry + kEntryPairs + 0x20 * i), StringAt(entry + kEntryPairs + 0x20 * i + 0x10))) InterlockedIncrement(&helpersFailed);
                    float rgb[3]{1.0f, 1.0f, 1.0f};
                    Read(entry + kEntryRgb, rgb, 12);
                    if (const uint32_t tint = Component(player, kTintId))
                        if (CallSetTint(tint, rgb[0], rgb[1], rgb[2], true)) InterlockedIncrement(&tintsSet);
                    InterlockedIncrement(&swapsFilled);
                    InterlockedIncrement(local ? &swapsLocal : &swapsGhost);
                    if (InterlockedCompareExchange(&swapsFilled, 0, 0) <= 12)
                        Line("skin on %s %08X: row %d, %u pair(s), tint %.2f %.2f %.2f", local ? "our Isaac" : "a partner", player, row, pairs, rgb[0], rgb[1], rgb[2]);
                }
            }
        } else if (msc) {
            InterlockedIncrement(&swapsAlready);
        }
    }
    return reinterpret_cast<SwapFn>(base + kSwapOriginal)(client);
}

// --- game thread ---
void Tick(DWORD now) {
    if (!enabled || !sigsOk) return;
    const bool wanted = Wanted(), inArena = ArenaWanted();
    static DWORD triedAt = 0;
    static bool arenaSeen = false;
    if (inArena && !arenaSeen)
        Line("arena: MP suit wanted (row %d, mode %d, pool mode %d, pool status %d)", LocalRow(), loadout::ApplyMode(), necro::poolMode, necro::PoolStatus());
    arenaSeen = inArena;
    if (inArena) {
        // the pool for the local MP row or a partner's
        bool anyMpRow = LocalRow() >= 0;
        AcquireSRWLockShared(&mpnative::lock);
        for (unsigned i = 0; i < mpnative::roster.count && !anyMpRow; ++i) anyMpRow = mpnative::roster.entries[i].skin < kStockRows;
        ReleaseSRWLockShared(&mpnative::lock);
        if (anyMpRow) necro::DemandForSuit();
        if (necro::PoolStatus() == 3 && !Registered() && now - triedAt >= 1000) {
            triedAt = now;
            Register();
        }
    } else if (wanted && mpmap::Arrived() && (mpmap::visit.epochs != visitEpoch || (!Registered() && now - triedAt >= 3000))) {
        triedAt = now;
        Register();
    }
    if (wanted && pendingPlayer) {
        // the arena waits for the 77 MB pool as well (30 s), a visit only for the skin stream (10 s)
        const DWORD limit = inArena ? 30000u : 10000u;
        if (pendingPlayer != LocalCampaignPlayer()) {
            Line("dress pending dropped: the body changed");
            pendingPlayer = 0;
        } else if (Registered() && BaseReady(LocalRow())) {
            if (DressNow(pendingPlayer, inArena ? "after the pool and the skin stream" : "after the skin stream")) pendingPlayer = 0;
        } else if (static_cast<LONG>(now - pendingSince) > static_cast<LONG>(limit)) {   // AskDress stamps fresh, maybe after `now`
            InterlockedIncrement(&dressFallbacks);
            Line("dress: row %d not ready in %lu s (pool %d, registered %d); the stand-in", LocalRow(), static_cast<unsigned long>(limit / 1000),
                 necro::PoolStatus(), Registered() ? 1 : 0);
            coop_uniform::BotDress(pendingPlayer, loadout::kMpSuitStandIn);
            pendingPlayer = 0;
        }
    }
    if (wantedSeen && !wanted) {
        // the visit is over: stop what still loads; loaded ones stay for UnloadSkinDB
        for (int i = 0; i < static_cast<int>(kStockRows); ++i) {
            if (!handles[i]) continue;
            const int status = StreamStatus(handles[i]);
            if (status >= 0 && status != 3) {
                const uint32_t sm = Ptr(base + kStreamManagerRva);
                CallInt1(sm, kUnload, handles[i]);
                const uint32_t entry = SkinEntry(i), zero = 0;
                if (entry) WriteBytes(entry + kEntryHandle, &zero, 4);
                InterlockedIncrement(&cancelled);
            }
            Line("visit over: skin handle %08X (row %d) status %d%s", handles[i], i, status, status >= 0 && status != 3 ? ", cancelled" : "");
        }
        std::memset(handles, 0, sizeof(handles));
        uniformIndex = -1;
        pendingPlayer = 0;
    }
    wantedSeen = wanted;
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Loadout", "ApplySuit", 1, ini.c_str()) != 0;
    arenaSuit = GetPrivateProfileIntA("Loadout", "ArenaSuit", 1, ini.c_str()) != 0;
    std::fprintf(log, "mpsuit arena: %s\n", arenaSuit ? "on ([Loadout] ArenaSuit=1): the MP suit in Mercenaries arenas, with the necro module's pool (77 MB)"
                                                       : "off ([Loadout] ArenaSuit=0)");
    if (!enabled) { std::fprintf(log, "mpsuit off ([Loadout] ApplySuit=0): MP rows wear the campaign Security suit\n"); return; }
    unsigned bad = 0;
    for (const Sig& s : kSigs) {
        unsigned char got[16]{};
        const bool ok = Read(base + s.rva, got, s.size) && !std::memcmp(got, s.bytes, s.size);
        if (!ok) { ++bad; std::fprintf(log, "mpsuit sig %s %08X MISMATCH\n", s.name, static_cast<unsigned>(base + s.rva)); }
    }
    sigsOk = !bad;
    if (sigsOk) slotIn = menu::SwapSlot(log, "Player::GetMaterialSwapList", kSwapSlot, kSwapOriginal, reinterpret_cast<void*>(OnGetMaterialSwapList));
    if (!slotIn) sigsOk = false;
    std::fprintf(log, "mpsuit %s: %u call sites checked, swap slot %s\n", sigsOk ? "on" : "OFF (MP rows keep the stand-in)", static_cast<unsigned>(std::size(kSigs)),
                 slotIn ? "ours" : "NOT taken");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = registrations * 3 + registerFailures * 5 + skinLoads * 7 + dressed * 11 + dressFallbacks * 13 + swapsFilled * 17 + swapsMissingMaterial * 19 +
                     ghostRefusals * 23 + cancelled * 29 + deferrals * 31 + swapsNoRow * 37;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "mpsuit registrations=%ld failures=%ld index=%d skin_loads=%ld ready_checks=%ld dressed=%ld deferred=%ld fallbacks=%ld | swap calls=%ld "
                      "filled=%ld (local %ld, partners %ld) already=%ld no_row=%ld missing_material=%ld helper_failures=%ld tints=%ld | ghost_refusals=%ld cancelled=%ld\n",
                 registrations, registerFailures, uniformIndex, skinLoads, skinReady, dressed, deferrals, dressFallbacks, swapCalls, swapsFilled, swapsLocal, swapsGhost,
                 swapsAlready, swapsNoRow, swapsMissingMaterial, helpersFailed, tintsSet, ghostRefusals, cancelled);
}
}  // namespace mpsuit
