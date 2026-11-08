// Dead Space 2 United by Hafer.
// Survival music on the MP maps ([SurvivalMusic] in DS2United.ini; logic in
// coop_survmusic.h, evidence in analysis/DS2_SURVIVAL_MUSIC_2026-10-09.md).
//
// The survival state comes from the randomizer's director (DS2Coop_SurvivalEvent
// kind 3) on the host and in solo, and from the host's survnet kind 3 on a
// client. The tracks are the eight PlayAudioStreams [MpMap] AllowMpMusic lets
// spawn; they are started and stopped by message through the dispatch at
// 0x00756C60 (bool __cdecl(msg*, int flags), plain ret), game thread only,
// while the MP map runs. The mirror keeps these hashes local (MusicHash).
// Those eight never spawned in a visit, so by default the campaign's music
// sections play instead whenever they are missing ([SurvivalMusic] Source).
namespace arena { bool RunningNow(); }   // runtime_coop_arena.inc
namespace survmusic {
bool enabled = false;
uint32_t stopDelayMs = 2000;
coop_survmusic::Player player;
enum Source { kAuto = 0, kMp = 1, kCampaign = 2 };
int sourceWanted = kAuto;
int source = -1;   // this arrival's: kMp, kCampaign, or -1 not decided yet
uint32_t sourceEpoch = 0xFFFFFFFFu;
unsigned calmPick = 0, wavePick = 0;
volatile LONG mpPlays = 0, campaignPlays = 0, calmPlays = 0, noSource = 0;
volatile LONG state = 0, wave = 0;   // the director's state (0 off, 1 arming, 2 wave, 3 intermission, 4 dead) and wave
volatile LONG sends = 0, sendFailures = 0, plays = 0, stops = 0, faults = 0, stateChanges = 0;
DWORD lastRepeat = 0, lastLines = 0;
unsigned lines = 0;
constexpr uint32_t kDispatchRva = 0x356C60;
struct Message { uint32_t hash; uint8_t inFlight, pad[3]; uint32_t source; };
static_assert(sizeof(Message) == 12, "message size");

__declspec(noinline) bool Send(uint32_t hash) {
    Message m{hash, 0, {0, 0, 0}, 0};
    bool result = false;
    __try {
        result = reinterpret_cast<bool(__cdecl*)(Message*, int)>(base + kDispatchRva)(&m, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
    InterlockedIncrement(result ? &sends : &sendFailures);
    return result;
}
// From the director (any thread) or a client's side channel.
void SetState(int newState, int newWave, const char* from) {
    const LONG before = InterlockedExchange(&state, newState);
    InterlockedExchange(&wave, newWave);
    if (before != newState) {
        InterlockedIncrement(&stateChanges);
        if (lines < 60) { ++lines; gate::Event("survmusic state %ld -> %d wave=%d from=%s", before, newState, newWave, from); }
    }
}
unsigned ListenersOf(uint32_t hash) {
    uint32_t objects[4]{}, vtables[4]{};
    return gate::Listeners(hash, objects, vtables, 4);
}
// Once per arrival, at the first play: the MP tracks when they are on the map,
// else the campaign sections. Two listeners on a section mean both global audio
// Lua stacks run (global_assets and global_mp_assets), logged for the record.
int ResolveSource() {
    if (mpmap::visit.epochs != sourceEpoch) { sourceEpoch = mpmap::visit.epochs; source = -1; }
    if (source >= 0) return source;
    const unsigned mp = ListenersOf(coop_survmusic::kPlay[4]), campaign = ListenersOf(coop_survmusic::kSectionWave[0]);
    source = sourceWanted == kMp ? (mp ? kMp : -1)
           : sourceWanted == kCampaign ? (campaign ? kCampaign : -1)
           : mp ? kMp : campaign ? kCampaign : -1;
    gate::Event("survmusic source on arrival %u: %s (wanted %s; aud_play_stream_mus_05 listeners=%u, mus_action_01 listeners=%u%s)", sourceEpoch,
                source == kMp ? "the MP round tracks" : source == kCampaign ? "the campaign music sections" : "NONE, nothing hears either",
                sourceWanted == kMp ? "mp" : sourceWanted == kCampaign ? "campaign" : "auto", mp, campaign,
                campaign > 1 ? ", both global audio Lua stacks run" : "");
    if (source < 0) { InterlockedIncrement(&noSource); source = -2; }   // asked once per arrival
    return source;
}

void Tick(DWORD now) {
    if (!enabled) return;
    const bool onMap = (mpmap::visit.phase == coop_mpmap::Phase::Arrived || arena::RunningNow()) && mpmap::LmState(nullptr) == coop_mpmap::kRunning;
    if (!onMap) {
        if (player.playing) player.Forget();   // the map's streams end with their entities
        return;
    }
    const LONG s = InterlockedCompareExchange(&state, 0, 0);
    // the host repeats its state, so a partner that missed a change converges
    if (network && IsHost() && s && now - lastRepeat >= 10000) {
        lastRepeat = now;
        survnet::Queue(3, nullptr, static_cast<uint32_t>((InterlockedCompareExchange(&wave, 0, 0) << 8) | s));
    }
    // 2 a wave, 5 a Mercenaries run (the director reports that to tell the respawn rules apart)
    const coop_survmusic::Action a = player.Update(s == 2 || s == 5, now, stopDelayMs);
    if (a.kind == coop_survmusic::Action::None) return;
    const int from = ResolveSource();
    if (a.kind == coop_survmusic::Action::Play) {
        InterlockedIncrement(&plays);
        if (from == kMp) {
            Send(coop_survmusic::kStop);
            const bool ok = Send(coop_survmusic::kPlay[a.track]);
            InterlockedIncrement(&mpPlays);
            if (lines < 60) {
                ++lines;
                gate::Event("survmusic play aud_play_stream_mus_%02d (%08X) wave=%ld: %s", a.track + 1, coop_survmusic::kPlay[a.track],
                            InterlockedCompareExchange(&wave, 0, 0), ok ? "sent" : "NO LISTENER");
            }
        } else if (from == kCampaign) {
            // the next action section each time: the conductor crossfades, the fear level sets the layers
            const unsigned i = wavePick++ % 4;
            const bool ok = Send(coop_survmusic::kSectionWave[i]);
            InterlockedIncrement(&campaignPlays);
            if (lines < 60) {
                ++lines;
                gate::Event("survmusic play %s (%08X) wave=%ld: %s", coop_survmusic::kSectionWaveNames[i], coop_survmusic::kSectionWave[i],
                            InterlockedCompareExchange(&wave, 0, 0), ok ? "sent" : "NO LISTENER");
            }
        }
    } else {
        InterlockedIncrement(&stops);
        if (from == kMp) {
            const bool ok = Send(coop_survmusic::kStop);
            if (lines < 60) { ++lines; gate::Event("survmusic stop: %s", ok ? "sent" : "no listener"); }
        } else if (from == kCampaign) {
            // no stop message for a section: a calm one takes over between the waves
            const unsigned i = calmPick++ % 6;
            const bool ok = Send(coop_survmusic::kSectionCalm[i]);
            InterlockedIncrement(&calmPlays);
            if (lines < 60) { ++lines; gate::Event("survmusic calm %s (%08X): %s", coop_survmusic::kSectionCalmNames[i], coop_survmusic::kSectionCalm[i], ok ? "sent" : "NO LISTENER"); }
        }
    }
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("SurvivalMusic", "Enabled", 0, ini.c_str()) != 0;
    stopDelayMs = static_cast<uint32_t>(GetPrivateProfileIntA("SurvivalMusic", "StopDelaySeconds", 2, ini.c_str())) * 1000u;
    char text[16]{};
    GetPrivateProfileStringA("SurvivalMusic", "Source", "auto", text, sizeof(text), ini.c_str());
    sourceWanted = _stricmp(text, "mp") == 0 ? kMp : _stricmp(text, "campaign") == 0 ? kCampaign : kAuto;
    unsigned char head[5]{};
    // push ecx / mov edx,[esp+8]: the dispatch's own first bytes, or another plugin's jmp in front of them
    const bool site = Read(base + kDispatchRva, head, sizeof(head)) && ((head[0] == 0x51 && head[1] == 0x8B && head[2] == 0x54) || head[0] == 0xE9);
    if (!site) enabled = false;
    std::fprintf(log, "survmusic enabled=%d source=%s stop_delay_ms=%u dispatch %08X %s allow_mp_music=%d\n", enabled ? 1 : 0,
                 sourceWanted == kMp ? "mp" : sourceWanted == kCampaign ? "campaign" : "auto (the MP tracks when on the map, else the campaign sections)", stopDelayMs,
                 static_cast<unsigned>(base + kDispatchRva), site ? "ok" : "MISMATCH (music off)", mpmap::allowMpMusic ? 1 : 0);
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    std::fprintf(log, "survmusic state=%ld wave=%ld playing=%d track=%d plays=%ld (mp %ld, campaign %ld) stops=%ld calm=%ld sends=%ld no_listener=%ld no_source=%ld "
                      "faults=%ld state_changes=%ld kept_entities=%ld\n",
                 InterlockedCompareExchange(&state, 0, 0), InterlockedCompareExchange(&wave, 0, 0), player.playing ? 1 : 0, player.track,
                 InterlockedCompareExchange(&plays, 0, 0), InterlockedCompareExchange(&mpPlays, 0, 0), InterlockedCompareExchange(&campaignPlays, 0, 0),
                 InterlockedCompareExchange(&stops, 0, 0), InterlockedCompareExchange(&calmPlays, 0, 0), InterlockedCompareExchange(&sends, 0, 0),
                 InterlockedCompareExchange(&sendFailures, 0, 0), InterlockedCompareExchange(&noSource, 0, 0), InterlockedCompareExchange(&faults, 0, 0),
                 InterlockedCompareExchange(&stateChanges, 0, 0),
                 InterlockedCompareExchange(&mpmap::musicKept, 0, 0));
}
}  // namespace survmusic
