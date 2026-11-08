// Dead Space 2 United by Richard Hafer.
// Ghost movement on a slow link: the engine's timeline, not ours.
//
// Every ghost (the partner's Isaac, the host's enemies on a client) keeps a
// two-sample history of received transforms (Entity+0x174, 0x70 bytes: two
// 0x30 entries of quaternion+position, +0x20 sender ms, +0x24 local ms; +0x60
// count, +0x61 newest). Entity::unpackUpdate (0x007D3520) feeds it through
//     imatrix44_t::AuthoritativeUpdate   0x007D0530, thiscall(mat44*, localMs, remoteMs), ret 0xC
// and Entity::GhostTick (0x007D1A70) extrapolates from the newest two with
// t = (now - prev.local) / (last.local - prev.local), then blends 0.25 a frame.
// Only the local arrival times go in. On a LAN those are even; at 400 ms ping
// with jitter two packets land in one frame and look like a sprint, one late
// packet looks like a stop, and the ghost lurches (research 25.09.).
//
// So each sample's local time is put back on the sender's timeline: sender
// ms plus the smallest (local - sender) offset seen lately, which is when it
// would have arrived with no queueing. The spacing is then the sender's own,
// reordered packets fall out the engine's "older than newest" check, and two
// arrivals in one frame no longer overwrite each other. The engine's own
// prediction and blending stay exactly as they are.
//
// Never worse than stock: a mapped time that would not be newer than the
// newest stored sample (a pause moved the offset) goes in as the raw arrival
// time, the way it always did. The sample with no sender time (0x010DBF24
// passes 0) is left alone.
namespace ghostclock {
bool enabled = false, installed = false;
constexpr uint32_t kAuthoritativeUpdateRva = 0x3D0530;
constexpr unsigned kTracks = 128, kWindow = 16;
safetyhook::InlineHook hook;

struct Track {
    uint32_t history = 0;
    uint32_t offsets[kWindow]{};
    unsigned head = 0, count = 0;
    DWORD lastUse = 0;
};
Track tracks[kTracks]{};
volatile LONG samples = 0, mapped = 0, rawFallback = 0, noSenderTime = 0, evicted = 0;
volatile LONG removedMsTotal = 0, removedMsMax = 0;

Track& TrackFor(uint32_t history, DWORD now) {
    Track* oldest = &tracks[0];
    for (auto& t : tracks) {
        if (t.history == history) return t;
        if (t.lastUse < oldest->lastUse) oldest = &t;
    }
    if (oldest->history) InterlockedIncrement(&evicted);
    *oldest = Track{};
    oldest->history = history;
    oldest->lastUse = now;
    return *oldest;
}

// The smallest offset in the window, compared wrap-safe (the two clocks
// start anywhere).
uint32_t MinOffset(const Track& t) {
    uint32_t best = t.offsets[0];
    for (unsigned i = 1; i < t.count; ++i)
        if (static_cast<int32_t>(t.offsets[i] - best) < 0) best = t.offsets[i];
    return best;
}

void __fastcall OnAuthoritativeUpdate(void* self, void*, const void* matrix, uint32_t local, uint32_t remote) {
    const auto history = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    if (!enabled || !remote || !history) {
        if (enabled && !remote) InterlockedIncrement(&noSenderTime);
        hook.thiscall<void>(self, matrix, local, remote);
        return;
    }
    InterlockedIncrement(&samples);
    const DWORD now = GetTickCount();
    Track& t = TrackFor(history, now);
    t.lastUse = now;
    t.offsets[t.head] = local - remote;
    t.head = (t.head + 1) % kWindow;
    if (t.count < kWindow) ++t.count;
    uint32_t onTimeline = remote + MinOffset(t);
    // The newest sample the history already holds.
    unsigned char count = 0, newest = 0;
    uint32_t newestLocal = 0;
    const bool haveNewest = Read(history + 0x60, &count, 1) && Read(history + 0x61, &newest, 1) && count && newest < 2 &&
                            Read(history + newest * 0x30 + 0x24, &newestLocal, 4);
    if (haveNewest && static_cast<int32_t>(onTimeline - newestLocal) <= 0 && static_cast<int32_t>(local - newestLocal) > 0) {
        onTimeline = local;
        InterlockedIncrement(&rawFallback);
    } else {
        InterlockedIncrement(&mapped);
        const LONG removed = static_cast<LONG>(local - onTimeline);
        if (removed >= 0 && removed < 5000) {
            InterlockedExchangeAdd(&removedMsTotal, removed);
            if (removed > InterlockedCompareExchange(&removedMsMax, 0, 0)) InterlockedExchange(&removedMsMax, removed);
        }
    }
    hook.thiscall<void>(self, matrix, onTimeline, remote);
}

bool Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "GhostTimeline", 0, ini.c_str()) != 0;
    // push esi / mov esi,ecx / movzx eax,byte [esi+0x61] / lea ecx,[eax+eax*2] / shl ecx,4 / mov edx,[ecx+esi+0x24]
    static const unsigned char kSig[] = {0x56, 0x8B, 0xF1, 0x0F, 0xB6, 0x46, 0x61, 0x8D, 0x0C, 0x40, 0xC1, 0xE1, 0x04, 0x8B, 0x54, 0x31};
    if (enabled)
        installed = hook_site::Install(log, "coop_ghostclock imatrix44_t::AuthoritativeUpdate", base + kAuthoritativeUpdateRva, kSig, sizeof(kSig),
                                       reinterpret_cast<void*>(OnAuthoritativeUpdate), hook);
    std::fprintf(log, "coop_ghostclock enabled=%d installed=%d (ghost samples keep the sender's spacing, not the arrival jitter)\n",
                 enabled ? 1 : 0, installed ? 1 : 0);
    return installed;
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG n = InterlockedCompareExchange(&samples, 0, 0);
    if (n / 300 == last) return;   // about every ten seconds of one ghost
    last = n / 300;
    const LONG m = InterlockedCompareExchange(&mapped, 0, 0);
    std::fprintf(log, "coop_ghostclock samples=%ld mapped=%ld raw_fallback=%ld no_sender_time=%ld evicted=%ld jitter_removed_avg=%ld ms max=%ld ms\n",
                 n, m, InterlockedCompareExchange(&rawFallback, 0, 0), InterlockedCompareExchange(&noSenderTime, 0, 0),
                 InterlockedCompareExchange(&evicted, 0, 0), m ? InterlockedCompareExchange(&removedMsTotal, 0, 0) / m : 0,
                 InterlockedCompareExchange(&removedMsMax, 0, 0));
}
}  // namespace ghostclock
