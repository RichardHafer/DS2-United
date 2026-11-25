// Dead Space 2 United by Richard Hafer.
// Dead entries in the player's objective list ([Coop] ObjectiveListGuard).
//
// Reported 19.11. for chapter 10: after the tripod section the mission update at the lift
// crashed in ObjectiveList::AddObjective (0x010CF680) at 0x010CF6D5, five times with a
// different value read from the list's first entry. AddObjective sorts the new entry into the
// list (head at +0x18, nodes {entry, next, prev}) by [[entry+0xC]+0x48]; one entry had been
// freed while its node stayed. An entry leaves the list in its destructor (0x010CF240) only
// through the one list it remembers at +0x454, so an entry destroyed while +0x454 named another
// list, or memory released without the destructor, leaves such a node.
//
// Before every AddObjective the nodes are checked: an ObjectiveEntry has exactly the three
// vtables its constructor (0x010A419E) and destructor write (+0x00, +0x10, +0x58; no class
// derives from it). A node whose entry fails that is taken out with the list's own erase
// (0x0109D400, thiscall(list, result*, iterator*), ret 8, never reads the entry) and the
// objective count at +0x30 drops, as RemoveObjective (0x010C0710) does it. The last destructor
// calls are kept, so the log says whether a dead entry ever ran its destructor.
namespace objguard {
constexpr uint32_t kAddObjectiveRva = 0xCCF680, kEntryDtorRva = 0xCCF240, kEraseRva = 0xC9D400;
constexpr uint32_t kVtable0 = 0x1776CF8, kVtable10 = 0x1776CF4, kVtable58 = 0x1776D20;
constexpr uint32_t kListNodes = 0x18, kListCount = 0x30, kEntryList = 0x454;
bool enabled = true, installed = false;
safetyhook::InlineHook addHook, dtorHook;
volatile LONG adds = 0, scans = 0, deadFound = 0, deadRemoved = 0, eraseFaults = 0, dtors = 0, lines = 0;

struct Destroyed { uint32_t entry = 0, list = 0; DWORD at = 0; };
Destroyed destroyed[64]{};
LONG destroyedNext = 0;

bool LiveEntry(uint32_t entry) {
    uint32_t v0 = 0, v10 = 0, v58 = 0;
    return entry && Read(entry, &v0, 4) && Read(entry + 0x10, &v10, 4) && Read(entry + 0x58, &v58, 4) && v0 == base + kVtable0 &&
           v10 == base + kVtable10 && v58 == base + kVtable58;
}
const Destroyed* WasDestroyed(uint32_t entry) {
    for (const Destroyed& d : destroyed)
        if (d.entry == entry) return &d;
    return nullptr;
}
__declspec(noinline) bool Erase(uint32_t nodes, uint32_t node, uint32_t& next) {
    __try {
        uint32_t iterator = node, result = 0;
        reinterpret_cast<uint32_t*(__thiscall*)(void*, uint32_t*, uint32_t*)>(base + kEraseRva)(reinterpret_cast<void*>(static_cast<uintptr_t>(nodes)), &result,
                                                                                               &iterator);
        next = result;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void Scrub(uint32_t list) {
    InterlockedIncrement(&scans);
    const uint32_t nodes = list + kListNodes;
    uint32_t node = 0;
    if (!Read(nodes, &node, 4)) return;
    for (unsigned guard = 0; node && guard < 512; ++guard) {
        uint32_t entry = 0, next = 0;
        if (!Read(node, &entry, 4) || !Read(node + 4, &next, 4)) return;
        if (LiveEntry(entry)) { node = next; continue; }
        InterlockedIncrement(&deadFound);
        uint32_t first = 0, remembered = 0;
        Read(entry, &first, 4);
        Read(entry + kEntryList, &remembered, 4);
        const Destroyed* d = WasDestroyed(entry);
        if (!Erase(nodes, node, next)) { InterlockedIncrement(&eraseFaults); return; }
        int32_t count = 0;
        if (Read(list + kListCount, &count, 4) && count > 0) { --count; WriteBytes(list + kListCount, &count, 4); }
        InterlockedIncrement(&deadRemoved);
        if (InterlockedIncrement(&lines) <= 40) {
            if (d)
                gate::Event("objguard: dead objective entry %08X removed from list %08X (first dword %08X, +0x454 %08X); its destructor ran %lu ms ago with "
                            "+0x454 %08X (this list +0xC is %08X)",
                            entry, list, first, remembered, static_cast<unsigned long>(GetTickCount() - d->at), d->list, list + 0xC);
            else
                gate::Event("objguard: dead objective entry %08X removed from list %08X (first dword %08X, +0x454 %08X); no destructor seen for it "
                            "(memory released without it, or before the last 64 destructions)",
                            entry, list, first, remembered);
        }
        node = next;
    }
}
uint32_t __fastcall OnAddObjective(void* list, void*, uint32_t entry) {
    InterlockedIncrement(&adds);
    if (enabled) {
        __try { Scrub(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(list))); }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&eraseFaults); }
    }
    return addHook.thiscall<uint32_t>(list, entry);
}
void __fastcall OnEntryDtor(void* entry, void*) {
    const uint32_t e = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry));
    uint32_t list = 0;
    Read(e + kEntryList, &list, 4);
    InterlockedIncrement(&dtors);
    const LONG slot = (destroyedNext++) & 63;
    destroyed[slot] = Destroyed{e, list, GetTickCount()};
    dtorHook.thiscall<void>(entry);
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "ObjectiveListGuard", 1, ini.c_str()) != 0;
    if (!enabled) { std::fprintf(log, "objguard off ([Coop] ObjectiveListGuard=0)\n"); return; }
    // sub esp,8 / push ebp / mov ebp,[esp+10h] / push edi / mov edi,ecx
    static const unsigned char kAdd[] = {0x83, 0xEC, 0x08, 0x55, 0x8B, 0x6C, 0x24, 0x10, 0x57, 0x8B, 0xF9};
    // sub esp,14h / push ebp / push esi / mov esi,ecx / push edi
    static const unsigned char kDtor[] = {0x83, 0xEC, 0x14, 0x55, 0x56, 0x8B, 0xF1, 0x57};
    installed = hook_site::Install(log, "objguard ObjectiveList::AddObjective", base + kAddObjectiveRva, kAdd, sizeof(kAdd), reinterpret_cast<void*>(OnAddObjective),
                                   addHook);
    const bool dtorOk = installed && hook_site::Install(log, "objguard ObjectiveEntry destructor", base + kEntryDtorRva, kDtor, sizeof(kDtor),
                                                        reinterpret_cast<void*>(OnEntryDtor), dtorHook);
    std::fprintf(log, "objguard %s: dead objective entries are taken out before each AddObjective; destructors %s\n", installed ? "on" : "NOT installed",
                 dtorOk ? "recorded" : "NOT recorded");
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG now = InterlockedCompareExchange(&adds, 0, 0) + InterlockedCompareExchange(&deadFound, 0, 0) * 1000 + InterlockedCompareExchange(&dtors, 0, 0) * 7;
    if (now == last) return;
    last = now;
    std::fprintf(log, "objguard adds=%ld scans=%ld dead_found=%ld removed=%ld erase_faults=%ld destructors=%ld\n", InterlockedCompareExchange(&adds, 0, 0),
                 InterlockedCompareExchange(&scans, 0, 0), InterlockedCompareExchange(&deadFound, 0, 0), InterlockedCompareExchange(&deadRemoved, 0, 0),
                 InterlockedCompareExchange(&eraseFaults, 0, 0), InterlockedCompareExchange(&dtors, 0, 0));
}
}  // namespace objguard
