// Dead Space 2 United by Hafer.
// Two engine crashes co-op exposes ([Coop] CrashGuards). Dumps of 12.10.:
//
// Submix release (32996, both machines at once on a Versus exit): with
// global_assets and global_mp_assets loaded together there are two submix
// LoadMixPatches. CreateSubmixVoices skips the second, but the release
// (0x00734800, thiscall(patch), ret 4, returns al) only checks that some submix
// is active and then unregisters the patch it was given: the MP one, never
// registered, with null list links. A patch that was never registered
// (+5 == 0, or both links +0x10/+0x14 null) is not released.
//
// Lurker deleted alive (34156, client): the host dropped a living Lurker ghost.
// While the Lurker deletes itself, the engine clears its component table
// (+0x44/+0x48) before destroying the components; the NavSmartObstacle
// component's removal broadcasts iMsgNavObstacleRemoved, the same Lurker
// still listens, and Lurker::HandleEvents (0x01007BC0, thiscall(msg), ret 4)
// calls FindUsableHardpoints on a null health component. With the table gone
// the message is not handled.
//
// Audio session list reset (42944, 25664: the main menu after an MP map): every
// audio config that loads runs ASessionManager::InitSessionPool (0x00730110,
// thiscall, plain ret, al=1), which empties the session list. global_mp_assets
// brings a second config (global_mp.auc) after the map's and the player's banks
// are loaded; their sessions fall out of the list, and unloading one of them
// at the menu unlinks a node that is no longer in it. With live sessions
// (+0x12) the list is kept and only the bank configs are cleared
// (ClearBankConfigs 0x007300A0, what the original does at its end).
//
// Message handle table (dump 7320, 13.10.): the engine's IMsgData handles live in one
// table of 512 slots ([0x0201C0F4] {ptr, handle}, handle = index << 23 | serial, free
// list head [0x0201C0F8], tail [0x0201C0FC]), made once at boot. A burst of about 250
// message objects in a long survival run empties the free list; from then on every new
// one is bound into slot 0. Quartermaster::HandleEvents (0x00DDD490) looks the sender of
// a stream message up there without a check, and a gun stream bought after that came
// back null (AV 0x00DDD4C9). Both lookups are guarded: a sender that is not a stream's
// message data is found again through the stream manager's lists by its handle, or the
// message is left alone. A census once a second names the burst when it happens.
//
// Two NPCs in one wall vent (dumps 22484 and 15744 of 08.10., 44176 of 16.10., always
// 0x00E93CCF): a state handler (thiscall(msg), ret 4) looks up the sub state machine
// E8B1C0E8 in its tree (FindStateMachine 0x013E28A0, ret 8) and sets its flag 2 without
// a null check; the second climber's tree has none. A missing one gets a scratch block,
// so the flag goes nowhere and the handler returns as before.
namespace crashguards {
bool enabled = false;
safetyhook::InlineHook submixHook, lurkerHook, sessionPoolHook;
constexpr uint32_t kSubmixReleaseRva = 0x334800, kLurkerHandleEventsRva = 0xC07BC0, kInitSessionPoolRva = 0x330110, kClearBankConfigsRva = 0x3300A0;
volatile LONG submixSkipped = 0, lurkerSkipped = 0, guardLines = 0, sessionResetsKept = 0, sessionResetFaults = 0;
bool submixInstalled = false, lurkerInstalled = false, sessionPoolInstalled = false;
constexpr uint32_t kQmLookupARva = 0x9DD4C9, kQmLookupBRva = 0x9DD5A0, kQmLeaveRva = 0x9DD574, kStreamMsgDataVtableRva = 0x1718DA8,
                   kStreamManagerRva = 0x1C1C098, kMsgTableRva = 0x1C1C0F4, kMsgHeadRva = 0x1C1C0F8, kMsgTailRva = 0x1C1C0FC,
                   kDamageMsgDataVtableRva = 0x175C74C;
constexpr unsigned kMsgSlots = 512;
safetyhook::MidHook qmLookupAHook, qmLookupBHook;
bool qmGuard = false;
volatile LONG qmLookups = 0, qmRecovered = 0, qmLeft = 0, qmLines = 0;
// The stream record whose message data carries `handle` (record +0x34 data, +0x38 handle).
uint32_t StreamByMsgHandle(uint32_t handle) {
    uint32_t manager = 0;
    if (!handle || !Read(base + kStreamManagerRva, &manager, 4) || !manager) return 0;
    for (const uint32_t list : {0x8Cu, 0x7Cu, 0x74u, 0x84u}) {
        uint32_t rec = 0;
        if (!Read(manager + list, &rec, 4)) continue;
        for (unsigned steps = 0; rec && steps < 1024; ++steps) {
            uint32_t h = 0, next = 0;
            if (Read(rec + 0x38, &h, 4) && h == handle) return rec;
            if (!Read(rec, &next, 4) || next == rec) break;
            rec = next;
        }
    }
    return 0;
}
void OnQmLookup(SafetyHookContext& ctx) {
    InterlockedIncrement(&qmLookups);
    const uint32_t data = static_cast<uint32_t>(ctx.ecx), handle = static_cast<uint32_t>(ctx.eax);
    uint32_t vtable = 0, back = 0;
    if (data && Read(data, &vtable, 4) && vtable == base + kStreamMsgDataVtableRva && Read(data + 0x10, &back, 4) && back == data - 0x34) return;
    const uint32_t rec = StreamByMsgHandle(handle);
    if (rec) {
        ctx.ecx = rec + 0x34;
        InterlockedIncrement(&qmRecovered);
        if (InterlockedIncrement(&qmLines) <= 20)
            gate::Event("crashguards: the Quartermaster's stream message %08X had %s in the handle table; found its stream %08X by the handle", handle,
                        data ? "another object" : "nothing", rec);
        return;
    }
    InterlockedIncrement(&qmLeft);
    if (InterlockedIncrement(&qmLines) <= 20)
        gate::Event("crashguards: the Quartermaster's stream message %08X has no stream (%s in the handle table, vtable %08X): left alone", handle,
                    data ? "another object" : "nothing", vtable);
    ctx.eip = base + kQmLeaveRva;   // pop esi / add esp,0Ch / ret 4: the handler's own exit
}
// The handle table, read only. Bound = the slot's object carries the slot's handle.
LONG msgBound = -1, msgHeadSeen = -1, msgCollapses = 0;
DWORD msgCensusAt = 0;
bool msgCensusDone = false;
void MsgTableCensus(FILE* log) {
    const DWORD now = GetTickCount();
    if (now - msgCensusAt < 1000) return;
    msgCensusAt = now;
    uint32_t table = 0, head = 0, tail = 0;
    if (!Read(base + kMsgTableRva, &table, 4) || !table || !Read(base + kMsgHeadRva, &head, 4) || !Read(base + kMsgTailRva, &tail, 4)) return;
    static uint32_t slots[kMsgSlots * 2];
    if (!Read(table, slots, sizeof(slots))) return;
    LONG bound = 0;
    for (unsigned i = 0; i < kMsgSlots; ++i) {
        const uint32_t ptr = slots[i * 2], handle = slots[i * 2 + 1];
        uint32_t own = 0;
        if (ptr && (handle >> 23) == i && Read(ptr + 4, &own, 4) && own == handle) ++bound;
    }
    const bool collapsed = head == 0;
    if (bound / 16 != msgBound / 16 || static_cast<LONG>(head == 0) != msgHeadSeen) {
        std::fprintf(log, "coop_crashguards message handles: %ld of %u bound, free head %u tail %u, slot 0 serial %u%s\n", bound, kMsgSlots, head, tail,
                     slots[1] & 0x7FFFFF, collapsed ? " - FREE LIST EMPTY: new message objects share slot 0" : "");
        msgBound = bound;
        msgHeadSeen = collapsed ? 1 : 0;
    }
    if (!collapsed || msgCensusDone) return;
    // the first collapse: what fills the table, by vtable (and the owner class of health components)
    msgCensusDone = true;
    InterlockedIncrement(&msgCollapses);
    struct Count { uint32_t vtable; unsigned n; };
    Count kinds[16]{}, owners[16]{};
    auto note = [](Count (&list)[16], uint32_t vtable) {
        for (auto& c : list) {
            if (c.vtable && c.vtable != vtable) continue;
            c.vtable = vtable;
            ++c.n;
            return;
        }
    };
    for (unsigned i = 0; i < kMsgSlots; ++i) {
        const uint32_t ptr = slots[i * 2];
        uint32_t vtable = 0;
        if (!ptr || !Read(ptr, &vtable, 4)) continue;
        note(kinds, vtable);
        uint32_t owner = 0, ownerVtable = 0;
        if (vtable == base + kDamageMsgDataVtableRva && Read(ptr - 0x20 + 0x10, &owner, 4) && owner && Read(owner, &ownerVtable, 4)) note(owners, ownerVtable);
    }
    char line[400];
    int used = std::snprintf(line, sizeof(line), "coop_crashguards message handle census at the collapse (vtable rva:count):");
    for (const auto& c : kinds)
        if (c.vtable && used > 0 && used < static_cast<int>(sizeof(line))) used += std::snprintf(line + used, sizeof(line) - used, " %08X:%u", c.vtable - static_cast<uint32_t>(base), c.n);
    std::fprintf(log, "%s\n", line);
    used = std::snprintf(line, sizeof(line), "coop_crashguards damage message owners (vtable rva:count):");
    for (const auto& c : owners)
        if (c.vtable && used > 0 && used < static_cast<int>(sizeof(line))) used += std::snprintf(line + used, sizeof(line) - used, " %08X:%u", c.vtable - static_cast<uint32_t>(base), c.n);
    std::fprintf(log, "%s\n", line);
}

bool __fastcall OnSubmixRelease(void* provider, void*, uint32_t patch) {
    uint8_t registered = 0;
    uint32_t links[2]{};
    if (patch && Read(patch + 5, &registered, 1) && Read(patch + 0x10, links, sizeof(links)) && (!registered || (!links[0] && !links[1]))) {
        InterlockedIncrement(&submixSkipped);
        if (InterlockedIncrement(&guardLines) <= 40)
            gate::Event("coop_crashguards submix release skipped: patch %08X was never registered (registered=%u links=%08X/%08X)", patch, registered,
                        links[0], links[1]);
        return false;
    }
    return submixHook.thiscall<bool>(provider, patch);
}
int __fastcall OnLurkerEvents(void* self, void*, void* message) {
    uint32_t table[2]{};
    const auto lurker = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (lurker && Read(lurker + 0x44, table, sizeof(table)) && !table[0] && !table[1]) {
        InterlockedIncrement(&lurkerSkipped);
        uint32_t hash = 0;
        Read(reinterpret_cast<uintptr_t>(message), &hash, 4);
        if (InterlockedIncrement(&guardLines) <= 40)
            gate::Event("coop_crashguards Lurker %08X is being deleted (no components): message %08X (%s) not handled", lurker, hash, coop_names::Name(hash));
        return 0;
    }
    return lurkerHook.thiscall<int>(self, message);
}

__declspec(noinline) bool ClearBankConfigs(void* manager) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(base + kClearBankConfigsRva)(manager);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool __fastcall OnInitSessionPool(void* manager, void*) {
    uint16_t live = 0;
    const auto m = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(manager));
    if (m && Read(m + 0x12, &live, 2) && live) {
        InterlockedIncrement(&sessionResetsKept);
        const bool cleared = ClearBankConfigs(manager);
        if (!cleared) InterlockedIncrement(&sessionResetFaults);
        if (InterlockedIncrement(&guardLines) <= 40)
            gate::Event("coop_crashguards audio session list kept: %u sessions live when another audio config loaded (caller %08X)%s", live,
                        static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress())), cleared ? "" : ", clearing the bank configs FAULTED");
        return true;
    }
    return sessionPoolHook.thiscall<bool>(manager);
}

// AptResourceHandler::UnloadResource 0x007090F0 (thiscall(resource) ret 4): an APX
// resource ([res] 0x3033FBE9) resets its AnimationManager slot [res+8] with
// InitAptInfo(-1). 15.10. (dump 26564, at the level exit after five checkpoint restarts on
// mp_bombRun): that slot lay in a manager already destroyed and freed (the 192-slot array,
// 43 KB, slot 0 basescreen), the -1 landed on the free chunk's back link, and the next free
// in that bin crashed (0x013F9F03). A slot outside the live manager ([0x0204199C], slots at
// +4, 0xE0 each) is left alone.
constexpr uint32_t kAptUnloadRva = 0x3090F0, kAptRegistryRva = 0x1C4199C, kAptNoSlotRva = 0x1AC0620, kAptSlotSize = 0xE0, kAptSlots = 192,
                   kAptMovie = 0x3033FBE9;
safetyhook::InlineHook aptUnloadHook;
bool aptInstalled = false;
volatile LONG aptUnloads = 0, aptStale = 0;
void __fastcall OnAptUnload(void* handler, void*, void* resource) {
    const uint32_t res = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resource));
    uint32_t kind = 0, slot = 0, none = 0, live = 0;
    if (res && Read(res, &kind, 4) && kind == kAptMovie && Read(res + 8, &slot, 4) && slot && Read(base + kAptNoSlotRva, &none, 4) && slot != none) {
        InterlockedIncrement(&aptUnloads);
        Read(base + kAptRegistryRva, &live, 4);
        const bool inside = live && slot >= live + 4 && slot < live + 4 + kAptSlots * kAptSlotSize && (slot - live - 4) % kAptSlotSize == 0;
        if (!inside) {
            InterlockedIncrement(&aptStale);
            if (InterlockedIncrement(&guardLines) <= 40)
                gate::Event("coop_crashguards Apt movie unload left alone: resource %08X keeps slot %08X, the live movie registry is %08X (caller %08X)", res, slot, live,
                            static_cast<unsigned>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
            return;
        }
    }
    aptUnloadHook.thiscall<void>(handler, resource);
}

constexpr uint32_t kVentFlagFnRva = 0xA93CC0, kVentFlagSiteRva = 0xA93CCF;
safetyhook::MidHook ventFlagHook;
bool ventFlagInstalled = false;
volatile LONG ventFlagGuards = 0, ventFlagLines = 0;
uint32_t ventFlagScratch[32]{};
void OnVentFlag(SafetyHookContext& ctx) {
    if (ctx.eax) return;
    ctx.eax = static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(ventFlagScratch));
    InterlockedIncrement(&ventFlagGuards);
    if (InterlockedIncrement(&ventFlagLines) > 20) return;
    uint32_t caller = 0, msg = 0;
    const uint32_t sp = static_cast<uint32_t>(ctx.esp);
    Read(sp, &caller, 4);
    Read(sp + 4, &msg, 4);
    gate::Event("coop_crashguards state machine E8B1C0E8 missing from a state handler's tree (two NPCs in one vent?): its flag dropped; caller %08X message %08X "
                "esi %08X edi %08X, tick %lu", caller, msg, static_cast<uint32_t>(ctx.esi), static_cast<uint32_t>(ctx.edi), GetTickCount());
}

// Player::RegisterAttacker (0x00892AD0, thiscall(Player*, Entity*), ret 4, bool) adds an enemy
// to the Player's waiting list (8-byte SafePtrs from +0x7C0, room for 48, count byte at +0x940)
// and checks only that list. ProcessAttackerSlots (0x0087A5D0) then moves the holders of the
// three attack slots (+0x730, 0x30 apart) back into it with no check at all: past 48 the next
// entry is the count byte itself (dump 8396, 18.10.: count 49, the unlink at 0x00742A04 read
// through 0x30000A31; a horde of pack enemies turned to the human after F9 sent the bots away).
// The list and the slots together stay at 48: one more is refused as if the list were full.
constexpr uint32_t kRegisterAttackerRva = 0x492AD0, kAttackerCount = 0x940, kAttackerSlots = 0x730, kAttackerSlotStride = 0x30;
constexpr unsigned kAttackerRoom = 48;
safetyhook::InlineHook registerAttackerHook;
bool attackerGuard = false;
volatile LONG attackersRegistered = 0, attackersRefused = 0, attackerLines = 0, attackerPeak = 0;
bool __fastcall OnRegisterAttacker(void* player, void*, void* attacker) {
    const uint32_t p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(player));
    unsigned char waiting = 0;
    if (p && Read(p + kAttackerCount, &waiting, 1)) {
        unsigned held = 0;
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t slot = 0;
            if (Read(p + kAttackerSlots + i * kAttackerSlotStride, &slot, 4) && slot) ++held;
        }
        const LONG total = static_cast<LONG>(waiting + held);
        if (total > InterlockedCompareExchange(&attackerPeak, 0, 0)) InterlockedExchange(&attackerPeak, total);
        if (waiting + held + 1 > kAttackerRoom) {
            InterlockedIncrement(&attackersRefused);
            if (InterlockedIncrement(&attackerLines) <= 20)
                gate::Event("coop_crashguards attacker list of player %08X full: %u waiting and %u in the attack slots, enemy %08X not registered "
                            "(the engine would write past the list)", p, static_cast<unsigned>(waiting), held,
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(attacker)));
            return false;
        }
    }
    InterlockedIncrement(&attackersRegistered);
    return registerAttackerHook.thiscall<bool>(player, attacker);
}

// ZoneWorld::Add (0x007EE6A0, thiscall(ZoneWorld*, ZoneRender*), ret 4) reads this+0x60 at once.
// ZoneRender::PostSpawnInit (0x007FDDB0) passes the global ZoneWorld [0x0204D554], which is null
// while a checkpoint restart rebuilds the world: an entity spawned in that window (a message in
// the dispatch) crashed the game right after a Hold the Zone game over (dump 71772, 02.11.).
// With no world there is nothing to add it to; the new world registers what it renders.
constexpr uint32_t kZoneWorldAddRva = 0x3EE6A0;
safetyhook::InlineHook zoneWorldAddHook;
bool zoneWorldGuard = false;
volatile LONG zoneWorldNulls = 0, zoneWorldLines = 0;
int __fastcall OnZoneWorldAdd(void* world, void*, void* render) {
    if (!world) {
        InterlockedIncrement(&zoneWorldNulls);
        if (InterlockedIncrement(&zoneWorldLines) <= 10)
            gate::Event("coop_crashguards ZoneWorld::Add without a zone world (a spawn while the world is rebuilt): render %08X not added, caller %08X",
                        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(render)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return 0;
    }
    return zoneWorldAddHook.thiscall<int>(world, render);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "CrashGuards", 1, ini.c_str()) != 0;
    if (enabled) {
        // push esi / mov esi,[ecx+60h] / xor eax,eax / push edi / mov edi,[esp+0Ch] / test esi,esi
        static const unsigned char kZoneWorldAdd[] = {0x56, 0x8B, 0x71, 0x60, 0x33, 0xC0, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x85, 0xF6};
        zoneWorldGuard = hook_site::Install(log, "coop_crashguards ZoneWorld::Add", base + kZoneWorldAddRva, kZoneWorldAdd, sizeof(kZoneWorldAdd),
                                            reinterpret_cast<void*>(OnZoneWorldAdd), zoneWorldAddHook);
        // push esi / push edi / mov edi,[esp+0Ch] / mov esi,ecx / test edi,edi / je +3Ah
        static const unsigned char kRegisterAttacker[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1, 0x85, 0xFF, 0x74, 0x3A};
        attackerGuard = hook_site::Install(log, "coop_crashguards Player::RegisterAttacker", base + kRegisterAttackerRva, kRegisterAttacker,
                                           sizeof(kRegisterAttacker), reinterpret_cast<void*>(OnRegisterAttacker), registerAttackerHook);
        // mov ecx,[ecx+0xC] / push 0 / push 0xE8B1C0E8 / call FindStateMachine, then or [eax+0x5C],2 / xor al,al / ret 4
        static const unsigned char kVentFn[] = {0x8B, 0x49, 0x0C, 0x6A, 0x00, 0x68, 0xE8, 0xC0, 0xB1, 0xE8, 0xE8, 0xD1, 0xEB, 0x54, 0x00};
        static const unsigned char kVentSite[] = {0x83, 0x48, 0x5C, 0x02, 0x32, 0xC0, 0xC2, 0x04, 0x00};
        unsigned char head[sizeof(kVentFn)]{};
        if (Read(base + kVentFlagFnRva, head, sizeof(head)) && std::memcmp(head, kVentFn, sizeof(kVentFn)) == 0)
            ventFlagInstalled = hook_site::InstallMid(log, "coop_crashguards vent state flag (0x00E93CCF)", base + kVentFlagSiteRva, kVentSite, sizeof(kVentSite),
                                                      OnVentFlag, ventFlagHook);
        else
            std::fprintf(log, "coop_crashguards vent state flag: function at %08X does not match; left alone\n", static_cast<unsigned>(base + kVentFlagFnRva));
        // sub esp,8 / push esi / mov esi,ecx / xor al,al / cmp byte [esi+0x9E4],1
        static const unsigned char kSubmix[] = {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x32, 0xC0, 0x80, 0xBE, 0xE4, 0x09, 0x00, 0x00, 0x01};
        // sub esp,0x14 / push ebp / push esi / push edi / push 0 / mov esi,ecx / call FindUsableHardpoints
        static const unsigned char kLurker[] = {0x83, 0xEC, 0x14, 0x55, 0x56, 0x57, 0x6A, 0x00, 0x8B, 0xF1, 0xE8};
        submixInstalled = hook_site::Install(log, "coop_crashguards SoundProviderRWAC2 submix release", base + kSubmixReleaseRva, kSubmix, sizeof(kSubmix),
                                             reinterpret_cast<void*>(OnSubmixRelease), submixHook);
        lurkerInstalled = hook_site::Install(log, "coop_crashguards Lurker::HandleEvents", base + kLurkerHandleEventsRva, kLurker, sizeof(kLurker),
                                             reinterpret_cast<void*>(OnLurkerEvents), lurkerHook);
        // sub esp,0xC / push ebx / push esi / push edi / mov edi,ecx / mov eax,[edi+0x4020] / lea ecx,[edi+8] / push 0x400 / push ecx
        static const unsigned char kPool[] = {0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87, 0x20, 0x40, 0x00, 0x00, 0x8D, 0x4F, 0x08, 0x68,
                                              0x00, 0x04, 0x00, 0x00, 0x51};
        sessionPoolInstalled = hook_site::Install(log, "coop_crashguards ASessionManager::InitSessionPool", base + kInitSessionPoolRva, kPool, sizeof(kPool),
                                                  reinterpret_cast<void*>(OnInitSessionPool), sessionPoolHook);
        // mov ecx,[esp+4] / mov eax,[ecx] / cmp eax,0x3A063031 / je / cmp eax,0x378CE1FC
        static const unsigned char kAptUnload[] = {0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x01, 0x3D, 0x31, 0x30, 0x06, 0x3A, 0x74, 0x15, 0x3D, 0xFC, 0xE1};
        aptInstalled = hook_site::Install(log, "coop_crashguards AptResourceHandler::UnloadResource", base + kAptUnloadRva, kAptUnload, sizeof(kAptUnload),
                                          reinterpret_cast<void*>(OnAptUnload), aptUnloadHook);
        // mov eax,[ecx+10h] / push ebp / xor ebp,ebp / mov [esp+18h],ebp
        static const unsigned char kLookupA[] = {0x8B, 0x41, 0x10, 0x55, 0x33, 0xED, 0x89, 0x6C, 0x24, 0x18};
        // mov eax,[ecx+10h] / mov dword [esp+14h],0
        static const unsigned char kLookupB[] = {0x8B, 0x41, 0x10, 0xC7, 0x44, 0x24, 0x14, 0x00, 0x00, 0x00, 0x00};
        static const unsigned char kLeave[] = {0x5E, 0x83, 0xC4, 0x0C, 0xC2, 0x04, 0x00};
        unsigned char leave[sizeof(kLeave)]{};
        if (Read(base + kQmLeaveRva, leave, sizeof(leave)) && std::memcmp(leave, kLeave, sizeof(kLeave)) == 0)
            qmGuard = hook_site::InstallMid(log, "coop_crashguards Quartermaster stream lookup (loaded)", base + kQmLookupARva, kLookupA, sizeof(kLookupA), OnQmLookup,
                                            qmLookupAHook) &&
                      hook_site::InstallMid(log, "coop_crashguards Quartermaster stream lookup (state)", base + kQmLookupBRva, kLookupB, sizeof(kLookupB), OnQmLookup,
                                            qmLookupBHook);
    }
    std::fprintf(log, "coop_crashguards enabled=%d submix_release=%d lurker_events=%d session_pool=%d (an unregistered submix patch is not released; a "
                      "Lurker deleting itself handles no more messages; a second audio config keeps the live sessions)\n", enabled ? 1 : 0,
                 submixInstalled ? 1 : 0, lurkerInstalled ? 1 : 0, sessionPoolInstalled ? 1 : 0);
    std::fprintf(log, "coop_crashguards quartermaster_lookup=%d (a stream message whose sender the handle table lost finds its stream by the handle, or is left alone)\n",
                 qmGuard ? 1 : 0);
    std::fprintf(log, "coop_crashguards apt_unload=%d (an Apt movie whose registry slot is not in the live registry is not reset)\n", aptInstalled ? 1 : 0);
    std::fprintf(log, "coop_crashguards vent_state_flag=%d (a state handler whose sub state machine is missing sets no flag instead of crashing at 0x00E93CCF)\n",
                 ventFlagInstalled ? 1 : 0);
    std::fprintf(log, "coop_crashguards attacker_list=%d (a Player's waiting attackers and attack slots stay at %u together; the engine checks only the list)\n",
                 attackerGuard ? 1 : 0, kAttackerRoom);
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    MsgTableCensus(log);
    {
        static LONG lastNulls = 0;
        const LONG nulls = InterlockedCompareExchange(&zoneWorldNulls, 0, 0);
        if (nulls != lastNulls) {
            lastNulls = nulls;
            std::fprintf(log, "coop_crashguards zone_world_add without a world=%ld (guard %s)\n", nulls, zoneWorldGuard ? "on" : "OFF");
        }
    }
    {
        static LONG lastAttackers = -1;
        const LONG a = InterlockedCompareExchange(&attackersRefused, 0, 0) * 64 + InterlockedCompareExchange(&attackerPeak, 0, 0);
        if (a != lastAttackers && attackerGuard) {
            lastAttackers = a;
            std::fprintf(log, "coop_crashguards attackers registered=%ld refused_list_full=%ld peak_on_one_player=%ld of %u\n",
                         InterlockedCompareExchange(&attackersRegistered, 0, 0), InterlockedCompareExchange(&attackersRefused, 0, 0),
                         InterlockedCompareExchange(&attackerPeak, 0, 0), kAttackerRoom);
        }
    }
    {
        static LONG lastQm = -1;
        const LONG qm = InterlockedCompareExchange(&qmRecovered, 0, 0) * 3 + InterlockedCompareExchange(&qmLeft, 0, 0);
        if (qm != lastQm) {
            lastQm = qm;
            std::fprintf(log, "coop_crashguards quartermaster lookups=%ld recovered=%ld left_alone=%ld handle_table_collapses=%ld\n", InterlockedCompareExchange(&qmLookups, 0, 0),
                         InterlockedCompareExchange(&qmRecovered, 0, 0), InterlockedCompareExchange(&qmLeft, 0, 0), InterlockedCompareExchange(&msgCollapses, 0, 0));
        }
    }
    {
        // when the live registry is rebuilt (the movies loaded before keep their old slots)
        static uint32_t lastRegistry = 0xFFFFFFFFu;
        uint32_t registry = 0;
        Read(base + kAptRegistryRva, &registry, 4);
        if (registry != lastRegistry && aptInstalled) {
            std::fprintf(log, "coop_crashguards Apt movie registry %08X -> %08X (tick %lu)\n", lastRegistry, registry, GetTickCount());
            lastRegistry = registry;
        }
        static LONG lastApt = -1;
        const LONG apt = InterlockedCompareExchange(&aptUnloads, 0, 0) + InterlockedCompareExchange(&aptStale, 0, 0) * 1000;
        if (apt != lastApt && aptInstalled) {
            lastApt = apt;
            std::fprintf(log, "coop_crashguards apt movie unloads=%ld stale_slots_left_alone=%ld\n", InterlockedCompareExchange(&aptUnloads, 0, 0),
                         InterlockedCompareExchange(&aptStale, 0, 0));
        }
    }
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&submixSkipped, 0, 0) + InterlockedCompareExchange(&lurkerSkipped, 0, 0) +
                     InterlockedCompareExchange(&sessionResetsKept, 0, 0) + InterlockedCompareExchange(&ventFlagGuards, 0, 0) * 7;
    if (now == last) return;
    last = now;
    std::fprintf(log, "coop_crashguards submix_releases_skipped=%ld lurker_messages_skipped=%ld session_resets_kept=%ld session_reset_faults=%ld vent_state_flags_dropped=%ld\n",
                 InterlockedCompareExchange(&submixSkipped, 0, 0), InterlockedCompareExchange(&lurkerSkipped, 0, 0),
                 InterlockedCompareExchange(&sessionResetsKept, 0, 0), InterlockedCompareExchange(&sessionResetFaults, 0, 0),
                 InterlockedCompareExchange(&ventFlagGuards, 0, 0));
}
}  // namespace crashguards
