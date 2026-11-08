// Dead Space 2 United by Richard Hafer.
// The debug menu ([Debug] in DS2United.ini): our own, drawn on the shop's overlay.
// Insert opens and closes it; A/D change the tab, W/S the row, Q/E jump a
// chapter in LEVELS, Space uses the row; the mouse selects, clicks and scrolls
// (runtime_coop_panelinput.inc). While it is open the game sees no keyboard,
// mouse or pad (focusrate::PanelHolds) and the numpad belongs to it
// (shop::foreignPanel): bot orders, turret and gadgets wait. The game keeps
// running.
// - LEVELS: every campaign checkpoint of the game's map.txt (generated_checkpoints.h,
//   guid names decoded and checked against the catalog). A second press loads it:
//   in a level the level script's own chapter change (level::RequestChange:
//   SetOverrideCheckpoint, ClearPersistent, StoreLevel, RequestLevelChange),
//   in the main menu the new game launch with the checkpoint override that the
//   arenas use, plus the chapter's start suit (map.txt /UNIFORM) and the plasma
//   cutter. With HoldSaves=1 saves and restart points are held until the main
//   menu: a jump never writes over the campaign save.
// - WEAPONS, SUPPLIES, SUITS: the supply terminal's own deliveries at price 0
//   (shop::BuyGun, BuySupply, BuySupplyDrop, BuySuit), so they need [Shop] Enabled=1.
// - CHEATS: god mode (HealthComponent::HandleDamage skipped for the local Isaac,
//   hooked on first use), infinite stasis, full health, stasis refill, credits,
//   power nodes.
namespace gamemode { int RequestRestart(); }   // runtime_coop_gamemode.inc
namespace debugmenu {
bool enabled = true, installed = false, holdSaves = true;
FILE* logOut = nullptr;   // the plugin log, kept open for the session
int key = VK_INSERT;
int32_t creditStep = 10000, nodeStep = 5;
constexpr uint32_t kConfirmMs = 3000, kStatusMs = 4000;
constexpr int kRowsShown = 10;

bool open = false, published = false;
coop_debug::Cursor cursor;
coop_debug::Confirm confirm;
coop_shop::Edge toggleEdge;
panelinput::Reader input;
coop_shop::View lastView{};
char status[96] = "";
uint8_t statusKind = coop_shop::kStatusInfo;
DWORD statusAt = 0, openedAt = 0, stasisAt = 0;
bool drawCheckDone = true;
LONG drawHooksAtOpen = 0, drawFramesAtOpen = 0, drawOffscreenAtOpen = 0;
volatile LONG notDrawn = 0, strayHolds = 0;
bool god = false, infiniteStasis = false;
volatile LONG godPlayer = 0;
// the kit after a launch from the main menu, and the save hold of a jump
struct Kit { bool armed = false; unsigned chapter = 0; uint32_t isaac = 0; DWORD seenAt = 0; };
Kit kit;
bool holdLevelSeen = false;
volatile LONG opens = 0, jumps = 0, jumpsFromMenu = 0, jumpsRefused = 0, gunsAsked = 0, suppliesAsked = 0, suitsAsked = 0, cheatsUsed = 0, godHits = 0,
              stasisRefills = 0, kitsGiven = 0, holdsReleased = 0, faults = 0, lines = 0;

void Line(const char* format, ...) {
    if (InterlockedIncrement(&lines) > 400) return;
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    gate::Event("debug %s", text);
}
void SetStatus(uint8_t kind, const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(status, sizeof(status), format, args);
    va_end(args);
    statusKind = kind;
    statusAt = GetTickCount();
}

// --- god mode: HealthComponent::HandleDamage (0x00F74D60, thiscall(component, damage),
// ret 4; both health component kinds end here, the bots' health floor sits on it too).
// Hooked when god mode is first switched on, so the retail bytes stay as long as
// nobody asks for it.
constexpr uint32_t kHandleDamageRva = 0xB74D60;
safetyhook::InlineHook damageHook;
bool damageHooked = false, damageTried = false;
void __fastcall OnHandleDamage(void* component, void*, const void* damage) {
    if (god) {
        const uint32_t c = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(component));
        uint32_t owner = 0;
        const uint32_t isaac = static_cast<uint32_t>(InterlockedCompareExchange(&godPlayer, 0, 0));
        if (isaac && Read(c + hits::kComponentOwner, &owner, 4) && owner == isaac) { InterlockedIncrement(&godHits); return; }
    }
    damageHook.thiscall<void>(component, damage);
}
bool HookDamage() {
    if (damageTried) return damageHooked;
    damageTried = true;
    // push ebp / mov ebp,esp / and esp,-16 / xorps xmm2,xmm2 / sub esp,74h / push ebx / push esi / mov esi,ecx
    static const unsigned char kSig[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x0F, 0x57, 0xD2, 0x83, 0xEC, 0x74, 0x53, 0x56, 0x8B, 0xF1};
    damageHooked = logOut && hook_site::Install(logOut,"debug HealthComponent::HandleDamage (god mode)", base + kHandleDamageRva, kSig, sizeof(kSig),
                                      reinterpret_cast<void*>(OnHandleDamage), damageHook);
    Line("god mode damage hook %s", damageHooked ? "installed" : "NOT installed");
    return damageHooked;
}

// --- cheats ----------------------------------------------------------------------------
bool RefillStasis(uint32_t player) {
    const uint32_t hc = mpmap::HealthComponent(player);
    float energy = 0.0f, max = 0.0f;
    if (!hc || !Read(hc + mpmap::kDhcEnergy, &energy, 4) || !Read(hc + mpmap::kDhcEnergyMax, &max, 4) || !std::isfinite(energy) || !(max > 0.0f) || max > 10000.0f)
        return false;
    if (energy >= max) return true;
    return mpmap::Have(mpmap::sAddEnergy) && mpmap::AddEnergyRaw(hc, max - energy);
}
bool FullHealth(uint32_t player, float& before, float& max) {
    const uint32_t hc = hits::FindHealthComponent(player);
    before = max = 0.0f;
    if (!hc || !Read(hc + hits::kHealthCurrent, &before, 4) || !Read(hc + hits::kHealthMax, &max, 4) || !(max > 0.0f) || !std::isfinite(max)) return false;
    return WriteBytes(hc + hits::kHealthCurrent, &max, 4);
}
void UseCheat(int row, uint32_t player) {
    if (row == coop_debug::kVoiceTest) {   // a test of the sound, no cheat: the progress stays clean
        if (!voices::installed) { SetStatus(coop_shop::kStatusBad, "Voices are off or have no pack (see the log)"); return; }
        const unsigned n = voices::TestAll();
        SetStatus(n ? coop_shop::kStatusGood : coop_shop::kStatusBad, n ? "%u character(s) speak, one after the other" : "Nobody here to speak", n);
        Line("voice test: %u character(s)", n);
        return;
    }
    InterlockedIncrement(&cheatsUsed);
    progress::Taint("a debug cheat");
    switch (row) {
    case coop_debug::kGodMode:
        if (!god && !HookDamage()) { SetStatus(coop_shop::kStatusBad, "God mode: the damage hook did not go in"); return; }
        god = !god;
        SetStatus(coop_shop::kStatusGood, "God mode %s", god ? "ON" : "OFF");
        Line("god mode %s", god ? "on" : "off");
        return;
    case coop_debug::kInfiniteStasis:
        infiniteStasis = !infiniteStasis;
        SetStatus(coop_shop::kStatusGood, "Infinite stasis %s", infiniteStasis ? "ON" : "OFF");
        Line("infinite stasis %s", infiniteStasis ? "on" : "off");
        return;
    case coop_debug::kShowEnemies: {
        const bool on = !InterlockedCompareExchange(&marks::showAll, 0, 0);
        InterlockedExchange(&marks::showAll, on ? 1 : 0);
        SetStatus(coop_shop::kStatusGood, "All enemies on the screen %s", on ? "ON" : "OFF");
        Line("show all enemies %s", on ? "on" : "off");
        return;
    }
    default:
        break;
    }
    if (!player) { SetStatus(coop_shop::kStatusBad, "No Isaac here"); return; }
    if (row == coop_debug::kFullHealth) {
        float before = 0.0f, max = 0.0f;
        const bool ok = FullHealth(player, before, max);
        SetStatus(ok ? coop_shop::kStatusGood : coop_shop::kStatusBad, ok ? "Health %.0f -> %.0f" : "Health not readable", before, max);
        Line("full health: %.0f -> %.0f %s", before, max, ok ? "ok" : "FAILED");
    } else if (row == coop_debug::kRefillStasis) {
        const bool ok = RefillStasis(player);
        SetStatus(ok ? coop_shop::kStatusGood : coop_shop::kStatusBad, ok ? "Stasis full" : "Stasis could not be refilled");
    } else if (row == coop_debug::kAddCredits) {
        const int32_t before = shop::Credits(player);
        const int32_t after = before < 0 ? -1 : static_cast<int32_t>(std::min<int64_t>(int64_t(before) + creditStep, 99999999));
        const bool ok = after >= 0 && WriteBytes(player + shop::kCredits, &after, 4);
        // not a credits find: the bots' share watch moves on with it
        if (ok && bot::leaderCreditsSeen >= 0) bot::leaderCreditsSeen += after - before;
        SetStatus(ok ? coop_shop::kStatusGood : coop_shop::kStatusBad, ok ? "Credits %d -> %d" : "Credits not readable", before, after);
        Line("credits %d -> %d %s", before, after, ok ? "ok" : "FAILED");
    } else if (row == coop_debug::kAddNodes) {
        int32_t before = -1;
        const bool read = Read(player + bot::kPlayerNodes, &before, 4) && before >= 0 && before < 10000;
        const int32_t after = read ? before + nodeStep : -1;
        const bool ok = read && WriteBytes(player + bot::kPlayerNodes, &after, 4);
        SetStatus(ok ? coop_shop::kStatusGood : coop_shop::kStatusBad, ok ? "Power nodes %d -> %d" : "Power nodes not readable", before, after);
        Line("power nodes %d -> %d %s", before, after, ok ? "ok" : "FAILED");
    }
}

// --- the HOST tab -------------------------------------------------------------------------
// The director's tools go to the randomizer half (DS2Survival_Admin, run on its tick);
// the player tools are applied here and sent to every partner (SVNT kind 12), who
// apply them to their own Isaac. A client sees the tab but cannot use it.
using AdminFn = int(__cdecl*)(int, int);
using AdminInfoFn = int(__cdecl*)(char*, int, char*, int);
using AdminFamilyFn = int(__cdecl*)(int, char*, int);
AdminFn adminFn = nullptr;
AdminInfoFn adminInfoFn = nullptr;
DWORD adminLookAt = 0, adminInfoAt = 0;
int familyCount = 0, adminSeq = -1;
char familyNames[24][24]{};
char directorInfo[96] = "";
coop_debug::HostValues hostValues;
bool godAll = false;
volatile LONG hostTools = 0, hostRefused = 0, everyoneSent = 0, everyoneApplied = 0, killAllRuns = 0, killAllKilled = 0;
void LookUpDirector(DWORD now) {
    if (adminFn || now - adminLookAt < 3000) return;
    adminLookAt = now;
    const HMODULE module = ds2coop::PartnerModule("DS2RandomizerObserver.asi");
    if (!module) return;
    adminFn = reinterpret_cast<AdminFn>(GetProcAddress(module, "DS2Survival_Admin"));
    adminInfoFn = reinterpret_cast<AdminInfoFn>(GetProcAddress(module, "DS2Survival_AdminInfo"));
    const auto familyFn = reinterpret_cast<AdminFamilyFn>(GetProcAddress(module, "DS2Survival_AdminFamily"));
    if (familyFn) {
        familyCount = std::min(familyFn(-1, nullptr, 0), static_cast<int>(std::size(familyNames)));
        for (int i = 0; i < familyCount; ++i) familyFn(i, familyNames[i], sizeof(familyNames[i]));
    }
    Line("host tools: director exports %s (info %s, %d kinds)", adminFn ? "found" : "MISSING", adminInfoFn ? "found" : "MISSING", familyCount);
}
// The director's line and its newest result, four times a second while the tab is up.
void PollDirector(DWORD now) {
    if (!adminInfoFn || now - adminInfoAt < 250) return;
    adminInfoAt = now;
    char result[96] = "";
    const int seq = adminInfoFn(directorInfo, sizeof(directorInfo), result, sizeof(result));
    if (adminSeq >= 0 && seq != adminSeq && result[0])
        SetStatus(result[0] == '!' ? coop_shop::kStatusBad : coop_shop::kStatusGood, "%s", result[0] == '!' ? result + 1 : result);
    adminSeq = seq;
}
void AskDirector(int command, int value, const char* what) {
    InterlockedIncrement(&hostTools);
    if (!adminFn) { InterlockedIncrement(&hostRefused); SetStatus(coop_shop::kStatusBad, "The director's tools are missing (randomizer half)"); return; }
    const bool queued = adminFn(command, value) == 1;
    if (!queued) InterlockedIncrement(&hostRefused);
    SetStatus(queued ? coop_shop::kStatusInfo : coop_shop::kStatusBad, queued ? "%s ..." : "%s: the director did not take it", what);
    Line("host tool %s (command %d value %d): %s", what, command, value, queued ? "asked" : "REFUSED");
}

// Every hostile NPC of the level, bosses and the Ubermorph aside: health to 0 through
// the verified SetHealth slot. The host owns the enemies; their deaths reach the partners.
unsigned KillAllEnemies(unsigned& seen) {
    uint32_t sim = 0;
    unsigned killed = 0;
    seen = 0;
    if (!Read(base + 0x1C1C0A0, &sim, 4) || !sim) return 0;
    for (uint32_t b = 0; b < 4096; ++b) {
        uint32_t node = 0;
        if (!Read(sim + 0x70 + b * 4, &node, 4)) break;
        for (unsigned guard = 0; node && guard < 64; ++guard) {
            uint32_t object = 0, vtable = 0;
            if (Read(node + 0x40, &object, 4) && object && Read(object, &vtable, 4)) {
                const auto kind = bot_sense::KindOf(vtable);
                if (kind != coop_bot::EnemyKind::Unknown && kind != coop_bot::EnemyKind::Boss && kind != coop_bot::EnemyKind::Hunter &&
                    !IsPlayerOrNecroEntity(object) && !phantom::Is(object)) {
                    ++seen;
                    const uint32_t hc = hits::FindHealthComponent(object);
                    float health = 0.0f;
                    if (hc && Read(hc + hits::kHealthCurrent, &health, 4) && health > 0.0f && hits::CallSetHealth(hc, 0.0f)) ++killed;
                }
            }
            if (!Read(node + 0x34, &node, 4)) break;
        }
    }
    return killed;
}

void GiveSupply(int row, uint32_t player);   // below
void Load(int row, bool fromHost);   // below
volatile LONG jumpsSent = 0, jumpsFollowed = 0, jumpsMismatched = 0, clientJumpsRefused = 0;
// One machine's share of a host tool for everyone: its own Isaac (and its bots).
void ApplyEveryone(int command, int value, const float* hostPos, bool fromHost) {
    InterlockedIncrement(&everyoneApplied);
    const uint32_t player = LocalCampaignPlayer();
    const char* who = fromHost ? "the host" : "you";
    switch (command) {
    case coop_debug::kCmdJump: {
        if (!fromHost) return;   // the host loads it himself
        uint32_t guid[3]{};
        if (hostPos) std::memcpy(guid, hostPos, sizeof(guid));
        const bool known = value >= 0 && value < static_cast<int>(std::size(coop_debug::kCheckpoints)) &&
                           std::memcmp(coop_debug::kCheckpoints[value].guid, guid, sizeof(guid)) == 0;
        if (!known) {
            InterlockedIncrement(&jumpsMismatched);
            Line("host jump to checkpoint row %d (%08X-%08X-%08X) NOT followed: this build has another checkpoint there", value, guid[0], guid[1], guid[2]);
            return;
        }
        InterlockedIncrement(&jumpsFollowed);
        Line("host jump to checkpoint row %d: followed here", value);
        Load(value, true);
        return;
    }
    case coop_debug::kCmdHeal: {
        float before = 0.0f, max = 0.0f;
        const bool ok = player && FullHealth(player, before, max);
        unsigned bots = 0;
        for (const uint32_t b : botEntities) { float bb = 0.0f, bm = 0.0f; if (b && FullHealth(b, bb, bm)) ++bots; }
        Line("host tool heal (from %s): Isaac %08X %.0f -> %.0f %s, %u bot(s)", who, player, before, max, ok ? "ok" : "FAILED", bots);
        notice::Show("HEALED BY THE HOST");
        return;
    }
    case coop_debug::kCmdRevive: {
        const DWORD t = GetTickCount();
        InterlockedExchange(&respawn::reviveAskedAt, static_cast<LONG>(t ? t : 1));
    }
        Line("host tool revive (from %s): asked, respawn stage %d", who, respawn::stage);
        return;
    case coop_debug::kCmdSupply: {
        const bool stasis = player && RefillStasis(player);
        if (player && shop::installed && shop::deliveryOk) GiveSupply(coop_shop::kGunCount + 4, player);
        Line("host tool supply drop (from %s): stasis %s, drop %s", who, stasis ? "full" : "not refilled",
             player && shop::installed && shop::deliveryOk ? "asked" : "not possible (no Isaac or no shop)");
        notice::Show("SUPPLIES FROM THE HOST");
        return;
    }
    case coop_debug::kCmdCredits: {
        const int32_t before = player ? shop::Credits(player) : -1;
        const int32_t after = before < 0 ? -1 : static_cast<int32_t>(std::min<int64_t>(int64_t(before) + (value > 0 ? value : 0), 99999999));
        const bool ok = after >= 0 && WriteBytes(player + shop::kCredits, &after, 4);
        if (ok && bot::leaderCreditsSeen >= 0) bot::leaderCreditsSeen += after - before;
        Line("host tool credits +%d (from %s): %d -> %d %s", value, who, before, after, ok ? "ok" : "FAILED");
        if (ok) notice::Show("CREDITS FROM THE HOST");
        return;
    }
    case coop_debug::kCmdGather:
        if (!fromHost || !hostPos) return;   // the host stays where he is
        Line("host tool gather: to (%.1f %.1f %.1f) %s", hostPos[0], hostPos[1], hostPos[2], catchup::PullTo(hostPos, "the host's gather") ? "pulled" : "FAILED");
        return;
    case coop_debug::kCmdGod:
        if (value && !HookDamage()) { Line("host tool god mode: the damage hook did not go in"); return; }
        god = value != 0;
        Line("host tool god mode %s (from %s)", god ? "on" : "off", who);
        notice::Show(god ? "GOD MODE ON (HOST)" : "GOD MODE OFF (HOST)");
        return;
    default:
        Line("host tool: unknown command %d", command);
        return;
    }
}
// A partner's copy, from the side channel's thread: applied on the next tick.
struct AdminAsk { int command, value; float pos[3]; };
SRWLOCK adminLock = SRWLOCK_INIT;
AdminAsk adminAsks[8]{};
unsigned adminAskCount = 0;
void OnAdmin(int command, int value, const float* pos) {
    progress::Taint("a host tool");
    AcquireSRWLockExclusive(&adminLock);
    if (adminAskCount < std::size(adminAsks)) {
        AdminAsk& a = adminAsks[adminAskCount++];
        a = AdminAsk{command, value, {}};
        if (pos) std::memcpy(a.pos, pos, sizeof(a.pos));
    }
    ReleaseSRWLockExclusive(&adminLock);
}
void DrainAdmin() {
    AdminAsk asks[8];
    unsigned n = 0;
    AcquireSRWLockExclusive(&adminLock);
    n = adminAskCount;
    std::copy_n(adminAsks, n, asks);
    adminAskCount = 0;
    ReleaseSRWLockExclusive(&adminLock);
    for (unsigned i = 0; i < n; ++i) ApplyEveryone(asks[i].command, asks[i].value, asks[i].pos, true);
}
void UseHost(int row, uint32_t player, DWORD now) {
    using namespace coop_debug;
    if (NetRole() == 2) { SetStatus(coop_shop::kStatusBad, "Only the host can use these"); return; }
    if (HostRowNeedsConfirm(row) && !confirm.Press(kHost, row, now, kConfirmMs)) {
        SetStatus(coop_shop::kStatusInfo, "Space or click again: %s", HostRowName(row));
        return;
    }
    if (row != kStartRun && row != kRestartRun && row != kRestartCheckpoint) progress::Taint("a host tool");
    switch (row) {
    case kStartRun: AskDirector(1, 0, "Start the run"); return;
    case kJumpWave: { char what[32]; std::snprintf(what, sizeof(what), "Jump to wave %d", hostValues.wave); AskDirector(2, hostValues.wave, what); return; }
    case kEndWave: AskDirector(3, 0, "End the wave"); return;
    case kSpawnEnemy: {
        char what[48];
        std::snprintf(what, sizeof(what), "Spawn %s", hostValues.family < familyCount ? familyNames[hostValues.family] : "?");
        AskDirector(4, hostValues.family, what);
        return;
    }
    case kHoldDirector: AskDirector(5, std::strstr(directorInfo, "HOLD") ? 0 : 1, "Director hold"); return;
    case kMaxAlive: AskDirector(6, hostValues.maxAlive, "Max alive"); return;
    case kRestartRun: AskDirector(7, 0, "Restart the run"); return;
    case kKillAll: {
        InterlockedIncrement(&hostTools);
        InterlockedIncrement(&killAllRuns);
        unsigned seen = 0;
        const unsigned killed = KillAllEnemies(seen);
        InterlockedExchangeAdd(&killAllKilled, static_cast<LONG>(killed));
        SetStatus(killed ? coop_shop::kStatusGood : coop_shop::kStatusInfo, "Killed %u of %u enemies", killed, seen);
        Line("host tool kill all: %u of %u hostile NPCs set to 0 health (bosses and the Ubermorph left)", killed, seen);
        return;
    }
    case kRestartCheckpoint: {
        InterlockedIncrement(&hostTools);
        const bool ok = gamemode::RequestRestart() == 1;
        SetStatus(ok ? coop_shop::kStatusGood : coop_shop::kStatusBad, ok ? "Checkpoint restart asked; the partners follow" : "The restart site did not verify");
        Line("host tool checkpoint restart: %s", ok ? "asked" : "REFUSED (site not verified)");
        return;
    }
    default:
        break;
    }
    const int command = EveryoneCommandFor(row);
    if (!command) return;
    InterlockedIncrement(&hostTools);
    float pos[3]{};
    if (player) Read(player + placement::kPlayerPosition, pos, 12);
    const int value = row == kCreditsAll ? creditStep : row == kGodAll ? (godAll ? 0 : 1) : 0;
    if (row == kGodAll) godAll = value != 0;
    ApplyEveryone(command, value, pos, false);
    const unsigned partners = peerTable.Count();
    if (network && partners) { survnet::QueueAdmin(command, value, pos); InterlockedIncrement(&everyoneSent); }
    SetStatus(coop_shop::kStatusGood, "%s: you%s", HostRowName(row), partners ? " and every partner" : " (no partners)");
}
void HostRowText(int row, DWORD now, coop_shop::Row& r, char* detail, size_t detailSize) {
    using namespace coop_debug;
    std::snprintf(r.label, sizeof(r.label), "%s", HostRowName(row));
    const bool armed = HostRowNeedsConfirm(row) && confirm.Armed(kHost, row, now, kConfirmMs);
    switch (row) {
    case kJumpWave: std::snprintf(r.right, sizeof(r.right), armed ? "AGAIN: WAVE %d" : "WAVE %d", hostValues.wave); break;
    case kSpawnEnemy: std::snprintf(r.right, sizeof(r.right), "%s", hostValues.family < familyCount ? familyNames[hostValues.family] : "?"); break;
    case kHoldDirector: std::snprintf(r.right, sizeof(r.right), "%s", std::strstr(directorInfo, "HOLD") ? "ON" : "OFF"); break;
    case kMaxAlive:
        if (hostValues.maxAlive) std::snprintf(r.right, sizeof(r.right), "SET %d", hostValues.maxAlive);
        else std::snprintf(r.right, sizeof(r.right), "SET AUTO");
        break;
    case kCreditsAll: std::snprintf(r.right, sizeof(r.right), "+%d", creditStep); break;
    case kGodAll: std::snprintf(r.right, sizeof(r.right), "%s", godAll ? "ON" : "OFF"); break;
    default: std::snprintf(r.right, sizeof(r.right), "%s", armed ? "AGAIN" : "USE"); break;
    }
    static const char* const kDetails[kHostRowCount] = {
        "Skips the start delay of the armed run",
        "Q/E change the wave (Shift: 5); this wave's enemies die, no rewards, no record",
        "In a wave: its enemies die, rewards drop. In the break: the next wave now",
        "Q/E pick the kind; it comes as the wave's next enemy (a stand-in if it cannot)",
        "Survival waves: no spawns, the start delay, wave timer and break stand still",
        "Q/E change, Space sets it; AUTO = the INI's (max 10 without the larger NavPower pool)",
        "Ends this run, the next one arms at once",
        "Every necromorph in the level except bosses and the Ubermorph",
        "Every player and bot to full health",
        "MP maps: whoever waits for a respawn comes back now",
        "Ammo for every gun at each player's feet and full stasis",
        "Added to every player's credits",
        "Every partner is put where you stand",
        "No damage reaches any player's health (scripted deaths still kill)",
        "The pause menu's checkpoint restart; the partners follow"};
    std::snprintf(detail, detailSize, "%s", kDetails[row]);
    if (NetRole() == 2) { r.style = coop_shop::kRowUnavailable; std::snprintf(r.right, sizeof(r.right), "HOST ONLY"); return; }
    if (HostRowIsDirector(row) && !adminFn) { r.style = coop_shop::kRowUnavailable; std::snprintf(r.right, sizeof(r.right), "N/A"); }
}

// --- deliveries through the supply terminal ---------------------------------------------
coop_shop::Item Free(coop_shop::Kind kind, int ref, const char* label) {
    coop_shop::Item it;
    it.kind = kind;
    it.ref = ref;
    it.price = 0;
    it.available = true;
    std::snprintf(it.label, sizeof(it.label), "%s", label);
    return it;
}
const char* ShopMissing(bool what) {
    if (!shop::installed) return "Needs [Shop] Enabled=1";
    return what ? nullptr : "A shop function did not match";
}
void GiveGun(int idx, uint32_t player) {
    InterlockedIncrement(&gunsAsked);
    progress::Taint("a free gun");
    if (const char* why = ShopMissing(shop::gunsOk)) { SetStatus(coop_shop::kStatusBad, "%s", why); return; }
    if (shop::pendingGun.idx >= 0) { SetStatus(coop_shop::kStatusBad, "Still waiting for the %s", coop_shop::kGuns[shop::pendingGun.idx].name); return; }
    shop::BuyGun(Free(coop_shop::Kind::Gun, idx, coop_shop::kGuns[idx].name), player, shop::Credits(player));
}
// SUPPLIES rows: ammo for every gun, then three med packs, a stasis pack and a supply drop.
constexpr int kSupplyExtras = 5;
int SupplyCount() { return coop_shop::kGunCount + kSupplyExtras; }
void SupplyLabel(int row, char* out, size_t size) {
    static const char* const kExtras[kSupplyExtras] = {"Small med pack", "Medium med pack", "Large med pack", "Stasis pack", "Supply drop (ammo for every gun)"};
    if (row < coop_shop::kGunCount && !coop_shop::HasAmmo(coop_shop::kGuns[row])) std::snprintf(out, size, "%s (needs no ammo)", coop_shop::kGuns[row].name);
    else if (row < coop_shop::kGunCount) std::snprintf(out, size, "%s ammo", coop_shop::kGuns[row].name);
    else std::snprintf(out, size, "%s", kExtras[row - coop_shop::kGunCount]);
}
void GiveSupply(int row, uint32_t player) {
    InterlockedIncrement(&suppliesAsked);
    progress::Taint("free supplies");
    if (const char* why = ShopMissing(shop::deliveryOk)) { SetStatus(coop_shop::kStatusBad, "%s", why); return; }
    char label[48];
    SupplyLabel(row, label, sizeof(label));
    if (row < coop_shop::kGunCount && !coop_shop::HasAmmo(coop_shop::kGuns[row])) { SetStatus(coop_shop::kStatusInfo, "%s", label); return; }
    if (row < coop_shop::kGunCount) { shop::BuySupply(Free(coop_shop::Kind::Ammo, row, label), player, shop::Credits(player)); return; }
    static const coop_shop::Kind kKinds[kSupplyExtras] = {coop_shop::Kind::MedSmall, coop_shop::Kind::MedMedium, coop_shop::Kind::MedLarge,
                                                          coop_shop::Kind::StasisPack, coop_shop::Kind::SupplyDrop};
    const coop_shop::Kind kind = kKinds[row - coop_shop::kGunCount];
    if (kind == coop_shop::Kind::SupplyDrop) shop::BuySupplyDrop(Free(kind, -1, label), player, shop::Credits(player));
    else shop::BuySupply(Free(kind, -1, label), player, shop::Credits(player));
}
void GiveSuit(int idx, uint32_t player) {
    InterlockedIncrement(&suitsAsked);
    progress::Taint("a free suit");
    if (const char* why = ShopMissing(shop::suitsOk)) { SetStatus(coop_shop::kStatusBad, "%s", why); return; }
    shop::BuySuit(Free(coop_shop::Kind::Suit, idx, bot::kSuits[idx].name), player, shop::Credits(player));
}

// --- checkpoint jumps ------------------------------------------------------------------------
bool InMainMenu() {
    char map[64] = "", stream[64] = "";
    return !level::CurrentMap(map, sizeof(map), stream, sizeof(stream)) || _strnicmp(map, "frontend", 8) == 0;
}
void ArmJacketGuard(const char* chapter, const char* name, uint32_t isaacNow);   // below
// The whole campaign is one map (ds_2), so the level follow never takes a jump within it:
// the host's jump goes to every partner as a host tool, and a connected client's own
// jump is refused (user, 19.10.: both had to jump by hand).
void Load(int row, bool fromHost) {
    const auto& cp = coop_debug::kCheckpoints[row];
    const auto& ch = coop_debug::kChapters[cp.chapter];
    char label[64];
    coop_debug::CheckpointLabel(static_cast<unsigned>(row), label, sizeof(label));
    const bool menu = InMainMenu();
    const bool connectedClient = NetRole() == 2 && peerTable.Count();
    const char* refusal = mpmap::InVisit() ? "On an MP map visit: go back first"
                        : LevelChangePending() ? "A level change is already pending"
                        : connectedClient && !fromHost ? "Only the host jumps; the partners follow"
                        : nullptr;
    if (connectedClient && !fromHost) InterlockedIncrement(&clientJumpsRefused);
    if (refusal) {
        InterlockedIncrement(&jumpsRefused);
        SetStatus(coop_shop::kStatusBad, "%s", refusal);
        Line("jump to %s REFUSED: %s", label, refusal);
        return;
    }
    if (arena::state != arena::State::Off) arena::End("a debug menu checkpoint jump");
    bool ok = false;
    if (menu) {
        ok = mpmap::StartCampaignFromMenu("ds_2", ch.stream, cp.guid);
        if (ok) kit = Kit{true, cp.chapter, 0, 0};
    } else {
        level::Packet p{};
        std::snprintf(p.map, sizeof(p.map), "ds_2");
        std::snprintf(p.stream, sizeof(p.stream), "%s", ch.stream);
        std::memcpy(p.checkpoint, cp.guid, 16);
        if (holdSaves) mpmap::InstallHooks();
        ok = level::RequestChange(p);
    }
    if (ok && holdSaves) { mpmap::debugHold = true; holdLevelSeen = false; }
    if (ok) ArmJacketGuard(ch.key, cp.name, menu ? 0 : LocalCampaignPlayer());
    InterlockedIncrement(ok ? (menu ? &jumpsFromMenu : &jumps) : &jumpsRefused);
    Line("jump to %s (%s, checkpoint %08X-%08X-%08X-%08X%s) from the %s: %s; saves %s", label, ch.key, cp.guid[0], cp.guid[1], cp.guid[2], cp.guid[3],
         cp.inCatalog ? "" : ", not in the catalog", menu ? "main menu (new game launch)" : "level (the script's chapter change)", ok ? "requested" : "FAILED",
         ok && holdSaves ? "held until the main menu" : "not held");
    if (!ok) { SetStatus(coop_shop::kStatusBad, "The load was refused"); return; }
    if (!fromHost && NetRole() == 1 && peerTable.Count()) {
        float words[3]{};
        std::memcpy(words, cp.guid, sizeof(words));
        survnet::QueueAdmin(coop_debug::kCmdJump, row, words);
        InterlockedIncrement(&jumpsSent);
        InterlockedIncrement(&everyoneSent);
        Line("jump to %s sent to %u partner(s)", label, peerTable.Count());
    }
    SetStatus(coop_shop::kStatusGood, "Loading %s", label);
    char text[96];
    std::snprintf(text, sizeof(text), "LOADING %s", label);
    for (char* c = text; *c; ++c) *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    notice::Show(text);
}
// --- the straitjacket after a jump --------------------------------------------------------------
// PLAYERSTATE_STRAIT_JACKET (case 2 of PlayerChangeState::Activate 0x00864380) is player flag 0x26
// in the bit set at Player+0x3DC. The debug starts of chapter 1's straitjacket rooms set it, the
// script that ends it runs between those rooms, and the flag outlives a level load: after a jump
// onward Isaac and the bots stayed in the straitjacket run (17.10.: the storage suit room, then
// chapter 2). After a jump to any other checkpoint ClearPlayerFlag 0x0085F790 (thiscall(flag),
// ret 4, the call the game's own "stop" makes) clears it on every player, for 15 s from the new
// level's start, so bots that spawn later are covered too.
constexpr uint32_t kClearPlayerFlagRva = 0x45F790, kPlayerFlags = 0x3DC, kStraitJacketFlag = 0x26;
// mov edx,ecx / push esi / mov esi,[esp+8] / mov ecx,esi / and ecx,1Fh / mov eax,1
const unsigned char kClearPlayerFlagBytes[] = {0x8B, 0xD1, 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0xCE, 0x83, 0xE1, 0x1F, 0xB8, 0x01, 0x00, 0x00, 0x00};
struct JacketGuard { bool armed = false, sawLoad = false; uint32_t isaac = 0; DWORD since = 0; };
JacketGuard jacket;
int jacketSite = -1;   // -1 unchecked, 0 bytes differ, 1 verified
volatile LONG jacketJumps = 0, jacketKept = 0, jacketCleared = 0, jacketFaults = 0;
bool StraitJacketCheckpoint(const char* chapter, const char* name) {
    // the checkpoints whose debug start sends msg_start_strait_jacket_mode (ch01_hos MultiManagers)
    static const char* const kRooms[] = {"initial", "psych_hall", "saferoom", "observation_hall"};
    static const char* const kScenes[] = {"igc_01_01", "igc_01_02", "horror_hallway_ses", "igc_01_03", "se_01_01", "se_01_02", "vm_01_01"};
    if (_stricmp(chapter, "ch01_hos") == 0) {
        for (const char* r : kRooms) if (_stricmp(name, r) == 0) return true;
    } else if (_stricmp(chapter, "Ch01_SE_Checkpoints") == 0) {
        for (const char* r : kScenes) if (_stricmp(name, r) == 0) return true;
    }
    return false;
}
void ArmJacketGuard(const char* chapter, const char* name, uint32_t isaacNow) {
    InterlockedIncrement(&jacketJumps);
    if (StraitJacketCheckpoint(chapter, name)) {
        InterlockedIncrement(&jacketKept);
        jacket = JacketGuard{};
        Line("straitjacket: %s %s is a straitjacket checkpoint, the flag is the level's", chapter, name);
        return;
    }
    jacket = JacketGuard{true, false, isaacNow, 0};
}
__declspec(noinline) bool ClearJacketFlag(uint32_t player) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(base + kClearPlayerFlagRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(player)), kStraitJacketFlag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void JacketTick(DWORD now, uint32_t player, bool running) {
    if (!jacket.armed) return;
    if (!running || !player) { jacket.sawLoad = true; return; }   // the old level goes first
    if (!jacket.sawLoad && player == jacket.isaac) return;
    if (player != jacket.isaac || !jacket.since) { jacket.isaac = player; jacket.since = now; }
    if (now - jacket.since > 15000) { jacket.armed = false; return; }
    static DWORD checkedAt = 0;
    if (now - checkedAt < 500) return;
    checkedAt = now;
    if (jacketSite < 0) {
        unsigned char bytes[sizeof(kClearPlayerFlagBytes)]{};
        jacketSite = Read(base + kClearPlayerFlagRva, bytes, sizeof(bytes)) && std::memcmp(bytes, kClearPlayerFlagBytes, sizeof(bytes)) == 0 ? 1 : 0;
        if (!jacketSite) Line("straitjacket: ClearPlayerFlag at %08X does not match, the flag is left alone", base + kClearPlayerFlagRva);
    }
    if (jacketSite != 1) { jacket.armed = false; return; }
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots || !Read(manager + 8, &capacity, 4) ||
        !capacity || capacity > 8)
        return;
    const uint32_t word = kPlayerFlags + 4 * (kStraitJacketFlag >> 5), bit = 1u << (kStraitJacketFlag & 31);
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t bp = 0, vtable = 0, flags = 0;
        if (!Read(slots + 4 * i, &bp, 4) || bp < 0x103C0) continue;
        const uint32_t entity = bp - 0x3C0;
        if (!Read(entity, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva || IsGhostPlayer(entity)) continue;
        if (!Read(entity + word, &flags, 4) || !(flags & bit)) continue;
        const bool ok = ClearJacketFlag(entity);
        InterlockedIncrement(ok ? &jacketCleared : &jacketFaults);
        Line("straitjacket: player %08X (slot %u) %s after the jump", entity, i, ok ? "taken out of the straitjacket mode" : "FAULTED clearing the flag");
    }
}
// After a launch from the main menu Isaac starts as a new game: the chapter's
// start suit (none for chapter 1), the plasma cutter, stasis and kinesis.
void KitTick(DWORD now, uint32_t player, bool running) {
    if (!kit.armed || !running || !player) return;
    if (player != kit.isaac) { kit.isaac = player; kit.seenAt = now; return; }
    if (now - kit.seenAt < 2000) return;
    kit.armed = false;
    InterlockedIncrement(&kitsGiven);
    const auto& ch = coop_debug::kChapters[kit.chapter];
    const int worn = bot::WornSuitIndex(player);
    const bool bare = worn < 0 || worn == bot::PatientSuit();
    const bool dressed = ch.hasUniform && bare && coop_uniform::BotDress(player, ch.uniform);
    Line("kit for Isaac %08X in %s: wears the %s; %s", player, ch.key, worn >= 0 ? bot::SuitName(worn) : "(no registered suit)",
         !ch.hasUniform ? "the chapter has no start suit" : !bare ? "suit kept" : dressed ? "changes into the chapter's suit" : "suit change FAILED");
    mpmap::GiveLoadout(player);
    mpmap::EnableModules(player);
}
void HoldTick(bool running) {
    if (!mpmap::debugHold) return;
    if (running && !InMainMenu()) { holdLevelSeen = true; return; }
    if (holdLevelSeen && InMainMenu()) {
        mpmap::debugHold = false;
        holdLevelSeen = false;
        InterlockedIncrement(&holdsReleased);
        Line("back in the main menu: saves released");
        mpmap::ClearOverride("debug menu jump over, back in the main menu");
    }
}

// --- the panel ----------------------------------------------------------------------------------
int RowCount(int tab) {
    switch (tab) {
    case coop_debug::kLevels: return static_cast<int>(coop_debug::kCheckpointCount);
    case coop_debug::kWeapons: return coop_shop::kGunCount;
    case coop_debug::kSupplies: return SupplyCount();
    case coop_debug::kSuits: return bot::kSuitKinds;
    case coop_debug::kCheats: return coop_debug::kCheatCount;
    case coop_debug::kHost: return coop_debug::kHostRowCount;
    default: return 0;
    }
}
void RowText(int tab, int row, uint32_t player, DWORD now, coop_shop::Row& r, char* detail, size_t detailSize) {
    r.style = coop_shop::kRowNormal;
    r.right[0] = 0;
    detail[0] = 0;
    switch (tab) {
    case coop_debug::kLevels: {
        const auto& cp = coop_debug::kCheckpoints[row];
        coop_debug::CheckpointLabel(static_cast<unsigned>(row), r.label, sizeof(r.label));
        std::snprintf(r.right, sizeof(r.right), "%s", confirm.Armed(tab, row, now, kConfirmMs) ? "AGAIN TO LOAD" : cp.inCatalog ? "LOAD" : "LOAD (?)");
        std::snprintf(detail, detailSize, "%s %08X-%08X%s", coop_debug::kChapters[cp.chapter].key, cp.guid[0], cp.guid[1],
                      holdSaves ? ", saves held until the main menu" : ", saves NOT held");
        return;
    }
    case coop_debug::kWeapons: {
        int kinds[8];
        const int n = player ? shop::CarriedKinds(player, kinds, nullptr) : 0;
        bool carried = false;
        for (int k = 0; k < n; ++k) carried = carried || kinds[k] == row;
        std::snprintf(r.label, sizeof(r.label), "%s", coop_shop::kGuns[row].name);
        std::snprintf(r.right, sizeof(r.right), "%s", !player ? "NO ISAAC" : carried ? "CARRIED" : "GIVE");
        if (!player || !shop::installed || !shop::gunsOk) r.style = coop_shop::kRowUnavailable;
        std::snprintf(detail, detailSize, "Into a free weapon slot; its stream loads first if it is not in");
        return;
    }
    case coop_debug::kSupplies:
        SupplyLabel(row, r.label, sizeof(r.label));
        std::snprintf(r.right, sizeof(r.right), "%s", player ? "GIVE" : "NO ISAAC");
        if (!player || !shop::installed || !shop::deliveryOk) r.style = coop_shop::kRowUnavailable;
        std::snprintf(detail, detailSize, "Into the RIG inventory, or at your feet when it is full");
        return;
    case coop_debug::kSuits: {
        bool registered = false;
        shop::SuitHere(bot::kSuits[row].guid, registered);
        const bool worn = player && bot::WornSuitIndex(player) == row;
        std::snprintf(r.label, sizeof(r.label), "%s", bot::kSuits[row].name);
        std::snprintf(r.right, sizeof(r.right), "%s", worn ? "WORN" : !registered ? "N/A" : player ? "WEAR" : "NO ISAAC");
        if (!player || !registered || !shop::installed || !shop::suitsOk) r.style = coop_shop::kRowUnavailable;
        std::snprintf(detail, detailSize, "%s", registered ? "ChangeToUniform; the bots follow out of the patient suit" : "Not registered in this build of the game");
        return;
    }
    case coop_debug::kCheats:
        std::snprintf(r.label, sizeof(r.label), "%s", coop_debug::CheatName(row));
        if (row == coop_debug::kGodMode) std::snprintf(r.right, sizeof(r.right), "%s", god ? "ON" : "OFF");
        else if (row == coop_debug::kInfiniteStasis) std::snprintf(r.right, sizeof(r.right), "%s", infiniteStasis ? "ON" : "OFF");
        else if (row == coop_debug::kShowEnemies) std::snprintf(r.right, sizeof(r.right), "%s", InterlockedCompareExchange(&marks::showAll, 0, 0) ? "ON" : "OFF");
        else if (row == coop_debug::kAddCredits) std::snprintf(r.right, sizeof(r.right), "+%d", creditStep);
        else if (row == coop_debug::kAddNodes) std::snprintf(r.right, sizeof(r.right), "+%d", nodeStep);
        else if (row == coop_debug::kVoiceTest) std::snprintf(r.right, sizeof(r.right), "%s", voices::installed ? "PLAY" : "OFF");
        else std::snprintf(r.right, sizeof(r.right), "USE");
        if ((!player && !coop_debug::CheatNeedsNoIsaac(row)) || (row == coop_debug::kVoiceTest && !voices::installed)) r.style = coop_shop::kRowUnavailable;
        std::snprintf(detail, detailSize, "%s", row == coop_debug::kGodMode ? "No damage reaches Isaac's health (scripted deaths still kill)"
                                              : row == coop_debug::kInfiniteStasis ? "The stasis module is refilled twice a second"
                                              : row == coop_debug::kShowEnemies ? "Every living enemy of the level: kind, health, distance (nearest 60)"
                                              : row == coop_debug::kVoiceTest ? "Everybody says a test line from his voice pack, in 3D, one after the other"
                                              : "Applied at once");
        return;
    case coop_debug::kHost:
        HostRowText(row, now, r, detail, detailSize);
        return;
    default:
        return;
    }
}
void Publish(uint32_t player, DWORD now) {
    coop_shop::View v;
    std::memset(&v, 0, sizeof(v));
    std::snprintf(v.title, sizeof(v.title), "DEBUG MENU");
    const int32_t credits = player ? shop::Credits(player) : -1;
    int32_t nodes = -1;
    if (player) Read(player + bot::kPlayerNodes, &nodes, 4);
    char number[24];
    coop_shop::FormatCredits(number, sizeof(number), credits < 0 ? 0 : credits);
    std::snprintf(v.credits, sizeof(v.credits), "CR %s  N %d", number, nodes < 0 ? 0 : nodes);
    char map[64] = "", stream[64] = "";
    level::CurrentMap(map, sizeof(map), stream, sizeof(stream));
    std::snprintf(v.wave, sizeof(v.wave), "%s%s%s%s", map[0] ? map : "?", god ? "  GOD" : "", infiniteStasis ? "  STASIS" : "", mpmap::debugHold ? "  SAVES HELD" : "");
    if (cursor.tab == coop_debug::kHost)
        std::snprintf(v.wave, sizeof(v.wave), "%s", NetRole() == 2 ? "CLIENT: THE HOST'S TOOLS" : directorInfo[0] ? directorInfo : "NO DIRECTOR");
    v.tabCount = coop_debug::kTabCount;
    for (int t = 0; t < coop_debug::kTabCount; ++t) std::snprintf(v.tabNames[t], sizeof(v.tabNames[t]), "%s", coop_debug::TabName(t));
    v.tab = cursor.tab;
    const int count = RowCount(cursor.tab);
    coop_debug::Fit(cursor, count, kRowsShown);
    const int top = cursor.top[cursor.tab], sel = cursor.row[cursor.tab];
    v.rowCapacity = kRowsShown;
    for (int i = top; i < count && v.rowCount < kRowsShown; ++i) {
        coop_shop::Row& r = v.rows[v.rowCount++];
        char detail[96];
        RowText(cursor.tab, i, player, now, r, detail, sizeof(detail));
        if (i == sel) { r.style |= coop_shop::kRowSelected; std::snprintf(v.detail, sizeof(v.detail), "%s", detail); }
    }
    v.above = top > 0;
    v.below = top + v.rowCount < count;
    // the newer of our own status and the supply terminal's (its deliveries report there)
    const bool shopNewer = shop::statusAt > statusAt && now - shop::statusAt < kStatusMs;
    if (shopNewer) { std::snprintf(v.status, sizeof(v.status), "%s", shop::status); v.statusKind = shop::statusKind; }
    else if (status[0] && (static_cast<LONG>(now - statusAt) <= 0 || now - statusAt < kStatusMs)) { std::snprintf(v.status, sizeof(v.status), "%s", status); v.statusKind = statusKind; }
    char keyName[16];
    std::snprintf(v.hint, sizeof(v.hint), "A/D TAB  W/S ROW  Q/E %s  SPACE OR CLICK USE  %s CLOSE",
                  cursor.tab == coop_debug::kLevels ? "CHAPTER" : cursor.tab == coop_debug::kHost ? "VALUE" : "PAGE", coop_shop::KeyName(key, keyName));
    if (published && std::memcmp(&v, &lastView, sizeof(v)) == 0) return;
    lastView = v;
    published = true;
    overlay::Publish(v);
}
void Close(const char* why) {
    open = false;
    published = false;
    overlay::Hide();
    overlay::SetPointer(false, 0, 0);
    for (int vk = 0x60; vk <= 0x6F; ++vk)
        if (GetAsyncKeyState(vk) & 0x8000) shop::swallow.Take(vk);
    InterlockedExchange(&shop::foreignPanel, 0);
    focusrate::PanelHolds(2, false);
    Line("closed (%s) after %lu s", why, static_cast<unsigned long>((GetTickCount() - openedAt) / 1000));
}
void Open(DWORD now, uint32_t player, bool menu) {
    if (shop::open) shop::Close("the debug menu opened", coop_shop::GateOut::Open, false);
    open = true;
    openedAt = now;
    published = false;
    input.Reset();
    InterlockedExchange(&shop::foreignPanel, 1);
    focusrate::PanelHolds(2, true);
    InterlockedIncrement(&opens);
    drawCheckDone = false;
    drawHooksAtOpen = InterlockedCompareExchange(&overlay::hookCalls, 0, 0);
    drawFramesAtOpen = InterlockedCompareExchange(&overlay::panelFrames, 0, 0);
    drawOffscreenAtOpen = InterlockedCompareExchange(&overlay::offscreenFrames, 0, 0);
    Line("opened (tab %s, overlay %s, %s, Isaac %08X)", coop_debug::TabName(cursor.tab), overlay::ready ? "ready" : "NOT READY",
         menu ? "main menu" : "in a level", player);
}
// 17.10.: after a level the menu opened in the main menu but nothing showed. One second after
// an open: were frames drawn, and if not, why (no render hook calls, disabled, camera hidden).
bool CheckDrawn(DWORD now) {
    if (drawCheckDone || now - openedAt < 1000) return false;
    drawCheckDone = true;
    const LONG hooks = InterlockedCompareExchange(&overlay::hookCalls, 0, 0) - drawHooksAtOpen;
    const LONG frames = InterlockedCompareExchange(&overlay::panelFrames, 0, 0) - drawFramesAtOpen;
    const LONG offscreen = InterlockedCompareExchange(&overlay::offscreenFrames, 0, 0) - drawOffscreenAtOpen;
    if (frames > 0) {
        const overlay::HitMap h = overlay::Hits();
        Line("open for 1 s: panel drawn in %ld frame(s) at (%d %d)-(%d %d) of %dx%d, %ld frame(s) ended offscreen (back buffer draws %ld, failures %ld), "
             "foreground %s",
             frames, h.panelX0, h.panelY0, h.panelX1, h.panelY1, h.w, h.h, offscreen, InterlockedCompareExchange(&overlay::backBufferDraws, 0, 0),
             InterlockedCompareExchange(&overlay::backBufferFailures, 0, 0), necro::GameWindowFocused() ? "this game" : "ANOTHER window");
        return false;
    }
    InterlockedIncrement(&notDrawn);
    Line("open for 1 s but NOT DRAWN: render hook calls +%ld, frames drawn +%ld, visible=%ld disabled=%ld camera_hidden=%ld draw_faults=%ld%s", hooks, frames,
         InterlockedCompareExchange(&overlay::visible, 0, 0), InterlockedCompareExchange(&overlay::disabled, 0, 0),
         InterlockedCompareExchange(&overlay::cameraHidden, 0, 0), InterlockedCompareExchange(&overlay::drawFaults, 0, 0),
         hooks == 0 ? " (the game's frame end is not reached: the frontend renders another way)" : "");
    // An open panel mutes the game's input (focusrate::PanelHolds): unseen, that is a frozen menu.
    Close("nothing drawn: the input goes back to the game");
    return true;
}
void Use(uint32_t player, DWORD now) {
    const int tab = cursor.tab, row = cursor.row[tab];
    if (row < 0 || row >= RowCount(tab)) return;
    switch (tab) {
    case coop_debug::kLevels:
        if (!confirm.Press(tab, row, now, kConfirmMs)) {
            char label[64];
            coop_debug::CheckpointLabel(static_cast<unsigned>(row), label, sizeof(label));
            SetStatus(coop_shop::kStatusInfo, "Space or click again to load %s", label);
            return;
        }
        Load(row, false);
        return;
    case coop_debug::kWeapons:
        if (player) GiveGun(row, player); else SetStatus(coop_shop::kStatusBad, "No Isaac here");
        return;
    case coop_debug::kSupplies:
        if (player) GiveSupply(row, player); else SetStatus(coop_shop::kStatusBad, "No Isaac here");
        return;
    case coop_debug::kSuits:
        if (player) GiveSuit(row, player); else SetStatus(coop_shop::kStatusBad, "No Isaac here");
        return;
    case coop_debug::kCheats:
        UseCheat(row, player);
        return;
    case coop_debug::kHost:
        UseHost(row, player, now);
        return;
    default:
        return;
    }
}

void TickInner() {
    const DWORD now = GetTickCount();
    const bool running = mpmap::LmState(nullptr) == coop_mpmap::kRunning;
    // Back in the main menu the player manager still holds the old Isaac for a
    // while (09.10.): no Isaac there, whatever slot 0 says.
    static bool menuNow = true;
    static DWORD menuReadAt = 0;
    if (now - menuReadAt >= 250) { menuReadAt = now; menuNow = InMainMenu(); }
    const uint32_t player = running && !menuNow ? LocalCampaignPlayer() : 0;
    InterlockedExchange(&godPlayer, static_cast<LONG>(player));
    KitTick(now, player, running);
    JacketTick(now, player, running);
    HoldTick(running);
    LookUpDirector(now);
    if (infiniteStasis && player && running && now - stasisAt >= 500) {
        stasisAt = now;
        if (RefillStasis(player)) InterlockedIncrement(&stasisRefills);
    }
    const bool focused = necro::GameWindowFocused();
    auto down = [&](int vk) { return focused && vk && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool toggle = toggleEdge.Pressed(down(key));
    if (toggle) {
        if (open) Close("its key");
        else Open(now, player, menuNow);
    }
    if (!open) {
        // a closed menu never keeps the game's input muted
        if (InterlockedCompareExchange(&focusrate::panelHolds, 0, 0) & 2) {
            focusrate::PanelHolds(2, false);
            if (InterlockedIncrement(&strayHolds) <= 3) Line("its input hold was still set while closed: released");
        }
        return;
    }
    if (CheckDrawn(now)) return;
    if (cursor.tab == coop_debug::kHost) PollDirector(now);
    // Escape is not a close key: it also opens the game's pause menu. The panel stays
    // up in the pause menu and in the main menu; the first build hid it whenever the
    // game time stood still, and with a stale Isaac that was the whole main menu.
    const panelinput::Frame f = input.Read(now, focused);
    if (f.tab >= 0 && f.tab < coop_debug::kTabCount) cursor.tab = f.tab;
    if (f.left) coop_debug::StepTab(cursor, -1);
    if (f.right) coop_debug::StepTab(cursor, 1);
    const int tab = cursor.tab, count = RowCount(tab);
    if (f.up) coop_debug::StepRow(cursor, -1, count);
    if (f.down) coop_debug::StepRow(cursor, 1, count);
    if (f.scroll) coop_debug::StepRow(cursor, f.scroll, count);
    if ((f.pageUp || f.pageDown) && tab == coop_debug::kLevels) cursor.row[tab] = coop_debug::ChapterJump(cursor.row[tab], f.pageDown ? 1 : -1);
    else if ((f.pageUp || f.pageDown) && tab == coop_debug::kHost && coop_debug::HostRowHasValue(cursor.row[tab]))
        coop_debug::StepHostValue(hostValues, cursor.row[tab], (f.pageDown ? 1 : -1) * ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 5 : 1), familyCount);
    else if (f.pageUp || f.pageDown) coop_debug::StepRow(cursor, f.pageDown ? kRowsShown : -kRowsShown, count);
    // The view's rows start at the tab's top row.
    if (f.hoverRow >= 0 && cursor.top[tab] + f.hoverRow < count) cursor.row[tab] = cursor.top[tab] + f.hoverRow;
    bool use = f.use;
    if (f.clickRow >= 0 && cursor.top[tab] + f.clickRow < count) {
        const int row = cursor.top[tab] + f.clickRow;
        if (row == cursor.row[tab]) use = true;
        else cursor.row[tab] = row;
    }
    if (use) Use(player, now);
    Publish(player, now);
}
int TickFilter(unsigned code) {
    InterlockedIncrement(&faults);
    gate::Event("debug FAULT tick code %08X", code);
    return EXCEPTION_EXECUTE_HANDLER;
}
// Game thread, from coop::Poll.
void Tick() {
    // The host's tools reach a partner whatever his own menu does ([Debug] Enabled=0 too).
    __try {
        DrainAdmin();
    } __except (TickFilter(GetExceptionCode())) {
    }
    if (!installed) return;
    __try {
        TickInner();
    } __except (TickFilter(GetExceptionCode())) {
        if (InterlockedCompareExchange(&faults, 0, 0) >= 5) {
            installed = false;
            open = false;
            overlay::Hide();
            overlay::SetPointer(false, 0, 0);
            InterlockedExchange(&shop::foreignPanel, 0);
            focusrate::PanelHolds(2, false);
            focusrate::panelKey[key & 0xFF] = false;
            gate::Event("debug: five faulted ticks, switched off for the session");
        }
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Debug", "Enabled", 1, ini.c_str()) != 0;
    logOut = log;   // the host's god mode hooks through it even with the menu off
    if (!enabled) { std::fprintf(log, "debug menu off ([Debug] Enabled=0)\n"); return; }
    logOut = log;
    key =GetPrivateProfileIntA("Debug", "Key", VK_INSERT, ini.c_str());
    if (key <= 0 || key > 255) key = VK_INSERT;
    holdSaves = GetPrivateProfileIntA("Debug", "HoldSaves", 1, ini.c_str()) != 0;
    creditStep = GetPrivateProfileIntA("Debug", "Credits", 10000, ini.c_str());
    nodeStep = GetPrivateProfileIntA("Debug", "PowerNodes", 5, ini.c_str());
    if (creditStep < 1 || creditStep > 1000000) creditStep = 10000;
    if (nodeStep < 1 || nodeStep > 100) nodeStep = 5;
    if (!overlay::ready) overlay::Install(log, ini);
    installed = overlay::ready;
    if (installed) focusrate::HideKeyFromGame(key);   // its key is ours whenever the menu is in
    std::fprintf(log, "debug menu %s: key %d, %u checkpoints in %u chapters from map.txt, saves %s on a jump, credits +%d, power nodes +%d; "
                      "deliveries %s (shop installed=%d guns=%d supplies=%d suits=%d)\n",
                 installed ? "on" : "OFF (the overlay did not install)", key, coop_debug::kCheckpointCount, coop_debug::kChapterCount,
                 holdSaves ? "held until the main menu" : "NOT held", creditStep, nodeStep, shop::installed ? "through the supply terminal" : "OFF ([Shop] Enabled=0)",
                 shop::installed ? 1 : 0, shop::gunsOk ? 1 : 0, shop::deliveryOk ? 1 : 0, shop::suitsOk ? 1 : 0);
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    const LONG values[] = {open ? 1 : 0, god ? 1 : 0, infiniteStasis ? 1 : 0, mpmap::debugHold ? 1 : 0, opens, jumps, jumpsFromMenu, jumpsRefused, gunsAsked,
                           suppliesAsked, suitsAsked, cheatsUsed, godHits, stasisRefills, kitsGiven, holdsReleased, faults, hostTools, hostRefused,
                           everyoneSent, everyoneApplied, killAllRuns, killAllKilled, respawn::revivesForced, survnet::adminSent, survnet::adminReceived};
    static LONG last[sizeof(values) / sizeof(values[0])] = {-1};
    if (std::memcmp(last, values, sizeof(values)) == 0) return;
    std::memcpy(last, values, sizeof(values));
    std::fprintf(log, "debug menu open=%ld god=%ld infinite_stasis=%ld saves_held=%ld opens=%ld jumps=%ld jumps_from_menu=%ld jumps_refused=%ld guns=%ld supplies=%ld "
                      "suits=%ld cheats=%ld god_hits=%ld stasis_refills=%ld kits=%ld holds_released=%ld faults=%ld damage_hook=%d | host tools=%ld refused=%ld "
                      "everyone sent=%ld applied=%ld kill_all runs=%ld killed=%ld revives_forced=%ld wire sent=%ld received=%ld director=%s\n",
                 values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9], values[10], values[11], values[12],
                 values[13], values[14], values[15], values[16], damageHooked ? 1 : 0, values[17], values[18], values[19], values[20], values[21],
                 values[22], values[23], values[24], values[25], adminFn ? "found" : "not found");
    static LONG lastJacket = -1;
    const LONG jacketNow = jacketJumps + jacketKept * 7 + jacketCleared * 13 + jacketFaults * 17;
    if (jacketNow != lastJacket) {
        lastJacket = jacketNow;
        std::fprintf(log, "debug straitjacket jumps=%ld kept=%ld cleared=%ld faults=%ld site=%d\n", jacketJumps, jacketKept, jacketCleared, jacketFaults, jacketSite);
    }
    static LONG lastSync = -1;
    const LONG syncNow = jumpsSent + jumpsFollowed * 7 + jumpsMismatched * 13 + clientJumpsRefused * 17;
    if (syncNow != lastSync) {
        lastSync = syncNow;
        std::fprintf(log, "debug jump_sync sent=%ld followed=%ld mismatched=%ld client_jumps_refused=%ld\n", jumpsSent, jumpsFollowed, jumpsMismatched,
                     clientJumpsRefused);
    }
}
}  // namespace debugmenu
