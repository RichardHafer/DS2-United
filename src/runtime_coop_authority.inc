// Dead Space 2 United by Hafer.
// Authority transfer, observation only.
//
// Kinesis in the engine's own multiplayer works by handing an object's
// authority to the player who grabs it: RPC_AuthorityTransferRequest goes to
// the owner, who answers Granted (with the packed state) or Denied, and Push
// hands authority back. DeadspaceGameManager::IsAuthorityTransferEnabled is
// a folded constant in the PDB (0x0067A7A0, the same stub as a dozen other
// trivial getters), so there is no switch to flip; the question is only
// whether anything in a campaign session ever asks. This counts.
//
// PC, from dump 45856 (2026-09-17). NetReflectable vtable 0x01B1F448 carries
// the four RPCs as construct/remote pairs:
//     +0x58 Push_construct     0x007E19D0 (ret 0x408)   +0x5C Push_remote     0x007E1A10 (ret 0x408)
//     +0x60 Request_construct  0x007E2240 (ret 4)       +0x64 Request_remote  0x007E2280 (ret 4)
//     +0x68 Granted_construct  0x007E1F90 (ret 0x40C)   +0x6C Granted_remote  0x007E1FE0 (ret 0x40C)
//     +0x70 Denied_construct   0x007E1DB0 (ret 4)       +0x74 Denied_remote   0x007E1DF0 (ret 4)
// Identified by what they do, not by order: Request_remote asks this+0x40
// (ShouldSurrenderAuthority), packs 0x400 bytes of state and posts a Granted
// event of 0x448 bytes through the connection; Denied_remote is fifteen
// bytes handing (this, connection, 1) to the NetSession at [0x020216DC];
// the two 0x138-frame remotes unpack that state. The Granted/Push pair is
// told apart by allocation size (0x448 vs 0x440), matching the construct
// each remote path uses.
//
// Mid hooks, because two of the eight take a PackBuffer by value (0x400
// bytes on the stack) and an inline detour would have to mirror that.
namespace authority {
bool installed = false;
struct Site { const char* name; uint32_t rva; const unsigned char* sig; size_t size; safetyhook::MidHook hook; volatile LONG count; };
static const unsigned char kPushC[]    = {0x56,0x57,0x68,0x40,0x04,0x00,0x00,0x8B,0xF1,0xE8};
static const unsigned char kPushR[]    = {0x81,0xEC,0x38,0x01,0x00,0x00,0x56,0x8B,0xF1,0x83,0x7E,0x0C};
static const unsigned char kRequestC[] = {0x56,0x6A,0x40,0x8B,0xF1,0xE8};
static const unsigned char kRequestR[] = {0x81,0xEC,0x48,0x05,0x00,0x00,0x53,0x55,0x8B,0xD9,0x8B,0x6B};
static const unsigned char kGrantedC[] = {0x56,0x57,0x68,0x48,0x04,0x00,0x00,0x8B,0xF1,0xE8};
static const unsigned char kGrantedR[] = {0x81,0xEC,0x38,0x01,0x00,0x00,0x56,0x8B,0xF1,0x83,0x7E,0x0C};
static const unsigned char kDeniedC[]  = {0x56,0x6A,0x40,0x8B,0xF1,0xE8};
static const unsigned char kDeniedR[]  = {0x8B,0x44,0x24,0x04,0x6A,0x01,0x50,0x51,0x8B,0x0D,0xDC,0x16};
Site sites[] = {
    {"request_sent",    0x3E2240, kRequestC, sizeof(kRequestC), {}, 0},
    {"request_recv",    0x3E2280, kRequestR, sizeof(kRequestR), {}, 0},
    {"granted_sent",    0x3E1F90, kGrantedC, sizeof(kGrantedC), {}, 0},
    {"granted_recv",    0x3E1FE0, kGrantedR, sizeof(kGrantedR), {}, 0},
    {"denied_sent",     0x3E1DB0, kDeniedC,  sizeof(kDeniedC),  {}, 0},
    {"denied_recv",     0x3E1DF0, kDeniedR,  sizeof(kDeniedR),  {}, 0},
    {"push_sent",       0x3E19D0, kPushC,    sizeof(kPushC),    {}, 0},
    {"push_recv",       0x3E1A10, kPushR,    sizeof(kPushR),    {}, 0},
};
constexpr unsigned kSites = sizeof(sites) / sizeof(sites[0]);

// What an event was about: the reflectable (this), its entity's GUID
// (entity = reflectable - 0x5C, GUID at entity + 0x24) and vtable, and who
// asked (the return address, for the sender side: that is the grab path).
void Note(unsigned site, const safetyhook::Context& ctx) {
    const LONG n = InterlockedIncrement(&sites[site].count);
    if (n > 12) return;   // the first dozen tell the story; after that the counters do
    const uint32_t reflectable = ctx.ecx;
    const uint32_t entity = reflectable - 0x5C;
    uint32_t vtable = 0, guid[4]{}, ret = 0, arg = 0;
    Read(entity, &vtable, 4);
    Read(entity + 0x24, guid, 16);
    Read(ctx.esp, &ret, 4);
    Read(ctx.esp + 4, &arg, 4);
    gate::Event("coop_authority %s reflectable=%08X entity=%08X vtable=%08X guid=%08X-%08X-%08X-%08X from=%08X arg=%08X",
                sites[site].name, reflectable, entity, vtable, guid[0], guid[1], guid[2], guid[3], ret, arg);
}
template <unsigned N> void OnSite(safetyhook::Context& ctx) { Note(N, ctx); }
safetyhook::MidHookFn callbacks[kSites] = {OnSite<0>, OnSite<1>, OnSite<2>, OnSite<3>, OnSite<4>, OnSite<5>, OnSite<6>, OnSite<7>};

bool Install(FILE* log, const std::string& ini) {
    if (GetPrivateProfileIntA("Coop", "ObserveAuthority", 1, ini.c_str()) == 0) return false;
    unsigned ok = 0;
    for (unsigned i = 0; i < kSites; ++i) {
        unsigned char actual[16]{};
        if (!Read(base + sites[i].rva, actual, sites[i].size) || std::memcmp(actual, sites[i].sig, sites[i].size) != 0) {
            std::fprintf(log, "coop_authority %s signature mismatch at %08X\n", sites[i].name, static_cast<unsigned>(base + sites[i].rva));
            continue;
        }
        auto result = safetyhook::MidHook::create(reinterpret_cast<void*>(base + sites[i].rva), callbacks[i]);
        if (!result) { std::fprintf(log, "coop_authority %s hook failed\n", sites[i].name); continue; }
        sites[i].hook = std::move(*result);
        ++ok;
    }
    installed = ok > 0;
    std::fprintf(log, "coop_authority observing %u/%u sites (NetReflectable vtable %08X slots +58..+74)\n",
                 ok, kSites, static_cast<unsigned>(base + 0x171F448));
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    LONG total = 0;
    for (auto& s : sites) total += InterlockedCompareExchange(&s.count, 0, 0);
    if (total == last) return;
    last = total;
    std::fprintf(log, "coop_authority");
    for (auto& s : sites) std::fprintf(log, " %s=%ld", s.name, InterlockedCompareExchange(&s.count, 0, 0));
    std::fprintf(log, "\n");
}
}  // namespace authority
