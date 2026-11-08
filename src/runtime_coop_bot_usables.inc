// Dead Space 2 United by Hafer.
// --- power node doors and stasis stations ----------------------------------------------
// Both are usables in the TargetingManager list ("store rooms") that the game offers a
// bot like any Player (Player+0x3F4 == entity+0x230) and starts on its use press
// (PlayerUseObjectSM::CheckUseObjectStart 0x00870C40, event use_object 0xC4CCF1F7 on the
// bot's own controller). Analysis: analysis\DS2_BOT_NODE_DOORS_STASIS_STATIONS_2026-10-08.md.
//
// PowerNodeSwitch (vtable 0x01B5DC78, IUsable +0x230 with vtable 0x01B5DB00) is the door
// panel. EnterUse shows the prompt; UpdateUse (0x00EDC830, thiscall(IUsable*, uint, float),
// ret 8) waits for the confirm, which it reads raw: DeviceManager::CheckButtonsANY
// (0x0077FE90, thiscall(dm, ctrl, mask, mode), ret 0Ch) with the advance mask 0x20,
// false on any pad whose "connected" byte is 0, so on every bot pad. Then the user's own
// nodes pay (Player vtable +0x248 / +0x24C, Player+0x42C), the switch goes on and the
// level opens the room. For a bot that call site (0x00EDC89D) goes through a stub that
// answers yes while that bot is in its confirm step at that panel; ebx holds the panel's
// IUsable and edi the user Player there. Everything else is the game's.
//
// RechargeStation (vtable 0x01B43108, IUsable +0x230 with vtable 0x01B42F90) needs no
// help: EnterUse (0x00BA73E0) makes the activator the charge target (+0x244) when the
// station is ready, and its tick pours the charge into that Player's stasis energy.
constexpr uint32_t kNodeSwitchVtableRva = 0x175DC78, kNodeSwitchUsableVtableRva = 0x175DB00, kStationVtableRva = 0x1743108,
                   kStationUsableVtableRva = 0x1742F90;
constexpr uint32_t kSwitchEnabled = 0x234, kSwitchOn = 0x235, kNodeAllowTime = 0x474, kNodeUser = 0x4D4, kNodeAttrRequired = 0x238;
constexpr uint32_t kStationState = 0x234, kStationActivator = 0x240, kStationTarget = 0x244, kStationFull = 0x2A4, kStationCharge = 0x2A8,
                   kStationDowntime = 0x2AC, kStationAttrAir = 0x88, kStationAttrActivation = 0xA0, kEntityAttributes = 0xC, kEntityMatrix = 0xD0;
constexpr uint32_t kGameSecondsRva = 0x1C4D3C4;   // the clock PowerNodeSwitch::CanBeUsed compares its denial time with
constexpr uint32_t kNodeConfirmSiteRva = 0xADC89D, kCheckButtonsRva = 0x37FE90, kNodeUpdateUseRva = 0xADC830, kStationEnterUseRva = 0x7A73E0;
constexpr unsigned kMaxDoors = 8, kMaxStations = 8;
constexpr float kUsableStand = 1.0f, kUsableScanRange = 30.0f;

bool doorsReady = false, stationsReady = false;
coop_bot::UsableTune usableTune;
uint32_t usableImageEnd = 0;

struct DoorMemo {
    uint32_t panel = 0;
    Vec3 at;
    DWORD leaderNearSince = 0, failedAt = 0;
    int failures = 0;
    bool seenThisScan = false;
    int8_t verdict[kMaxBots]{-1, -1, -1};
};
struct StationMemo {
    uint32_t entity = 0;
    Vec3 at;
    DWORD failedAt = 0;
    int failures = 0;
    bool seenThisScan = false;
    int8_t verdict[kMaxBots]{-1, -1, -1};
};
DoorMemo doorMemos[kMaxDoors];
StationMemo stationMemos[kMaxStations];
DWORD usableScanAt = 0;

enum class UseStep : uint8_t { None, Approach, Align, Press, Confirm, Charge };
enum class UseEnd : uint8_t { Done, Fight, Gone, Taken, NeverThere, NotOffered, PressRefused, LetGo, Stuck, Interrupted, Count };
const char* UseEndName(UseEnd e) {
    static const char* const kNames[] = {"done", "fight", "gone", "taken", "never_there", "not_offered", "press_refused", "let_go", "stuck", "interrupted"};
    const unsigned i = static_cast<unsigned>(e);
    return i < std::size(kNames) ? kNames[i] : "?";
}
struct UseVisit {
    UseStep step = UseStep::None;
    bool door = false, inward = false, confirmArmed = false, confirmed = false, backPressed = false;
    uint32_t target = 0, entity = 0;
    int side = 0, presses = 0, nodesBefore = -1;
    float energyBefore = -1.0f;
    DWORD startedAt = 0, stepAt = 0, pressAt = 0, tickAt = 0, evalAt = 0, backUntil = 0;
};
UseVisit useVisits[kMaxBots];

volatile LONG useLines = 0, doorsSeen = 0, doorStarts = 0, doorOffers = 0, doorPresses = 0, doorUses = 0, doorOpened = 0, doorBackPresses = 0,
              stationsSeen = 0, stationStarts = 0, stationOffers = 0, stationPresses = 0, stationActivations = 0, stationRefills = 0, stationEnergyGain = 0,
              usableScanFaults = 0, confirmCalls = 0, confirmForced = 0, confirmBotWaits = 0, confirmPadMismatch = 0, confirmRelays = 0, verdictLines = 0,
              doorAdoptions = 0;
volatile LONG doorVerdicts[static_cast<unsigned>(coop_bot::DoorVerdict::Count)]{};
volatile LONG stationVerdicts[static_cast<unsigned>(coop_bot::StationVerdict::Count)]{};
volatile LONG doorEnds[static_cast<unsigned>(UseEnd::Count)]{}, stationEnds[static_cast<unsigned>(UseEnd::Count)]{};
bool UseLog() { return InterlockedIncrement(&useLines) <= 400; }
// Verdict changes have their own budget, so a leader pacing near a panel cannot use up the visit lines.
bool VerdictLog() { return InterlockedIncrement(&verdictLines) <= 150; }

// --- the confirm stub ---
uint32_t confirmEbx = 0, confirmEdi = 0;
uintptr_t confirmChained = 0;
bool confirmLaid = false, confirmRefused = false;
using CheckButtons = bool(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
bool __fastcall OnNodeConfirm(void* dm, void*, uint32_t ctrl, uint32_t mask, uint32_t mode) {
    InterlockedIncrement(&confirmCalls);
    const uint32_t panel = confirmEbx - kUsable, user = confirmEdi;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const Unit& unit = units[i];
        UseVisit& v = useVisits[i];
        if (!unit.entity || unit.entity != user) continue;
        if (v.step != UseStep::Confirm || !v.door || v.target != panel || !v.confirmArmed) { InterlockedIncrement(&confirmBotWaits); break; }
        if ((ctrl & 0xFF) != unit.controller) {
            if (InterlockedIncrement(&confirmPadMismatch) <= 5 && UseLog())
                gate::Event("bot %u: power node confirm asked on pad %u, its own is %u; not answered", i + 1, ctrl & 0xFF, unit.controller);
            break;
        }
        v.confirmed = true;
        if (InterlockedIncrement(&confirmForced) <= 20 && UseLog())
            gate::Event("bot %u: power node panel %08X asks for the confirm (pad %u, mask %04X, mode %u): answered yes", i + 1, panel, ctrl & 0xFF, mask & 0xFFFF,
                        mode & 0xFF);
        return true;
    }
    return reinterpret_cast<CheckButtons>(confirmChained ? confirmChained : base + kCheckButtonsRva)(dm, ctrl, mask, mode);
}
__declspec(naked) void NodeConfirmStub() {
    __asm {
        mov confirmEbx, ebx
        mov confirmEdi, edi
        jmp OnNodeConfirm
    }
}
// Lays the stub in where it is missing; true while the site carries it.
bool RefreshConfirm(FILE* log) {
    if (confirmRefused) return false;
    const uintptr_t site = base + kNodeConfirmSiteRva;
    unsigned char code[13]{};
    if (!Read(site - 8, code, sizeof(code))) return false;
    const uintptr_t stub = reinterpret_cast<uintptr_t>(&NodeConfirmStub);
    int32_t rel = 0;
    std::memcpy(&rel, code + 9, 4);
    const uintptr_t current = site + 5 + rel;
    if (code[8] == 0xE8 && current == stub) return true;
    // or al,8 tail / mov ecx,esi / call eax / push eax / mov ecx,ebp
    static const unsigned char kBefore[] = {0x08, 0x8B, 0xCE, 0xFF, 0xD0, 0x50, 0x8B, 0xCD};
    const uintptr_t original = base + kCheckButtonsRva;
    const bool foreign = current < base || current >= usableImageEnd;
    if (std::memcmp(code, kBefore, sizeof(kBefore)) != 0 || code[8] != 0xE8 || (current != original && !foreign)) {
        confirmRefused = true;
        if (log) std::fprintf(log, "bot node_doors confirm call at %08X unexpected (call byte %02X, target %08X); left alone\n", static_cast<unsigned>(site), code[8],
                              static_cast<unsigned>(current));
        else gate::Event("bot: power node confirm call at %08X changed into something unexpected; left alone", static_cast<unsigned>(site));
        return false;
    }
    confirmChained = current;
    unsigned char call[5] = {0xE8};
    const int32_t toStub = static_cast<int32_t>(stub - (site + 5));
    std::memcpy(call + 1, &toStub, 4);
    if (!WriteBytes(site, call, sizeof(call))) return false;
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(call));
    if (confirmLaid) InterlockedIncrement(&confirmRelays);
    confirmLaid = true;
    if (log) std::fprintf(log, "bot node_doors confirm call at %08X: stub in front of %s %08X\n", static_cast<unsigned>(site), foreign ? "a foreign hook" : "the engine",
                          static_cast<unsigned>(current));
    else gate::Event("bot: power node confirm call at %08X rewritten by someone else, stub laid in again (in front of %08X)", static_cast<unsigned>(site),
                     static_cast<unsigned>(current));
    return true;
}

// --- reading the two classes ---
bool IsNodeSwitch(uint32_t e) { uint32_t vt = 0; return e && Read(e, &vt, 4) && vt == base + kNodeSwitchVtableRva; }
bool IsStation(uint32_t e) { uint32_t vt = 0; return e && Read(e, &vt, 4) && vt == base + kStationVtableRva; }
float GameSeconds() { float t = 0.0f; return Read(base + kGameSecondsRva, &t, 4) && std::isfinite(t) ? t : 0.0f; }
uint32_t SafeRefEntity(uint32_t field) { uint32_t ref = 0; return Read(field, &ref, 4) && ref > 0x10010 ? ref - 0x10 : 0; }
struct PanelState { bool enabled = false, on = false; uint32_t user = 0; float allowAt = 0.0f; int required = 0; };
bool ReadPanel(uint32_t panel, PanelState& s) {
    unsigned char flags[2]{};
    uint32_t attrs = 0;
    int32_t required = 0;
    if (!IsNodeSwitch(panel) || !Read(panel + kSwitchEnabled, flags, 2) || !Read(panel + kNodeAllowTime, &s.allowAt, 4)) return false;
    s.enabled = flags[0] != 0;
    s.on = flags[1] != 0;
    s.user = SafeRefEntity(panel + kNodeUser);
    s.required = Read(panel + kEntityAttributes, &attrs, 4) && attrs && Read(attrs + kNodeAttrRequired, &required, 4) && required >= 0 && required < 100 ? required : 0;
    return true;
}
struct StationState { int32_t state = -1; uint32_t target = 0, activator = 0; float full = 0.0f, charge = 0.0f, downtime = 0.0f; bool air = false; };
bool ReadStation(uint32_t station, StationState& s) {
    uint32_t attrs = 0;
    unsigned char air = 0;
    float values[3]{};
    if (!IsStation(station) || !Read(station + kStationState, &s.state, 4) || !Read(station + kStationActivator, &s.activator, 4) ||
        !Read(station + kStationTarget, &s.target, 4) || !Read(station + kStationFull, values, sizeof(values)))
        return false;
    s.full = values[0];
    s.charge = values[1];
    s.downtime = values[2];
    s.air = Read(station + kEntityAttributes, &attrs, 4) && attrs && Read(attrs + kStationAttrAir, &air, 1) && air;
    return std::isfinite(s.charge) && std::isfinite(s.downtime);
}
int NodeCount(uint32_t player) { int32_t n = 0; return Read(player + kPlayerNodes, &n, 4) && n >= 0 && n < 100000 ? n : -1; }
bool StasisOf(uint32_t player, float& energy, float& max) { return EnergyOf(HealthOf(player), energy, max); }
uint32_t Offered(uint32_t player) { uint32_t usable = 0; Read(player + kNearbyUsable, &usable, 4); return usable; }

// Where the bot stands: in front of the panel or the station's activation point,
// along its facing (third matrix row); the other side on the second try.
Vec3 UseStand(uint32_t target, bool door, int side, Vec3& faceAt) {
    float m[16]{};
    Read(target + kEntityMatrix, m, sizeof(m));
    const Vec3 r0 = {m[0], m[1], m[2]}, r1 = {m[4], m[5], m[6]}, r2 = {m[8], m[9], m[10]};
    Vec3 at = {m[12], m[13], m[14]};
    Vec3 front = {m[8], 0.0f, m[10]};
    float len = coop_bot::Length(front);
    if (len < 0.05f) { front = {0.0f, 0.0f, 1.0f}; len = 1.0f; }
    front = coop_bot::Scale(front, 1.0f / len);
    float sign = 1.0f;
    uint32_t attrs = 0;
    float off[4]{};
    if (!door && Read(target + kEntityAttributes, &attrs, 4) && attrs && Read(attrs + kStationAttrActivation, off, sizeof(off)) && std::isfinite(off[0]) &&
        std::isfinite(off[1]) && std::isfinite(off[2]) && std::fabs(off[0]) + std::fabs(off[1]) + std::fabs(off[2]) < 3.0f) {
        at = coop_bot::Add(at, coop_bot::Add(coop_bot::Add(coop_bot::Scale(r0, off[0]), coop_bot::Scale(r1, off[1])), coop_bot::Scale(r2, off[2])));
        if (off[2] < 0.0f) sign = -1.0f;
    }
    if (side == 1) sign = -sign;
    faceAt = {at.x, at.y + (door ? 1.3f : 1.0f), at.z};
    return coop_bot::Add(at, coop_bot::Scale(front, sign * kUsableStand));
}

// --- the scan: panels and stations around the leader, from the usable list ---
void UsablesScan(uint32_t leader, const Vec3& leaderPos, DWORD now) {
    if ((!doorsReady && !stationsReady) || now - usableScanAt < 500) return;
    usableScanAt = now;
    if (doorsReady && confirmLaid) RefreshConfirm(nullptr);
    for (DoorMemo& d : doorMemos) d.seenThisScan = false;
    for (StationMemo& s : stationMemos) s.seenThisScan = false;
    uint32_t tm = 0, node = 0;
    if (Read(base + kTargetingRva, &tm, 4) && tm && FirstUsable(tm, node)) {
        for (unsigned steps = 0; node && steps < 512; ++steps) {
            uint32_t vtable = 0, ref = 0, next = 0;
            if (!Read(node, &vtable, 4) || vtable != base + kTargetInfoVtableRva) { InterlockedIncrement(&usableScanFaults); break; }
            if (Read(node + kTargetEntity, &ref, 4) && ref > 0x10010) {
                const uint32_t e = ref - 0x10;
                uint32_t kind = 0;
                Vec3 at;
                Read(e, &kind, 4);
                const bool door = doorsReady && kind == base + kNodeSwitchVtableRva;
                const bool station = stationsReady && kind == base + kStationVtableRva;
                if ((door || station) && bot_sense::Pos(e, at) && coop_bot::Flat(at, leaderPos) < kUsableScanRange && std::fabs(at.y - leaderPos.y) < 6.0f) {
                    if (door) {
                        DoorMemo* slot = nullptr;
                        for (DoorMemo& d : doorMemos) if (d.panel == e) slot = &d;
                        if (!slot)
                            for (DoorMemo& d : doorMemos)
                                if (!d.panel) {
                                    d = DoorMemo{};
                                    d.panel = e;
                                    slot = &d;
                                    InterlockedIncrement(&doorsSeen);
                                    PanelState ps;
                                    ReadPanel(e, ps);
                                    if (UseLog())
                                        gate::Event("bot: power node panel %08X at (%.1f %.1f %.1f), %.1f m from the leader: enabled %d on %d needs %d node(s)", e, at.x, at.y,
                                                    at.z, coop_bot::Flat(at, leaderPos), ps.enabled ? 1 : 0, ps.on ? 1 : 0, ps.required);
                                    break;
                                }
                        if (slot) { slot->at = at; slot->seenThisScan = true; }
                    } else {
                        StationMemo* slot = nullptr;
                        for (StationMemo& s : stationMemos) if (s.entity == e) slot = &s;
                        if (!slot)
                            for (StationMemo& s : stationMemos)
                                if (!s.entity) {
                                    s = StationMemo{};
                                    s.entity = e;
                                    slot = &s;
                                    InterlockedIncrement(&stationsSeen);
                                    StationState ss;
                                    ReadStation(e, ss);
                                    if (UseLog())
                                        gate::Event("bot: stasis station %08X at (%.1f %.1f %.1f), %.1f m from the leader: state %d charge %.0f of %.0f downtime %.1f air %d", e,
                                                    at.x, at.y, at.z, coop_bot::Flat(at, leaderPos), ss.state, ss.charge, ss.full, ss.downtime, ss.air ? 1 : 0);
                                    break;
                                }
                        if (slot) { slot->at = at; slot->seenThisScan = true; }
                    }
                }
            }
            if (!Read(node + kTargetNext, &next, 4) || next == node) break;
            node = next;
        }
    }
    for (DoorMemo& d : doorMemos) if (d.panel && !d.seenThisScan) d = DoorMemo{};
    for (StationMemo& s : stationMemos) if (s.entity && !s.seenThisScan) s = StationMemo{};
    // How long the leader has stood near each panel without using it.
    for (DoorMemo& d : doorMemos) {
        if (!d.panel) continue;
        PanelState ps;
        const bool read = ReadPanel(d.panel, ps);
        const bool leaderClose = read && coop_bot::Flat(d.at, leaderPos) <= usableTune.doorLeaderMeters && std::fabs(d.at.y - leaderPos.y) <= usableTune.floorMeters &&
                                 ps.user != leader;
        if (!leaderClose) d.leaderNearSince = 0;
        else if (!d.leaderNearSince) d.leaderNearSince = now;
        // A bot the panel holds in its prompt without a visit (cut short by a scene) finishes what it started: its visit is taken up again at the confirm.
        Unit* holder = read && ps.user ? UnitOf(ps.user) : nullptr;
        if (!holder) continue;
        UseVisit& w = useVisits[holder->index];
        if (w.door && w.target == d.panel && (w.step == UseStep::Press || w.step == UseStep::Confirm)) continue;
        w = UseVisit{};
        w.door = true;
        w.target = d.panel;
        w.entity = ps.user;
        w.step = UseStep::Confirm;
        w.startedAt = w.stepAt = w.tickAt = w.evalAt = now;
        w.nodesBefore = NodeCount(ps.user);
        InterlockedIncrement(&doorAdoptions);
        if (UseLog()) gate::Event("bot %u: held by the power node panel %08X without a visit; takes it up again at the confirm", holder->index + 1, d.panel);
    }
}

// --- one bot's visit ---
void EndUse(UseEnd how, const char* detail) {
    UseVisit& v = useVisits[u->index];
    if (v.step == UseStep::None) return;
    InterlockedIncrement(v.door ? &doorEnds[static_cast<unsigned>(how)] : &stationEnds[static_cast<unsigned>(how)]);
    // Only what this bot could not do counts against the panel or station; taken by someone else does not.
    const bool failed = how == UseEnd::NeverThere || how == UseEnd::NotOffered || how == UseEnd::PressRefused || how == UseEnd::LetGo || how == UseEnd::Stuck;
    if (failed) {
        for (DoorMemo& d : doorMemos) if (v.door && d.panel == v.target) { ++d.failures; d.failedAt = GetTickCount(); }
        for (StationMemo& s : stationMemos) if (!v.door && s.entity == v.target) { ++s.failures; s.failedAt = GetTickCount(); }
    }
    const int nodes = NodeCount(u->entity);
    float energy = -1.0f, max = 0.0f;
    if (!v.door) StasisOf(u->entity, energy, max);
    if (UseLog()) {
        if (v.door)
            gate::Event("bot %u: power node panel %08X: %s (%s) after %lu ms, step %d, presses %d, confirmed %d, nodes %d -> %d", u->index + 1, v.target,
                        UseEndName(how), detail, static_cast<unsigned long>(GetTickCount() - v.startedAt), static_cast<int>(v.step), v.presses, v.confirmed ? 1 : 0,
                        v.nodesBefore, nodes);
        else
            gate::Event("bot %u: stasis station %08X: %s (%s) after %lu ms, step %d, presses %d, energy %.0f -> %.0f of %.0f", u->index + 1, v.target,
                        UseEndName(how), detail, static_cast<unsigned long>(GetTickCount() - v.startedAt), static_cast<int>(v.step), v.presses, v.energyBefore, energy,
                        max);
    }
    v = UseVisit{};
    u->steer.Reset();
}

void MaybeStartUse(uint32_t leader, const Vec3& self, const Vec3& leaderPos, DWORD now) {
    UseVisit& v = useVisits[u->index];
    if (now - v.evalAt < 500) return;
    v.evalAt = now;
    const bool busy = u->state != State::Active || u->kioskVisit != KioskStep::None || u->holding || u->flying ||
                      u->zeroG || u->tkStep != TkStep::None || u->rescuing || (u->liftHoldUntil && now < u->liftHoldUntil);
    const bool calm = u->seen.enemyCount == 0 && u->command.mode != coop_bot::Mode::Combat;
    bool otherDoor = false, otherStation = false;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        if (i == u->index || useVisits[i].step == UseStep::None) continue;
        if (useVisits[i].door) otherDoor = true;
        else otherStation = true;
    }
    const uint32_t leaderOffered = Offered(leader);
    // Power node doors: the nearest one it may open.
    if (doorsReady) {
        const int nodes = NodeCount(u->entity);
        const bool walletSettled = !benchReady || u->nodesSet;
        const float clock = GameSeconds();
        DoorMemo* best = nullptr;
        float bestDist = 1e9f;
        for (DoorMemo& d : doorMemos) {
            if (!d.panel) continue;
            PanelState ps;
            if (!ReadPanel(d.panel, ps)) continue;
            coop_bot::NodeDoorFacts f;
            f.busy = busy;
            f.enabled = ps.enabled;
            f.on = ps.on;
            f.inUse = ps.user != 0;
            f.leaderAtIt = leaderOffered == d.panel + kUsable;
            f.waitingOut = !(clock > ps.allowAt);
            f.calm = calm;
            f.otherBot = otherDoor;
            f.nodes = walletSettled ? nodes : 0;
            f.required = ps.required;
            f.selfDist = coop_bot::Flat(self, d.at);
            f.selfDy = self.y - d.at.y;
            f.leaderDist = coop_bot::Flat(leaderPos, d.at);
            f.leaderDy = leaderPos.y - d.at.y;
            f.leaderNearMs = d.leaderNearSince ? now - d.leaderNearSince : 0;
            f.failures = d.failures;
            f.sinceFailureMs = d.failedAt ? (static_cast<LONG>(now - d.failedAt) > 0 ? now - d.failedAt : 0) : 0xFFFFFFFFu;   // EndUse stamps fresh
            const coop_bot::DoorVerdict verdict = coop_bot::JudgeNodeDoor(f, usableTune);
            InterlockedIncrement(&doorVerdicts[static_cast<unsigned>(verdict)]);
            if (d.verdict[u->index] != static_cast<int8_t>(verdict)) {
                d.verdict[u->index] = static_cast<int8_t>(verdict);
                if (VerdictLog())
                    gate::Event("bot %u: power node panel %08X: %s (nodes %d of %d needed, wallet %s, bot %.1f m, leader %.1f m near %lu ms, enabled %d on %d user %08X, "
                                "denial until %.1f now %.1f, failures %d)",
                                u->index + 1, d.panel, coop_bot::DoorVerdictName(verdict), nodes, ps.required, walletSettled ? "settled" : "not settled", f.selfDist,
                                f.leaderDist, static_cast<unsigned long>(f.leaderNearMs), ps.enabled ? 1 : 0, ps.on ? 1 : 0, ps.user, ps.allowAt, clock, d.failures);
            }
            if (verdict == coop_bot::DoorVerdict::Go && f.selfDist < bestDist) { bestDist = f.selfDist; best = &d; }
        }
        if (best) {
            v = UseVisit{};
            v.door = true;
            v.target = best->panel;
            v.entity = u->entity;
            v.step = UseStep::Approach;
            v.startedAt = v.stepAt = v.tickAt = v.evalAt = now;
            v.nodesBefore = nodes;
            InterlockedIncrement(&doorStarts);
            Vec3 face;
            const Vec3 stand = UseStand(v.target, true, 0, face);
            if (UseLog())
                gate::Event("bot %u: goes to open the power node panel %08X with its own node (%d), stands at (%.1f %.1f %.1f), %.1f m away", u->index + 1, v.target,
                            nodes, stand.x, stand.y, stand.z, coop_bot::Flat(self, stand));
            return;
        }
    }
    // Stasis stations: the nearest one when its stasis runs low.
    if (stationsReady) {
        uint32_t bits = 0;
        const bool module = Read(u->entity + kAbilities, &bits, 4) && (bits & kAbilityStasis);
        float energy = 0.0f, max = 0.0f, leaderEnergy = 0.0f, leaderMax = 0.0f;
        const bool known = StasisOf(u->entity, energy, max);
        const bool leaderLow = StasisOf(leader, leaderEnergy, leaderMax) && leaderEnergy + 0.5f < leaderMax;
        StationMemo* best = nullptr;
        float bestDist = 1e9f;
        for (StationMemo& s : stationMemos) {
            if (!s.entity) continue;
            StationState ss;
            if (!ReadStation(s.entity, ss)) continue;
            coop_bot::StationFacts f;
            f.busy = busy;
            f.stasisModule = module;
            f.energyKnown = known;
            f.energy01 = known ? energy / max : 1.0f;
            f.airStation = ss.air;
            f.state = ss.state;
            f.targetSet = ss.target != 0;
            f.leaderTarget = ss.target == leader;
            f.leaderAtIt = leaderOffered == s.entity + kUsable && leaderLow;
            f.charge = ss.charge;
            f.downtime = ss.downtime;
            f.calm = calm;
            f.otherBot = otherStation;
            f.selfDist = coop_bot::Flat(self, s.at);
            f.selfDy = self.y - s.at.y;
            f.leaderDist = coop_bot::Flat(leaderPos, s.at);
            f.leaderDy = leaderPos.y - s.at.y;
            f.failures = s.failures;
            f.sinceFailureMs = s.failedAt ? (static_cast<LONG>(now - s.failedAt) > 0 ? now - s.failedAt : 0) : 0xFFFFFFFFu;
            const coop_bot::StationVerdict verdict = coop_bot::JudgeStation(f, usableTune);
            InterlockedIncrement(&stationVerdicts[static_cast<unsigned>(verdict)]);
            // Energy above the threshold is the usual case: logged only on the way down.
            if (s.verdict[u->index] != static_cast<int8_t>(verdict)) {
                s.verdict[u->index] = static_cast<int8_t>(verdict);
                if (verdict != coop_bot::StationVerdict::EnergyOk && VerdictLog())
                    gate::Event("bot %u: stasis station %08X: %s (energy %.0f of %.0f, module %d, state %d target %08X charge %.0f downtime %.1f air %d, bot %.1f m, leader "
                                "%.1f m, failures %d)",
                                u->index + 1, s.entity, coop_bot::StationVerdictName(verdict), energy, max, module ? 1 : 0, ss.state, ss.target, ss.charge, ss.downtime,
                                ss.air ? 1 : 0, f.selfDist, f.leaderDist, s.failures);
            }
            if (verdict == coop_bot::StationVerdict::Go && f.selfDist < bestDist) { bestDist = f.selfDist; best = &s; }
        }
        if (best) {
            v = UseVisit{};
            v.door = false;
            v.target = best->entity;
            v.entity = u->entity;
            v.step = UseStep::Approach;
            v.startedAt = v.stepAt = v.tickAt = v.evalAt = now;
            v.energyBefore = energy;
            InterlockedIncrement(&stationStarts);
            Vec3 face;
            const Vec3 stand = UseStand(v.target, false, 0, face);
            if (UseLog())
                gate::Event("bot %u: goes to the stasis station %08X (energy %.0f of %.0f), stands at (%.1f %.1f %.1f), %.1f m away", u->index + 1, v.target, energy, max,
                            stand.x, stand.y, stand.z, coop_bot::Flat(self, stand));
        }
    }
}

// Still free for this bot: nobody else uses it and the leader does not want it now.
const char* StillFree(uint32_t leader) {
    const UseVisit& v = useVisits[u->index];
    const uint32_t leaderOffered = Offered(leader);
    if (v.door) {
        PanelState ps;
        if (!ReadPanel(v.target, ps)) return "the panel is gone";
        if (ps.on) return "the room is open already";
        if (!ps.enabled) return "the panel was disabled";
        if (ps.user && ps.user != u->entity) return ps.user == leader ? "the leader uses it" : "someone else uses it";
        if (leaderOffered == v.target + kUsable) return "the leader stepped up to it";
        return nullptr;
    }
    StationState ss;
    if (!ReadStation(v.target, ss)) return "the station is gone";
    if (ss.target && ss.target != u->entity) return ss.target == leader ? "the leader charges at it" : "it charges someone else";
    if (ss.state != 1) return "it is not ready";
    return nullptr;
}

// One frame of a visit. False when the brain should run instead.
bool UsablesTick(uint32_t leader, const Vec3& self, const Vec3& leaderPos, DWORD now) {
    if (!doorsReady && !stationsReady) return false;
    UseVisit& v = useVisits[u->index];
    if (v.step != UseStep::None && (v.entity != u->entity || now - v.tickAt > 1500)) EndUse(UseEnd::Interrupted, v.entity != u->entity ? "a new body" : "something else ran");
    if (v.step == UseStep::None) MaybeStartUse(leader, self, leaderPos, now);
    if (v.step == UseStep::None) return false;
    v.tickAt = now;
    Vec3 face;
    const Vec3 stand = UseStand(v.target, v.door, v.side, face);
    const uint32_t offered = Offered(u->entity);
    const bool isOffered = offered == v.target + kUsable;
    coop_bot::Command c;
    c.mode = coop_bot::Mode::Idle;
    // Before the game takes it: given up when a fight starts or the target is someone
    // else's. A press still in flight (600 ms) is waited out, so a bot the game takes
    // after all is never left in the prompt without its visit.
    if (v.step == UseStep::Approach || v.step == UseStep::Align || v.step == UseStep::Press) {
        bool taken = false;
        if (v.step == UseStep::Press) {
            PanelState ps;
            StationState ss;
            taken = v.door ? ReadPanel(v.target, ps) && ps.user == u->entity : ReadStation(v.target, ss) && ss.target == u->entity;
        }
        const bool pressInFlight = v.step == UseStep::Press && v.presses > 0 && now - v.pressAt < 600;
        if (!taken && !pressInFlight) {
            if (u->seen.enemyCount > 0) { EndUse(UseEnd::Fight, "a fight came first"); return false; }
            if (u->holding || u->kioskVisit != KioskStep::None) { EndUse(UseEnd::Interrupted, "told to hold, or at a kiosk"); return false; }
            if (const char* why = StillFree(leader)) {
                EndUse(std::strstr(why, "gone") ? UseEnd::Gone : UseEnd::Taken, why);
                return false;
            }
        }
    }
    switch (v.step) {
    case UseStep::Approach: {
        if (now - v.stepAt > 25000) { EndUse(UseEnd::NeverThere, "not there in 25 s"); return false; }
        Vec3 out = {stand.x - face.x, 0.0f, stand.z - face.z};
        const float len = coop_bot::Length(out);
        const Vec3 outside = len > 0.1f ? coop_bot::Add(stand, coop_bot::Scale(out, 0.8f / len)) : stand;
        const bool blockedNow = now - u->routeBlockedAt < 500;
        if (!v.inward && (coop_bot::Flat(self, outside) < 0.5f || coop_bot::Flat(self, face) < kUsableStand + 0.3f || blockedNow)) {
            v.inward = true;
            u->routeBlockedAt = 0;
        }
        const bool shortOfIt = blockedNow && coop_bot::Flat(self, face) < 2.3f;
        if (isOffered || (v.inward && (coop_bot::Flat(self, stand) < 0.6f || shortOfIt))) {
            if (UseLog())
                gate::Event("bot %u: at the %s %08X, %.1f m from it (%s)", u->index + 1, v.door ? "power node panel" : "stasis station", v.target, coop_bot::Flat(self, face),
                            isOffered ? "offered" : (shortOfIt ? "as close as the way goes" : "on the spot"));
            v.step = UseStep::Align;
            v.stepAt = now;
            break;
        }
        c.move = true;
        c.moveTo = v.inward ? stand : outside;
        c.run = coop_bot::Flat(self, stand) > 6.0f;
        if (v.inward) { c.look = true; c.lookAt = face; }
        break;
    }
    case UseStep::Align:
        c.look = true;
        c.lookAt = face;
        if (isOffered) {
            InterlockedIncrement(v.door ? &doorOffers : &stationOffers);
            v.step = UseStep::Press;
            v.stepAt = now;
            v.presses = 0;
            break;
        }
        if (coop_bot::Flat(self, stand) > 0.9f) { c.move = true; c.moveTo = stand; }
        if (now - v.stepAt > 3500) {
            if (v.side == 0) {
                if (UseLog())
                    gate::Event("bot %u: the %s %08X is not offered (nearby usable %08X); tries the other side", u->index + 1, v.door ? "power node panel" : "stasis station",
                                v.target, offered);
                v.side = 1;
                v.inward = false;
                v.step = UseStep::Approach;
                v.stepAt = now;
                break;
            }
            char text[64];
            std::snprintf(text, sizeof(text), "never offered (nearby usable %08X)", offered);
            EndUse(UseEnd::NotOffered, text);
            return false;
        }
        break;
    case UseStep::Press: {
        c.look = true;
        c.lookAt = face;
        bool taken = false;
        if (v.door) {
            PanelState ps;
            taken = ReadPanel(v.target, ps) && ps.user == u->entity;
        } else {
            StationState ss;
            taken = ReadStation(v.target, ss) && ss.target == u->entity;
        }
        if (taken) {
            if (v.door) {
                InterlockedIncrement(&doorUses);
                v.step = UseStep::Confirm;
            } else {
                InterlockedIncrement(&stationActivations);
                v.step = UseStep::Charge;
            }
            v.stepAt = now;
            if (UseLog())
                gate::Event("bot %u: the %s %08X took it after %d press(es)", u->index + 1, v.door ? "power node panel" : "stasis station", v.target, v.presses);
            break;
        }
        if (!v.presses || now - v.pressAt >= 1200) {
            if (v.presses >= 3) {
                char text[96];
                std::snprintf(text, sizeof(text), "three presses of use did nothing (offered %d)", isOffered ? 1 : 0);
                EndUse(UseEnd::PressRefused, text);
                return false;
            }
            if (!isOffered) break;   // only ever pressed while the game offers it
            ++v.presses;
            v.pressAt = now;
            c.use = true;
            InterlockedIncrement(v.door ? &doorPresses : &stationPresses);
        }
        break;
    }
    case UseStep::Confirm: {
        // In the game's use machine: hands off the pad, the prompt shows a moment.
        u->pad = Pad{};
        if (now < v.backUntil) u->pad.buttons = kBackButton;
        u->padLive = true;
        if (!v.confirmArmed && now - v.stepAt >= 700) v.confirmArmed = true;
        PanelState ps;
        const bool read = ReadPanel(v.target, ps);
        const int nodes = NodeCount(u->entity);
        if ((read && ps.on) || (nodes >= 0 && v.nodesBefore >= 0 && nodes < v.nodesBefore)) {
            InterlockedIncrement(&doorOpened);
            EndUse(UseEnd::Done, read && ps.on ? "opened, its own node spent" : "a node spent, the panel not on yet");
            return true;
        }
        if (!read) { EndUse(UseEnd::Gone, "the panel is gone"); return true; }
        if (ps.user != u->entity) {
            EndUse(UseEnd::LetGo, v.confirmed ? "the panel let it go after the confirm without spending a node" : "the panel let it go before the confirm");
            return true;
        }
        // No answer to the confirm: the game's own cancel (itemmenu_dismiss, 0x80 on its pad), once.
        if (now - v.stepAt > 8000 && !v.backPressed) {
            v.backPressed = true;
            v.backUntil = now + 110;
            InterlockedIncrement(&doorBackPresses);
            if (UseLog())
                gate::Event("bot %u: power node panel %08X: no confirm in 8 s (stub calls %ld, forced %ld, bot waits %ld); presses Back to cancel", u->index + 1, v.target,
                            confirmCalls, confirmForced, confirmBotWaits);
        }
        if (now - v.stepAt > 15000) { EndUse(UseEnd::Stuck, "still at the panel after 15 s; left to the game"); return true; }
        return true;
    }
    case UseStep::Charge: {
        c.look = true;
        c.lookAt = face;
        StationState ss;
        float energy = 0.0f, max = 0.0f;
        const bool read = ReadStation(v.target, ss);
        const bool known = StasisOf(u->entity, energy, max);
        if (u->seen.enemyCount > 0) { EndUse(UseEnd::Fight, "a fight came first; the charge goes on"); return false; }
        if (!read || ss.target != u->entity || (known && energy + 0.5f >= max) || now - v.stepAt > 12000) {
            const LONG gain = known && v.energyBefore >= 0.0f ? static_cast<LONG>(energy - v.energyBefore) : 0;
            if (gain > 0) { InterlockedIncrement(&stationRefills); InterlockedExchangeAdd(&stationEnergyGain, gain); }
            const char* why = !read ? "the station is gone"
                              : ss.target != u->entity ? "the station let it go"
                              : known && energy + 0.5f >= max ? "full"
                                                              : "12 s at the station";
            EndUse(gain > 0 ? UseEnd::Done : UseEnd::LetGo, why);
            return false;
        }
        break;
    }
    default:
        return false;
    }
    u->command = c;
    Act(self, leaderPos, now);
    return true;
}

bool InstallUsables(FILE* log, bool doorsWanted, bool stationsWanted) {
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        usableImageEnd = static_cast<uint32_t>(base + nt->OptionalHeader.SizeOfImage);
    }
    struct Sig { uint32_t rva; unsigned char bytes[16]; size_t size; const char* name; };
    auto check = [&](const Sig& s) {
        unsigned char actual[16]{};
        const bool ok = Read(base + s.rva, actual, s.size) && std::memcmp(actual, s.bytes, s.size) == 0;
        if (!ok) std::fprintf(log, "bot usables: %s signature mismatch at %08X\n", s.name, static_cast<unsigned>(base + s.rva));
        return ok;
    };
    if (doorsWanted) {
        static const Sig kDoorSigs[] = {
            // mov al,[ecx+234h] / ret: Switch::IsEnabled
            {0xABF0E0, {0x8A, 0x81, 0x34, 0x02, 0x00, 0x00, 0xC3}, 7, "Switch::IsEnabled"},
            // mov al,[ecx+235h] / ret: Switch::IsOn
            {0xABF0F0, {0x8A, 0x81, 0x35, 0x02, 0x00, 0x00, 0xC3}, 7, "Switch::IsOn"},
            // comiss xmm0,[esi+244h]: PowerNodeSwitch::CanBeUsed, the denial time at entity+0x474
            {0xA98D43, {0x0F, 0x2F, 0x86, 0x44, 0x02, 0x00, 0x00}, 7, "PowerNodeSwitch::CanBeUsed denial time"},
            // lea edi,[esi+2A4h]: PowerNodeSwitch::SetUser, the user SafePtr at entity+0x4D4
            {0xABEB16, {0x8D, 0xBE, 0xA4, 0x02, 0x00, 0x00}, 6, "PowerNodeSwitch::SetUser"},
            // push ecx / push ebx / push ebp / mov ebx,ecx / mov ecx,[ebx+2A4h] / push esi / push edi / test ecx,ecx / je
            {kNodeUpdateUseRva, {0x51, 0x53, 0x55, 0x8B, 0xD9, 0x8B, 0x8B, 0xA4, 0x02, 0x00, 0x00, 0x56, 0x57, 0x85, 0xC9, 0x74}, 16, "PowerNodeSwitch::UpdateUse"},
            // cmp eax,[esi+238h]: the nodes the panel asks for
            {0xADC8BC, {0x3B, 0x86, 0x38, 0x02, 0x00, 0x00}, 6, "PowerNodeSwitch::UpdateUse price"},
        };
        bool ok = true;
        for (const Sig& s : kDoorSigs) ok = check(s) && ok;
        uint32_t slot = 0, setter = 0;
        const bool slotOk = Read(base + kNodeSwitchUsableVtableRva + 0x3C, &slot, 4) && slot == base + kNodeUpdateUseRva;
        // mov eax,[esp+4] / mov [ecx+42Ch],eax / ret 4: Player::SetUpgradeNodes, what the panel calls on its user
        static const unsigned char kSetNodes[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x81, 0x2C, 0x04, 0x00, 0x00, 0xC2, 0x04, 0x00};
        unsigned char setterBytes[sizeof(kSetNodes)]{};
        const bool setterOk = Read(base + placement::kPlayerPrimaryVtableRva + 0x24C, &setter, 4) && Read(setter, setterBytes, sizeof(setterBytes)) &&
                              std::memcmp(setterBytes, kSetNodes, sizeof(kSetNodes)) == 0;
        if (!slotOk) std::fprintf(log, "bot usables: PowerNodeSwitch IUsable slot 0x3C is %08X, not UpdateUse\n", slot);
        if (!setterOk) std::fprintf(log, "bot usables: Player vtable +0x24C (%08X) is not SetUpgradeNodes\n", setter);
        doorsReady = ok && slotOk && setterOk && RefreshConfirm(log);
    }
    if (stationsWanted) {
        static const Sig kStationSigs[] = {
            // mov eax,[ecx+10h] / push eax / add ecx,-230h / call: RechargeStation::EnterUse -> TryToActivateRechargeStation(activator)
            {kStationEnterUseRva, {0x8B, 0x41, 0x10, 0x50, 0x81, 0xC1, 0xD0, 0xFD, 0xFF, 0xFF, 0xE8}, 11, "RechargeStation::EnterUse"},
            // xorps xmm0,xmm0 / comiss xmm0,[edi+2A8h]: the charge left
            {0x7A730D, {0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0x87, 0xA8, 0x02, 0x00, 0x00}, 10, "RechargeStation charge"},
            // mov ecx,[edi+0Ch] / cmp byte [ecx+88h],0: an air station
            {0x7A7320, {0x8B, 0x4F, 0x0C, 0x80, 0xB9, 0x88, 0x00, 0x00, 0x00, 0x00}, 10, "RechargeStation air flag"},
            // movss xmm0,[edi+2ACh]: the downtime
            {0x7A73AB, {0xF3, 0x0F, 0x10, 0x87, 0xAC, 0x02, 0x00, 0x00}, 8, "RechargeStation downtime"},
            // cmp dword [edi+234h],1 / jne / mov [edi+244h],esi: ready, then the charge target
            {0x7A73BC, {0x83, 0xBF, 0x34, 0x02, 0x00, 0x00, 0x01, 0x75, 0xDE, 0x89, 0xB7, 0x44, 0x02, 0x00, 0x00}, 15, "RechargeStation charge target"},
        };
        bool ok = true;
        for (const Sig& s : kStationSigs) ok = check(s) && ok;
        uint32_t slot = 0;
        const bool slotOk = Read(base + kStationUsableVtableRva + 0x2C, &slot, 4) && slot == base + kStationEnterUseRva;
        if (!slotOk) std::fprintf(log, "bot usables: RechargeStation IUsable slot 0x2C is %08X, not EnterUse\n", slot);
        stationsReady = ok && slotOk && energyReady;
        if (ok && slotOk && !energyReady) std::fprintf(log, "bot usables: stasis stations need the stasis energy calls (energy_calls=0)\n");
    }
    // A bot's use pushing an input context is logged by the context hooks (no push or pop is
    // expected on these paths); with KioskEnter=0 they only watch.
    bool push = false, pop = false;
    if (doorsReady || stationsReady) InstallContextHooks(log, push, pop);
    std::fprintf(log, "bot node_doors=%d (wanted=%d confirm_stub=%d: a bot with its own power node opens a locked room when the leader stood within %.1f m of the panel "
                      "for %lu ms and nothing threatens) stasis_stations=%d (wanted=%d: a bot below %.0f%% stasis uses a ready station within %.0f m, after the leader) "
                      "context_watch=%d\n",
                 doorsReady ? 1 : 0, doorsWanted ? 1 : 0, confirmLaid ? 1 : 0, usableTune.doorLeaderMeters, static_cast<unsigned long>(usableTune.doorWaitMs),
                 stationsReady ? 1 : 0, stationsWanted ? 1 : 0, usableTune.stationBelow * 100.0f, usableTune.stationMeters, push && pop ? 1 : 0);
    return doorsReady || stationsReady;
}

void WriteUsableDiagnostics(FILE* log) {
    if (!doorsReady && !stationsReady) return;
    static LONG last = -1;
    LONG v = doorAdoptions + doorsSeen + doorStarts + doorOffers + doorPresses + doorUses + doorOpened + doorBackPresses + stationsSeen + stationStarts + stationOffers + stationPresses +
             stationActivations + stationRefills + confirmCalls + confirmForced + confirmPadMismatch + confirmRelays + usableScanFaults + contextByBot;
    for (LONG e : doorEnds) v += e;
    for (LONG e : stationEnds) v += e;
    if (v == last) return;
    last = v;
    char doorWhy[512] = "", stationWhy[512] = "", doorEnd[256] = "", stationEnd[256] = "";
    size_t n = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(coop_bot::DoorVerdict::Count) && n < sizeof(doorWhy); ++i)
        if (doorVerdicts[i])
            n += std::snprintf(doorWhy + n, sizeof(doorWhy) - n, "%s%s:%ld", n ? "," : "", coop_bot::DoorVerdictName(static_cast<coop_bot::DoorVerdict>(i)), doorVerdicts[i]);
    n = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(coop_bot::StationVerdict::Count) && n < sizeof(stationWhy); ++i)
        if (stationVerdicts[i])
            n += std::snprintf(stationWhy + n, sizeof(stationWhy) - n, "%s%s:%ld", n ? "," : "",
                               coop_bot::StationVerdictName(static_cast<coop_bot::StationVerdict>(i)), stationVerdicts[i]);
    n = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(UseEnd::Count) && n < sizeof(doorEnd); ++i)
        if (doorEnds[i]) n += std::snprintf(doorEnd + n, sizeof(doorEnd) - n, "%s%s:%ld", n ? "," : "", UseEndName(static_cast<UseEnd>(i)), doorEnds[i]);
    n = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(UseEnd::Count) && n < sizeof(stationEnd); ++i)
        if (stationEnds[i]) n += std::snprintf(stationEnd + n, sizeof(stationEnd) - n, "%s%s:%ld", n ? "," : "", UseEndName(static_cast<UseEnd>(i)), stationEnds[i]);
    std::fprintf(log, "bot usables doors seen=%ld starts=%ld offered=%ld presses=%ld taken=%ld opened=%ld back_presses=%ld adoptions=%ld ends={%s} checks={%s} "
                      "confirm calls=%ld forced=%ld bot_waits=%ld pad_mismatch=%ld relays=%ld | stations seen=%ld starts=%ld offered=%ld presses=%ld activations=%ld "
                      "refills=%ld energy_gained=%ld ends={%s} checks={%s} | scan_faults=%ld context_by_bot=%ld\n",
                 doorsSeen, doorStarts, doorOffers, doorPresses, doorUses, doorOpened, doorBackPresses, doorAdoptions, doorEnd, doorWhy, confirmCalls, confirmForced, confirmBotWaits,
                 confirmPadMismatch, confirmRelays, stationsSeen, stationStarts, stationOffers, stationPresses, stationActivations, stationRefills, stationEnergyGain,
                 stationEnd, stationWhy, usableScanFaults, contextByBot);
}
