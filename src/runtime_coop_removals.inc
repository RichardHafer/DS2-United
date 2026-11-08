// Dead Space 2 United by Hafer.
// The host's removals of scenery reach the clients ([Mirror] SharedRemovals).
//
// 25.09., ch01 psych hall: the client had two props floating at knee height
// that the host no longer had. The host's MultiRemoveTarget 597BD243 fired
// from its own lua step after the fight (its target list: two
// DrivenScriptableProps, 36CB343E/36CB343F, plus three script objects); the
// client's never did, and MultiRemoveTarget is excluded from mirroring and
// dropped from relayed MultiManagers since 17.09., when a replayed
// remove_area01_extras deleted a partner's objects together with his streams.
//
// MultiRemoveTarget::HandleEvents (PC 0x007E5E50, thiscall(msg), ret 4) acts
// when the message id equals attributes+0: for each of the 8 GUIDs at
// attributes+0x0C it marks the entity removed in the EntityDataManager
// ([0x02021300], only with attributes+0x8C bit 0), then walks
// SimManager::FindBase ([0x0201C0A0], 0x00776CA0 thiscall(guid*, prev), ret 8)
// and deletes each match that is not pending delete (0x00756900 thiscall) with
// Base::Delete (0x007568B0 cdecl). The host now reports the targets whose
// live class is scenery ([Mirror] RemovalClasses/RemovalNodes); a client
// repeats exactly those steps for exactly those GUIDs and checks the class of
// its own copy again. NPCs, players, pickups, triggers and streams never go.
//
// Any other deletion of scenery ([Mirror] SharedDeletes), from every machine:
// a prop broken, faded out after its destruction or removed by a script next
// to the local Isaac. 25.09.: two DismemberableProps and a prop were gone on the
// client and still stood on the host. Seen in necro's Base::Delete hook
// (0x007568B0, the one hook on that site); only within DeleteRadius of the
// local Isaac, so a room unloading far away never deletes a partner's, and a
// burst (level teardown) is dropped whole.
namespace removals {
bool enabled = false, installed = false;
constexpr uint32_t kHandleEventsRva = 0x3E5E50, kFindBaseRva = 0x376CA0, kPendingDeleteRva = 0x356900, kDeleteRva = 0x3568B0;
constexpr uint32_t kEdmGetRva = 0x3B6350, kEdmAllocateRva = 0x3B6C60, kSetPersistableRva = 0x3B5DD0;
constexpr uint32_t kSimManagerRva = 0x1C1C0A0, kEntityDataRva = 0x1C21300, kChunkType = 0x137726B7;
constexpr uint32_t kMagic = 0x54564D52;   // 'RMVT'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kFlagPersist = 1, kFlagDeleted = 2;
constexpr unsigned kTargets = 8;
struct Packet {
    uint32_t magic, version, token, sequence, hash, flags, count;
    uint32_t guids[kTargets][4];
};
static_assert(sizeof(Packet) == 156, "Unexpected removal wire layout");

// A player's own props are never mirrored. The helmet is a prop of its own, and
// every player wearing that suit has one with the same GUID: 12.10. 01:00 a body's
// helmet went with it, the partner deleted every helmet of that GUID (the heads
// of all players vanished) and one was deleted twice (heap crash 20088).
volatile LONG playerPropsKept = 0;
bool HelmetGuid(const uint32_t* guid) {
    for (const auto& h : coop_removals_data::kHelmetGuids)
        if (std::memcmp(h, guid, 16) == 0) return true;
    return false;
}
bool HeldBy(uint32_t player, uint32_t object) {
    uint32_t vtable = 0, held = 0;
    if (!player || !Read(player, &vtable, 4) || vtable != base + placement::kPlayerPrimaryVtableRva) return false;
    if (Read(player + 0x168C, &held, 4) && held > 0x10010 && held - 0x10 == object) return true;   // the helmet (SafePtr)
    return Read(player + 0xCEC, &held, 4) && held > 0x10010 && held - 0x10 == object;              // the item in hand
}
bool PlayerOwned(uint32_t object, const uint32_t* guid) {
    if (guid && HelmetGuid(guid)) return true;
    for (unsigned i = 0; i < std::size(placement::playerObjects); ++i)
        if (HeldBy(static_cast<uint32_t>(InterlockedCompareExchange(&placement::playerObjects[i], 0, 0)), object)) return true;
    uint32_t manager = 0, slots = 0, capacity = 0;
    if (!Read(base + kPlayerManagerRva, &manager, 4) || !manager || !Read(manager + 4, &slots, 4) || !slots || !Read(manager + 8, &capacity, 4) || capacity > 8)
        return false;
    for (uint32_t i = 0; i < capacity; ++i) {
        uint32_t bp = 0;
        if (Read(slots + 4 * i, &bp, 4) && bp >= 0x103C0 && HeldBy(bp - 0x3C0, object)) return true;
    }
    return false;
}
bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.count && p.count <= kTargets; }

gate::RuleSet rules, deleteRules, npcRules;
bool sharedDeletes = false, applying = false, inRemoveTarget = false;
float deleteRadius = 25.0f;
uint32_t pendingDeletes[kTargets][4]{};
unsigned pendingDeleteCount = 0;
DWORD burstFrom = 0;
unsigned burstCount = 0;
volatile LONG deletesSeen = 0, deletesQueued = 0, deletesFar = 0, deletesBurst = 0;
safetyhook::InlineHook hook;
uint32_t sequence = 0;
struct Outgoing { Packet packet; DWORD at; unsigned sends; };
Outgoing outbox[8]{};
struct Seen { uint32_t token, sequence; };
Seen seen[64]{};
unsigned seenHead = 0;
volatile LONG fired = 0, packetsSent = 0, guidsSent = 0, withheld = 0, received = 0, duplicates = 0, removed = 0, persisted = 0,
              notLoaded = 0, wrongClass = 0, faults = 0, events = 0;

bool Scenery(uint32_t object, char* cls, size_t clsSize) {
    uint32_t vtable = 0;
    if (!object || !Read(object, &vtable, 4)) return false;
    for (unsigned i = 0; i < rules.nodeCount; ++i)
        if (vtable == base + rules.nodeVtables[i]) { std::snprintf(cls, clsSize, "%s", rules.nodeNames[i]); return true; }
    for (unsigned i = 0; i < rules.entityCount; ++i)
        if (gate::IsA(object, vtable, rules.entities[i].hash)) { std::snprintf(cls, clsSize, "%s", rules.entities[i].name); return true; }
    return false;
}
// The people of a scene ([Mirror] RemovalNpcClasses). 02.11.: the two soldiers of the first
// scene were removed on the host when it ended; the client followed that scene, its own cues
// stayed local, and the host's removal was withheld as "not scenery": the soldiers stood on.
// A removal of one waits while our own Isaac still plays a sequence (they may be its actors).
bool SceneNpc(uint32_t object, char* cls, size_t clsSize) {
    uint32_t vtable = 0;
    if (!object || !Read(object, &vtable, 4)) return false;
    for (unsigned i = 0; i < npcRules.entityCount; ++i) {
        const auto& rule = npcRules.entities[i];
        // NeutralNPC's retail class query has a different prologue from the generic queries.
        const bool exactNeutral = rule.hash == 0x82C7DF9Au && vtable == base + 0x1784EE8;
        if (exactNeutral || gate::IsA(object, vtable, rule.hash)) {
            std::snprintf(cls, clsSize, "%s", rule.name);
            return true;
        }
    }
    return false;
}
constexpr DWORD kNpcWaitMs = 60000;
struct Waiting { uint32_t guid[4]; DWORD since; };
Waiting npcWaiting[16]{};
unsigned npcWaitingCount = 0;
volatile LONG npcsRemoved = 0, npcsWaited = 0, npcsExpired = 0, withheldLines = 0;
bool Clear(const uint32_t* g) {
    return (!g[0] && !g[1] && !g[2] && !g[3]) || (g[0] == 0xBADBADBA && g[1] == 0xBEEFBEEF && g[2] == 0xEAC15A55 && g[3] == 0x91100911);
}
uint32_t FindBase(const uint32_t* guid, uint32_t previous) {
    uint32_t sim = 0;
    if (!Read(base + kSimManagerRva, &sim, 4) || !sim) return 0;
    using Find = uint32_t(__thiscall*)(uint32_t, const uint32_t*, uint32_t);
    return reinterpret_cast<Find>(base + kFindBaseRva)(sim, guid, previous);
}
bool PendingDelete(uint32_t object) {
    using Pending = bool(__thiscall*)(uint32_t);
    return reinterpret_cast<Pending>(base + kPendingDeleteRva)(object);
}
// The EntityDataManager note: the entity stays gone when its room streams in again.
bool MarkRemoved(const uint32_t* guid) {
    uint32_t edm = 0;
    if (!Read(base + kEntityDataRva, &edm, 4) || !edm) return false;
    using Get = uint32_t(__thiscall*)(uint32_t, const uint32_t*, uint32_t, int);
    using Allocate = uint32_t(__thiscall*)(uint32_t, uint32_t, const uint32_t*, uint32_t, int);
    using SetPersistable = void(__thiscall*)(uint32_t, int);
    if (reinterpret_cast<Get>(base + kEdmGetRva)(edm, guid, kChunkType, -1)) return true;
    const uint32_t chunk = reinterpret_cast<Allocate>(base + kEdmAllocateRva)(edm, 0, guid, kChunkType, -1);
    if (!chunk) return false;
    reinterpret_cast<SetPersistable>(base + kSetPersistableRva)(chunk, 1);
    return true;
}

void Queue(const Packet& p, DWORD now);   // below
void Report(uint32_t manager, uint32_t hash) {
    uint32_t attributes = 0, persist = 0;
    if (!Read(manager + 0x0C, &attributes, 4) || !attributes) return;
    Read(attributes + 0x8C, &persist, 1);
    Packet p{kMagic, kVersion, peerReadiness.token, 0, hash, persist & 1, 0, {}};
    char names[128] = "";
    size_t used = 0;
    for (unsigned i = 0; i < kTargets; ++i) {
        uint32_t guid[4]{};
        if (!Read(attributes + 0x0C + i * 16, guid, 16) || Clear(guid)) continue;
        char cls[32] = "";
        const uint32_t object = FindBase(guid, 0);
        if (!object || (!Scenery(object, cls, sizeof(cls)) && !SceneNpc(object, cls, sizeof(cls)))) {
            InterlockedIncrement(&withheld);
            if (object && InterlockedIncrement(&withheldLines) <= 40) {
                uint32_t vtable = 0;
                Read(object, &vtable, 4);
                gate::Event("coop_removals %08X target %08X-%08X (vtable %08X) withheld: not scenery, not a scene NPC", hash, guid[0], guid[1], vtable);
            }
            continue;
        }
        std::memcpy(p.guids[p.count++], guid, 16);
        if (used < sizeof(names) - 40) used += std::snprintf(names + used, sizeof(names) - used, " %08X:%s", guid[1], cls);
    }
    if (!p.count) return;
    p.sequence = ++sequence;
    Queue(p, GetTickCount());
    InterlockedExchangeAdd(&guidsSent, static_cast<LONG>(p.count));
    if (InterlockedIncrement(&events) <= 60)
        gate::Event("coop_removals %08X removes %u scenery target(s) here, sent to the clients:%s", hash, p.count, names);
}

void __fastcall OnHandleEvents(void* self, void*, uint32_t* msg) {
    const auto manager = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t attributes = 0, trigger = 0, id = 0;
    if (enabled && IsHost() && peerTable.Count() && msg && Read(reinterpret_cast<uintptr_t>(msg), &id, 4) &&
        Read(manager + 0x0C, &attributes, 4) && attributes && Read(attributes, &trigger, 4) && id == trigger) {
        InterlockedIncrement(&fired);
        __try { Report(manager, id); } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&faults); }
    }
    // Its own deletions are reported above (host) or stay local (client), not again as deletions.
    const bool outer = inRemoveTarget;
    inRemoveTarget = true;
    hook.thiscall<void>(self, msg);
    inRemoveTarget = outer;
}

void Queue(const Packet& p, DWORD now) {
    unsigned slot = 0;
    for (unsigned i = 1; i < std::size(outbox); ++i)
        if (outbox[i].sends >= 3 || static_cast<LONG>(outbox[i].at - outbox[slot].at) < 0) slot = i;
    outbox[slot] = Outgoing{p, now, 0};
}
// From necro's Base::Delete hook, before the object goes; game thread.
void OnBaseDelete(uint32_t object) {
    if (!sharedDeletes || !installed || applying || inRemoveTarget || !inLevel || !peerTable.Count() || gate::replaying) return;
    uint32_t vtable = 0, guid[4]{};
    if (!Read(object, &vtable, 4) || vtable < base + gate::kRdataLo || vtable >= base + gate::kRdataHi) return;
    char cls[32] = "";
    if (!mirror::ClassOf(object, vtable, deleteRules, cls, sizeof(cls)) && !SceneNpc(object, cls, sizeof(cls))) return;
    InterlockedIncrement(&deletesSeen);
    const DWORD now = GetTickCount();
    if (now - burstFrom > 250) { burstFrom = now; burstCount = 0; }
    if (++burstCount > 12) { InterlockedIncrement(&deletesBurst); pendingDeleteCount = 0; return; }
    const uint32_t local = LocalSlotZeroEntity();
    float at[3]{}, me[3]{};
    if (!local || !Read(object + 0xC0, at, 12) || !Read(local + 0xC0, me, 12)) return;
    const float dx = at[0] - me[0], dy = at[1] - me[1], dz = at[2] - me[2];
    if (dx * dx + dy * dy + dz * dz > deleteRadius * deleteRadius) { InterlockedIncrement(&deletesFar); return; }
    if (!Read(object + 0x24, guid, 16) || Clear(guid)) return;
    if (PlayerOwned(object, guid)) {
        if (InterlockedIncrement(&playerPropsKept) <= 20)
            gate::Event("coop_removals %s %08X-%08X deleted here is a player's own prop (helmet or item): not sent", cls, guid[0], guid[1]);
        return;
    }
    for (unsigned i = 0; i < pendingDeleteCount; ++i)
        if (std::memcmp(pendingDeletes[i], guid, 16) == 0) return;
    if (pendingDeleteCount < kTargets) std::memcpy(pendingDeletes[pendingDeleteCount++], guid, 16);
    InterlockedIncrement(&deletesQueued);
    if (InterlockedIncrement(&events) <= 60)
        gate::Event("coop_removals %s %08X-%08X deleted here next to our Isaac: sent to the partners", cls, guid[0], guid[1]);
}

// The scene NPCs whose removal waited for our sequence to end.
void NpcWaitTick(DWORD now) {
    if (!npcWaitingCount || cutscene::SequenceRunningNow()) return;
    applying = true;
    for (unsigned w = 0; w < npcWaitingCount; ++w) {
        const Waiting& it = npcWaiting[w];
        if (now - it.since > kNpcWaitMs) { InterlockedIncrement(&npcsExpired); continue; }
        __try {
            for (uint32_t object = FindBase(it.guid, 0), guard = 0; object && guard < 8; object = FindBase(it.guid, object), ++guard) {
                char cls[32] = "";
                if (!SceneNpc(object, cls, sizeof(cls)) || PendingDelete(object)) continue;
                reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kDeleteRva)(object);
                InterlockedIncrement(&npcsRemoved);
                InterlockedIncrement(&removed);
                gate::Event("coop_removals %s %08X-%08X removed here after our sequence ended, as on the host", cls, it.guid[0], it.guid[1]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&faults);
        }
    }
    npcWaitingCount = 0;
    applying = false;
}
// Sent at once, again after 100 and 400 ms; a partner folds the copies.
void Tick(DWORD now) {
    if (installed && inLevel) NpcWaitTick(now);
    else npcWaitingCount = 0;
    if (!installed || !peerTable.Count()) return;
    // Deletions of this frame and the ones before, one packet (a burst waits 250 ms to be told from a teardown).
    if (pendingDeleteCount && now - burstFrom > 250) {
        Packet p{kMagic, kVersion, peerReadiness.token, ++sequence, 0, kFlagDeleted, pendingDeleteCount, {}};
        std::memcpy(p.guids, pendingDeletes, pendingDeleteCount * 16);
        InterlockedExchangeAdd(&guidsSent, static_cast<LONG>(pendingDeleteCount));
        pendingDeleteCount = 0;
        Queue(p, now);
    }
    static const DWORD kAfter[] = {0, 100, 400};
    for (auto& o : outbox) {
        if (!o.packet.magic || o.sends >= std::size(kAfter) || now - o.at < kAfter[o.sends]) continue;   // empty slot
        if (SendToPeers(&o.packet, sizeof(o.packet))) InterlockedIncrement(&packetsSent);
        ++o.sends;
    }
}

void Apply(const Packet& p) {
    unsigned gone = 0, marked = 0, absent = 0, refused = 0;
    for (unsigned i = 0; i < p.count; ++i) {
        const uint32_t* guid = p.guids[i];
        if (Clear(guid)) continue;
        if ((p.flags & kFlagPersist) && MarkRemoved(guid)) ++marked;
        bool any = false;
        for (uint32_t object = FindBase(guid, 0), guard = 0; object && guard < 8; object = FindBase(guid, object), ++guard) {
            any = true;
            char cls[32] = "";
            uint32_t vtable = 0;
            const bool npc = SceneNpc(object, cls, sizeof(cls));
            const bool allowed = npc || ((p.flags & kFlagDeleted) ? Read(object, &vtable, 4) && mirror::ClassOf(object, vtable, deleteRules, cls, sizeof(cls))
                                                                  : Scenery(object, cls, sizeof(cls)));
            if (!allowed) { ++refused; continue; }
            if (npc && cutscene::SequenceRunningNow()) {
                bool known = false;
                for (unsigned w = 0; w < npcWaitingCount; ++w) known = known || std::memcmp(npcWaiting[w].guid, guid, 16) == 0;
                if (!known && npcWaitingCount < std::size(npcWaiting)) {
                    std::memcpy(npcWaiting[npcWaitingCount].guid, guid, 16);
                    npcWaiting[npcWaitingCount++].since = GetTickCount();
                    InterlockedIncrement(&npcsWaited);
                    gate::Event("coop_removals %s %08X-%08X removed by the host waits: our Isaac still plays a sequence", cls, guid[0], guid[1]);
                }
                break;
            }
            if (npc && InterlockedIncrement(&npcsRemoved) <= 40) gate::Event("coop_removals %s %08X-%08X removed here as on the host", cls, guid[0], guid[1]);
            if (PlayerOwned(object, guid)) { ++refused; InterlockedIncrement(&playerPropsKept); continue; }
            if (PendingDelete(object)) continue;
            reinterpret_cast<void(__cdecl*)(uint32_t)>(base + kDeleteRva)(object);
            ++gone;
        }
        if (!any) ++absent;
    }
    InterlockedExchangeAdd(&removed, static_cast<LONG>(gone));
    InterlockedExchangeAdd(&persisted, static_cast<LONG>(marked));
    InterlockedExchangeAdd(&notLoaded, static_cast<LONG>(absent));
    InterlockedExchangeAdd(&wrongClass, static_cast<LONG>(refused));
    if (InterlockedIncrement(&events) <= 60)
        gate::Event("coop_removals partner's %s %08X: %u of %u target(s) removed here, %u remembered for later, %u not loaded, %u refused (not scenery here)",
                    (p.flags & kFlagDeleted) ? "deletions" : "MultiRemoveTarget", p.hash, gone, p.count, marked, absent, refused);
}

// Called on the game thread from the receive loop.
void OnPacket(const Packet& p) {
    if (!installed || !inLevel) return;
    // A MultiRemoveTarget report only ever comes from the host; deletions from anybody.
    if ((p.flags & kFlagDeleted) ? !sharedDeletes : (!enabled || IsHost())) return;
    InterlockedIncrement(&received);
    for (const auto& s : seen)
        if (s.token == p.token && s.sequence == p.sequence) { InterlockedIncrement(&duplicates); return; }
    seen[seenHead] = Seen{p.token, p.sequence};
    seenHead = (seenHead + 1) % std::size(seen);
    applying = true;
    __try { Apply(p); } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&faults); }
    applying = false;
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Mirror", "SharedRemovals", 0, ini.c_str()) != 0;
    sharedDeletes = GetPrivateProfileIntA("Mirror", "SharedDeletes", 0, ini.c_str()) != 0;
    deleteRadius = static_cast<float>(GetPrivateProfileIntA("Mirror", "DeleteRadius", 25, ini.c_str()));
    gate::ReadRuleSet(log, ini, deleteRules, "Mirror", "DeleteClasses", "ScriptableProp,DamageableProp,DismemberableProp,BreakableContainer",
                      "DeleteNodes", "");
    gate::ReadRuleSet(log, ini, rules, "Mirror", "RemovalClasses", "ScriptableProp,DamageableProp,DismemberableProp", "RemovalNodes", "ExecuteVFX");
    gate::ReadRuleSet(log, ini, npcRules, "Mirror", "RemovalNpcClasses", "NeutralNPC", "RemovalNpcNodes", "");
    if (enabled || sharedDeletes) {
        // mov eax,[ecx+0Ch] / mov ecx,[esp+4] / mov edx,[ecx] / cmp edx,[eax] / jne
        static const unsigned char kSig[] = {0x8B, 0x41, 0x0C, 0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x11, 0x3B, 0x10, 0x0F, 0x85};
        installed = hook_site::Install(log, "coop_removals MultiRemoveTarget::HandleEvents", base + kHandleEventsRva, kSig, sizeof(kSig),
                                       reinterpret_cast<void*>(OnHandleEvents), hook);
    }
    std::fprintf(log, "coop_removals shared=%d deletes=%d radius=%.0f installed=%d (the host's removals and anybody's deletions of scenery happen on every machine)\n",
                 enabled ? 1 : 0, sharedDeletes ? 1 : 0, deleteRadius, installed ? 1 : 0);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG n = fired + received + faults + deletesSeen;
    if (n == last) return;
    last = n;
    std::fprintf(log, "coop_removals fired=%ld packets_sent=%ld guids_sent=%ld withheld=%ld received=%ld duplicates=%ld removed=%ld persisted=%ld not_loaded=%ld refused=%ld faults=%ld\n",
                 fired, packetsSent, guidsSent, withheld, received, duplicates, removed, persisted, notLoaded, wrongClass, faults);
    std::fprintf(log, "coop_removals deletes seen=%ld queued=%ld far=%ld burst_dropped=%ld player_props_kept=%ld | scene_npcs removed=%ld waited=%ld expired=%ld waiting=%u\n",
                 deletesSeen, deletesQueued, deletesFar, deletesBurst, playerPropsKept, npcsRemoved, npcsWaited, npcsExpired, npcWaitingCount);
}
}  // namespace removals
