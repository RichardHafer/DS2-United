// Dead Space 2 United by Richard Hafer.
namespace weapon_probe {
safetyhook::InlineHook hook;
volatile LONG calls = 0, missing = 0, invalid = 0, wrongOwner = 0, dropped = 0;
struct Sample {
    uint32_t player, hash, ammo, plasma, active, pending, readable;
    coop_weapon::Result result;
    coop_weapon::Identity identity;
};
std::mutex mutex;
Sample samples[8]{};
unsigned count = 0;

// The last hash each Player was told to hold, kept past the report flush so
// the ghost watch can ask for it again.
struct Held { uint32_t player, hash, ammo, plasma; DWORD at; };
Held held[8]{};
unsigned heldCount = 0;
bool HeldBy(uint32_t player, Held& out) {
    std::lock_guard<std::mutex> lock(mutex);
    for (unsigned i = 0; i < heldCount; ++i) if (held[i].player == player) { out = held[i]; return true; }
    return false;
}

// Who changes a gun's owner. 24.09. (dump 24692): the ghost's plasma cutter
// lay next to it with no owner, out of its inventory, while the ghost's
// pending weapon still pointed at it and its active weapon was empty. The
// owner setter (0x00DC09B0, thiscall(gun, Sentient* owner), ret 4) is called
// by the net unpack through the gun's vtable +0x1FC (0x00DC8960, returning to
// 0x00DC896E) and by the game's own pickup and drop paths.
safetyhook::InlineHook ownerHook;
volatile LONG ownerCalls = 0, ownerCleared = 0, ownerMoved = 0, ownerFromNet = 0;
struct OwnerEvent { uint32_t gun, name, from, to, caller, outer; DWORD at; };
OwnerEvent ownerEvents[32]{};
unsigned ownerEventCount = 0;
volatile LONG ownerEventsDropped = 0;
void __fastcall OnOwner(void* self, void*, uint32_t owner) {
    const auto gun = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    uint32_t ref = 0, name = 0, outer = 0;
    Read(gun + 0x240, &ref, 4);
    Read(gun + 0x25C, &name, 4);
    const uint32_t from = ref ? ref - 0x10 : 0;
    const auto caller = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const bool fromNet = caller == static_cast<uint32_t>(base) + 0x9C896E;
    if (fromNet) Read(reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) + 0x10, &outer, 4);
    InterlockedIncrement(&ownerCalls);
    if (from != owner) {
        InterlockedIncrement(owner ? &ownerMoved : &ownerCleared);
        if (fromNet) InterlockedIncrement(&ownerFromNet);
        std::lock_guard<std::mutex> lock(mutex);
        if (ownerEventCount < std::size(ownerEvents))
            ownerEvents[ownerEventCount++] = OwnerEvent{gun, name, from, owner, caller, outer, GetTickCount()};
        else InterlockedIncrement(&ownerEventsDropped);
    }
    ownerHook.thiscall<void>(self, owner);
}
void __fastcall OnUpdate(void* self, void*, uint32_t hash, uint32_t ammo, uint32_t plasma) {
    const auto player = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    const auto result = coop_weapon::Find(player, hash,
        static_cast<uint32_t>(base + placement::kPlayerPrimaryVtableRva),
        [](uint32_t a, void* out, size_t n) { return Read(a, out, n); });
    InterlockedIncrement(&calls);
    if (result.status == coop_weapon::Status::Missing) InterlockedIncrement(&missing);
    if (result.status == coop_weapon::Status::Invalid) InterlockedIncrement(&invalid);
    if (result.status == coop_weapon::Status::Found && result.owner != player)
        InterlockedIncrement(&wrongOwner);
    Sample sample{player, hash, ammo & 0xFFFF, plasma & 0xFF, 0, 0, 0, result, {}};
    if (result.status == coop_weapon::Status::Found)
        sample.identity = coop_weapon::InspectIdentity(result.item, static_cast<uint32_t>(base),
            [](uint32_t a, void* out, size_t n) { return Read(a, out, n); });
    hook.thiscall<void>(self, hash, ammo, plasma);
    if (Read(player + 0xCEC, &sample.active, 4)) sample.readable |= 1;
    if (Read(player + 0x424, &sample.pending, 4)) sample.readable |= 2;
    // Keep only the latest sample per Player between worker reports.
    std::lock_guard<std::mutex> lock(mutex);
    {
        // Bodies come and go with every level and restart: when full, the stalest makes room.
        unsigned i = 0;
        while (i < heldCount && held[i].player != player) ++i;
        if (i == std::size(held)) {
            i = 0;
            for (unsigned j = 1; j < heldCount; ++j) if (static_cast<LONG>(held[j].at - held[i].at) < 0) i = j;
        }
        held[i] = Held{player, hash, ammo, plasma, GetTickCount()};
        if (i == heldCount) ++heldCount;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (samples[i].player == player) { samples[i] = sample; return; }
    }
    if (count < std::size(samples)) samples[count++] = sample;
    else InterlockedIncrement(&dropped);
}

void Install(FILE* log, const std::string& ini) {
    const bool requested = GetPrivateProfileIntA("Coop", "GhostWeaponProbe", 0, ini.c_str()) != 0;
    std::fprintf(log, "coop_weapon_probe config=%d mutation=none\n", requested);
    if (!requested) return;
    const unsigned char expected[] = {0x8B,0x44,0x24,0x04,0x53,0x55,0x56,0x57,
        0x8B,0xD9,0x50,0x8D,0x8B,0x04,0x0C,0x00,0x00};
    unsigned char actual[sizeof(expected)]{};
    if (!Read(base + 0x46BAD0, actual, sizeof(actual)) ||
        std::memcmp(actual, expected, sizeof(actual))) {
        std::fprintf(log, "coop_weapon_probe signature_mismatch; disabled\n"); return;
    }
    hook = safetyhook::create_inline(reinterpret_cast<void*>(base + 0x46BAD0),
                                   reinterpret_cast<void*>(OnUpdate));
    std::fprintf(log, "coop_weapon_probe signature_ok hook=%d\n", static_cast<bool>(hook));
    // push ebx / mov ebx,ecx / mov eax,[ebx+0x240] / push edi / lea edi,[ebx+0x240]
    static const unsigned char kOwner[] = {0x53, 0x8B, 0xD9, 0x8B, 0x83, 0x40, 0x02, 0x00, 0x00, 0x57, 0x8D, 0xBB, 0x40, 0x02, 0x00, 0x00};
    hook_site::Install(log, "coop_weapon_probe gun owner", base + 0x9C09B0, kOwner, sizeof(kOwner),
                       reinterpret_cast<void*>(OnOwner), ownerHook);
}

void Flush(FILE* log) {
    if (!hook) return;
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    Sample pending[8]{};
    unsigned size = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        size = count; std::copy_n(samples, size, pending); count = 0;
    }
    OwnerEvent events[std::size(ownerEvents)]{};
    unsigned eventCount = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        eventCount = ownerEventCount; std::copy_n(ownerEvents, eventCount, events); ownerEventCount = 0;
    }
    for (unsigned i = 0; i < eventCount; ++i) {
        const auto& e = events[i];
        std::fprintf(log, "coop_weapon_owner t=%lu gun=%08X name=%08X from=%08X to=%08X caller=%08X%s outer=%08X\n",
                     static_cast<unsigned long>(e.at), e.gun, e.name, e.from, e.to, e.caller,
                     e.caller == static_cast<uint32_t>(base) + 0x9C896E ? " (net unpack)" : "", e.outer);
    }
    if (ownerHook)
        std::fprintf(log, "coop_weapon_owner calls=%ld cleared=%ld moved=%ld from_net=%ld dropped=%ld\n",
                     InterlockedCompareExchange(&ownerCalls, 0, 0), InterlockedCompareExchange(&ownerCleared, 0, 0),
                     InterlockedCompareExchange(&ownerMoved, 0, 0), InterlockedCompareExchange(&ownerFromNet, 0, 0),
                     InterlockedCompareExchange(&ownerEventsDropped, 0, 0));
    if (!size) return;
    for (unsigned i = 0; i < size; ++i) {
        const auto& s = pending[i];
        std::fprintf(log, "coop_weapon_update player=%08X hash=%08X ammo=%u plasma=%u lookup=%d item=%08X owner=%08X entries=%u active_ref=%08X pending_ref=%08X readable_mask=%u\n",
            s.player, s.hash, s.ammo, s.plasma, static_cast<int>(s.result.status),
            s.result.item, s.result.owner, s.result.count, s.active, s.pending, s.readable);
        const auto& id = s.identity;
        std::fprintf(log, "coop_weapon_identity player=%08X item=%08X vtable=%08X kind=%d readable_mask=%u template=%08X-%08X-%08X-%08X proxy=%08X-%08X-%08X-%08X phase=before_native\n",
            s.player, s.result.item, id.vtable, static_cast<int>(id.kind), id.readable,
            id.templateGuid[0], id.templateGuid[1], id.templateGuid[2], id.templateGuid[3],
            id.proxyGuid[0], id.proxyGuid[1], id.proxyGuid[2], id.proxyGuid[3]);
    }
    std::fprintf(log, "coop_weapon_probe calls=%ld missing=%ld invalid=%ld wrong_owner=%ld dropped=%ld\n",
                 calls, missing, invalid, wrongOwner, dropped);
}
}  // namespace weapon_probe
