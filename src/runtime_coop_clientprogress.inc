// Dead Space 2 United by Richard Hafer.
// A co-op player's own progress (user, 16.10.: "bau client fortschritt";
// analysis/COOP_SAVE_STATIONS_2026-10-16.md F4). Every client save is held (the host's world
// is the save), so every client session began with 0 credits and the four starting items.
//  * A client keeps its player in a file per host (plugins\DS2ClientProgress\host_<id>.ds2client,
//    coop_client_progress.h): written when it would save (a held kiosk or autosave), when the
//    host saved (his save key in the checkpoint packet moved), before a level change and every
//    two minutes, only while its Isaac lives and plays no scene and no necro. Read once per
//    session with that host after the level ran 5 s; then the death restart point is stored
//    again, so a restart keeps it.
//  * Every machine across a necro transform: the Isaac that comes back is the level's template
//    (four items, no credits), so what he carried goes with him and comes back.
// Credits and power nodes only ever rise, guns and items missing are given, bench nodes are
// filled (the trees are this machine's own), the suit is put on.
#include <cerrno>
namespace mpnative { extern std::string profilePath; }   // runtime_coop_mpnative.inc: DS2Profile.ini
namespace clientprog {
bool enabled = true;   // [Coop] ClientProgress
std::string folder;
uint64_t hostId = 0, ownHostId = 0;
uint32_t hostSaveKey = 0;
bool hostKeySeen = false, sessionApplied = false, captureWanted = false, changeCaptured = false;
uint64_t sessionHost = 0, unreadableHost = 0;
const char* captureReason = "";
DWORD settledSince = 0, lastCapture = 0, pendingApplyAt = 0;
// The session's first apply: the host's load barrier only releases a dressed, restored
// Isaac, so guns, items and credits need not wait the 5 s the periodic capture waits.
DWORD firstApplyMs = 1000;
uint32_t pendingPlayer = 0;
coop_client_progress::File carried, loaded, captured;
bool haveCarried = false;
const char* gunNames[bot::kGunKinds]{};
volatile LONG captures = 0, writes = 0, writeFailures = 0, loads = 0, loadFailures = 0, applies = 0, carries = 0, carriesApplied = 0, noFile = 0,
              givenGuns = 0, givenStacks = 0, giveFailures = 0, nodesFilled = 0, suitChanges = 0, hostKeyChanges = 0, reapplies = 0;

template<class... A> void Event(const char* format, A... args) { gate::Event(format, args...); }

uint64_t OwnHostId() {
    if (ownHostId) return ownHostId;
    if (mpnative::profilePath.empty()) return 0;
    char text[32]{};
    GetPrivateProfileStringA("Coop", "HostId", "", text, sizeof(text), mpnative::profilePath.c_str());
    uint64_t id = text[0] ? _strtoui64(text, nullptr, 16) : 0;
    if (!id) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        id = (static_cast<uint64_t>(counter.QuadPart) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(GetTickCount()) << 32) ^ GetCurrentProcessId();
        if (!id) id = 1;
        std::snprintf(text, sizeof(text), "%08X%08X", static_cast<unsigned>(id >> 32), static_cast<unsigned>(id));
        WritePrivateProfileStringA("Coop", "HostId", text, mpnative::profilePath.c_str());
        Event("client progress: this machine's host id %s (DS2Profile.ini [Coop] HostId)", text);
    }
    ownHostId = id;
    return id;
}
uint32_t HostSaveKey() {
    uint32_t sm = 0, hash = 0;
    return Read(base + bot::kSaveManagerRva, &sm, 4) && sm && Read(sm + 0x2C, &hash, 4) ? hash : 0;
}
// The host's checkpoint packet (reserved[1..3]), on a client.
void OnHost(uint32_t, uint32_t saveKey, uint32_t idHi, uint32_t idLo) {
    if (!enabled || IsHost()) return;
    const uint64_t id = (static_cast<uint64_t>(idHi) << 32) | idLo;
    if (id && id != hostId) {
        if (hostId) {
            haveCarried = false;
            pendingApplyAt = 0;
        }
        sessionApplied = false;
        hostKeySeen = false;
        captureWanted = false;
        settledSince = 0;
        hostId = id;
        Event("client progress: host id %08X%08X", idHi, idLo);
    }
    if (!saveKey) return;
    if (hostKeySeen && saveKey != hostSaveKey) {
        InterlockedIncrement(&hostKeyChanges);
        captureWanted = true;
        captureReason = "the host saved";
    }
    hostSaveKey = saveKey;
    hostKeySeen = true;
}
void NoteSaveAttempt(const char* why) {
    if (!enabled || IsHost()) return;
    captureWanted = true;
    captureReason = why;
}

bool Capture(uint32_t player, coop_client_progress::File& f) {
    f = coop_client_progress::File{};
    f.host = hostId;
    const int32_t credits = shop::Credits(player);
    if (credits < 0) return false;
    f.credits = credits > coop_client_progress::kMaxCredits ? static_cast<int32_t>(coop_client_progress::kMaxCredits) : credits;
    int32_t nodes = 0;
    if (!Read(player + bot::kPlayerNodes, &nodes, 4)) return false;
    f.nodes = nodes < 0 ? 0 : (nodes > coop_client_progress::kMaxNodes ? static_cast<int32_t>(coop_client_progress::kMaxNodes) : nodes);
    f.guns = respawn::GunsOf(player);
    respawn::Stack stacks[coop_client_progress::kMaxStacks]{};
    const unsigned n = respawn::ReadStacks(player, stacks, coop_client_progress::kMaxStacks);
    for (unsigned i = 0; i < n; ++i) {
        std::memcpy(f.stacks[f.stackCount].guid, stacks[i].guid, 16);
        f.stacks[f.stackCount++].amount = stacks[i].amount;
    }
    if (suitSender.have) { f.hasSuit = true; std::memcpy(f.suit, suitSender.guid, 16); }
    uint32_t manager = 0, count = 0;
    if (Read(base + bot::kUpgradeManagerRva, &manager, 4) && manager && Read(manager + bot::kUpgradeSet + bot::kTreeCount, &count, 4) && count <= 64) {
        for (uint32_t t = 0; t < count && f.treeCount < coop_client_progress::kMaxTrees; ++t) {
            uint32_t tree = 0, key = 0;
            if (!Read(manager + bot::kUpgradeSet + t * 4, &tree, 4) || !tree || !Read(tree, &key, 4)) continue;
            coop_bot::BenchNode nodesRead[coop_bot::kMaxBenchNodes];
            uint32_t where[coop_bot::kMaxBenchNodes]{};
            const int read = bot::ReadTree(tree, nodesRead, where);
            coop_client_progress::Tree& out = f.trees[f.treeCount];
            out = coop_client_progress::Tree{};
            out.key = key;
            for (int k = 0; k < read && out.count < coop_client_progress::kMaxTreeNodes; ++k)
                if (nodesRead[k].filled && !nodesRead[k].start) out.nodes[out.count++] = nodesRead[k].key;
            if (out.count) ++f.treeCount;
        }
    }
    char map[64]{}, stream[8]{};
    if (level::CurrentMap(map, sizeof(map), stream, sizeof(stream))) std::snprintf(f.map, sizeof(f.map), "%s", map);
    InterlockedIncrement(&captures);
    return true;
}

void Apply(uint32_t player, const coop_client_progress::File& f, const char* why) {
    char done[256] = "";
    size_t used = 0;
    auto note = [&](const char* format, auto... args) {
        if (used < sizeof(done)) used += std::snprintf(done + used, sizeof(done) - used, format, args...);
    };
    const int32_t credits = shop::Credits(player);
    if (credits >= 0 && f.credits > credits && WriteBytes(player + shop::kCredits, &f.credits, 4)) note(" credits %d->%d", credits, f.credits);
    int32_t nodes = 0;
    if (Read(player + bot::kPlayerNodes, &nodes, 4) && f.nodes > nodes && WriteBytes(player + bot::kPlayerNodes, &f.nodes, 4)) note(" nodes %d->%d", nodes, f.nodes);
    const uint32_t has = respawn::GunsOf(player);
    for (int k = 0; k < bot::kGunKinds; ++k) {
        if (!(f.guns & 1u << k) || (has & 1u << k)) continue;
        const char* failed = respawn::GiveGun(player, k);
        InterlockedIncrement(failed ? &giveFailures : &givenGuns);
        note(" %s%s", bot::kGunCatalogue[k].name, failed ? "(failed)" : "");
        if (failed) Event("client progress: %s not given: %s", bot::kGunCatalogue[k].name, failed);
    }
    respawn::Stack now[coop_client_progress::kMaxStacks]{};
    const unsigned present = respawn::ReadStacks(player, now, coop_client_progress::kMaxStacks);
    unsigned stacksGiven = 0;
    for (unsigned i = 0; i < f.stackCount; ++i) {
        respawn::Stack want{};
        std::memcpy(want.guid, f.stacks[i].guid, 16);
        want.amount = f.stacks[i].amount;
        for (unsigned j = 0; j < present && want.amount; ++j) {
            if (std::memcmp(now[j].guid, want.guid, 16) != 0 || !now[j].amount) continue;
            const uint32_t take = now[j].amount < want.amount ? now[j].amount : want.amount;
            now[j].amount -= take;
            want.amount -= take;
        }
        if (!want.amount) continue;
        const char* failed = respawn::GiveStack(player, want, i);
        InterlockedIncrement(failed ? &giveFailures : &givenStacks);
        if (!failed) ++stacksGiven;
        else Event("client progress: stack %08X-%08X x%u: %s", want.guid[0], want.guid[1], want.amount, failed);
    }
    if (stacksGiven) note(" stacks+%u", stacksGiven);
    if (f.hasSuit && !(suitSender.have && std::memcmp(suitSender.guid, f.suit, 16) == 0)) {
        if (::coop_uniform::BotDress(player, f.suit)) { InterlockedIncrement(&suitChanges); note(" suit %08X", f.suit[0]); }
        else note(" suit %08X(failed)", f.suit[0]);
    }
    uint32_t touched[coop_client_progress::kMaxTrees]{};
    unsigned touchedCount = 0, filled = 0;
    for (unsigned t = 0; t < f.treeCount; ++t) {
        const uint32_t tree = bot::TreeFor(f.trees[t].key);
        coop_bot::BenchNode nodesRead[coop_bot::kMaxBenchNodes];
        uint32_t where[coop_bot::kMaxBenchNodes]{};
        const int read = tree ? bot::ReadTree(tree, nodesRead, where) : -1;
        bool any = false;
        for (unsigned k = 0; k < f.trees[t].count; ++k)
            for (int n = 0; n < read; ++n) {
                if (nodesRead[n].key != f.trees[t].nodes[k] || nodesRead[n].filled) continue;
                const uint32_t one = 1;
                if (WriteBytes(where[n] + bot::kNodeFill, &one, 4)) { ++filled; any = true; }
                break;
            }
        if (any && touchedCount < std::size(touched)) touched[touchedCount++] = tree;
    }
    if (filled) {
        InterlockedExchangeAdd(&nodesFilled, static_cast<LONG>(filled));
        bot::RecalcGuns(player, touched, touchedCount);
        shop::PlayerRecalcRaw(player);
        note(" bench nodes+%u", filled);
    }
    InterlockedIncrement(&applies);
    Event("client progress applied (%s) to Isaac %08X:%s", why, player, used ? done : " nothing missing");
    if (!IsHost()) { checkpoint::Reapply("client progress applied"); InterlockedIncrement(&reapplies); }
}

bool WriteFile(const coop_client_progress::File& f) {
    if (folder.empty() || !f.host || f.host != hostId || f.host == unreadableHost) return false;
    CreateDirectoryA(folder.c_str(), nullptr);
    const std::string text = coop_client_progress::Write(f, gunNames, bot::kGunKinds);
    const std::string path = folder + coop_client_progress::FileName(f.host), temp = path + ".tmp", backup = path + ".bak";
    FILE* out = nullptr;
    if (fopen_s(&out, temp.c_str(), "wb") != 0 || !out) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    if (std::fclose(out) != 0 || !ok) return false;
    CopyFileA(path.c_str(), backup.c_str(), FALSE);
    return MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
// 0 read, 1 no file, -1 a file that did not parse (the backup is tried)
int ReadFile(uint64_t host, coop_client_progress::File& f) {
    const std::string path = folder + coop_client_progress::FileName(host);
    const int result = coop_client_progress::ReadForHost(host, f, [&](unsigned copy, coop_client_progress::File& candidate) {
        const std::string name = copy ? path + ".bak" : path;
        FILE* in = nullptr;
        const errno_t opened = fopen_s(&in, name.c_str(), "rb");
        if (opened || !in) return opened == ENOENT ? 1 : -1;
        std::string text;
        char chunk[4096];
        size_t got = 0;
        while (text.size() <= 256 * 1024 && (got = std::fread(chunk, 1, sizeof(chunk), in)) > 0)
            text.append(chunk, got);
        const bool complete = !std::ferror(in) && text.size() <= 256 * 1024;
        std::fclose(in);
        if (!complete) return -1;
        const auto error = coop_client_progress::Parse(text.data(), text.size(), gunNames, bot::kGunKinds, candidate);
        if (error != coop_client_progress::Error::None || candidate.host != host) {
            Event("client progress: %s refused (%s)", name.c_str(), error == coop_client_progress::Error::None ? "another host's" : coop_client_progress::ErrorName(error));
            return -1;
        }
        if (copy) Event("client progress: recovered %s from its backup", path.c_str());
        return 0;
    });
    if (result < 0) unreadableHost = host;
    else if (unreadableHost == host) unreadableHost = 0;
    return result;
}

bool RestartPending() {
    const uint32_t loop = level::GameMainLoop();
    uint32_t state = 0;
    return loop && Read(loop + 0x210, &state, 4) && state;
}
bool Alive(uint32_t player) { return player && !nameplate::Dead(player); }

// From the necro module, before it removes Isaac (Transform, ghost start); not for a watch.
void BeforeIsaacGoes(uint32_t player) {
    if (!enabled || !player || necro::LocalRole() == 3) return;
    if (!Capture(player, carried)) return;
    haveCarried = true;
    InterlockedIncrement(&carries);
    Event("client progress: Isaac %08X goes into the necro with credits=%d nodes=%d guns=%X stacks=%u", player, carried.credits, carried.nodes, carried.guns,
          carried.stackCount);
    // A client's file keeps the human's state: what the necro does is not progress.
    if (!IsHost() && sessionApplied && sessionHost == hostId && hostId && Alive(player) && WriteFile(carried)) InterlockedIncrement(&writes);
}
// After the necro key brought Isaac back (Revert, EscapeGhost): the template Isaac gets it back.
void AfterIsaacReturns(uint32_t player, const char*) {
    if (!enabled || !player) return;
    pendingPlayer = player;
    pendingApplyAt = GetTickCount() + 1500;
}

void Tick(DWORD now) {
    if (!enabled) return;
    if (!inLevel) { settledSince = 0; changeCaptured = false; pendingApplyAt = 0; return; }
    const uint32_t player = LocalCampaignPlayer();
    if (pendingApplyAt && static_cast<int32_t>(now - pendingApplyAt) >= 0) {
        pendingApplyAt = 0;
        if (player && player == pendingPlayer && necro::LocalRole() == 0) {
            if (!IsHost() && hostId && !sessionApplied) {
                coop_client_progress::File& f = loaded;
                const int r = ReadFile(hostId, f);
                InterlockedIncrement(r == 0 ? &loads : r == 1 ? &noFile : &loadFailures);
                sessionApplied = true;
                sessionHost = hostId;
                if (r == 0) Apply(player, f, "the session's first Isaac, back from the necro");
                else if (haveCarried) Apply(player, carried, "back from the necro");
            } else if (haveCarried) {
                InterlockedIncrement(&carriesApplied);
                Apply(player, carried, "back from the necro");
            }
        }
    }
    if (IsHost() || !peerTable.Count() || !hostId) return;
    // Before a level change: the player as he leaves.
    if (LevelChangePending()) {
        if (!changeCaptured && sessionApplied && sessionHost == hostId && player && Alive(player) && necro::LocalRole() == 0) {
            changeCaptured = true;
            if (Capture(player, captured) && WriteFile(captured)) { InterlockedIncrement(&writes); lastCapture = now; Event("client progress written (a level change)"); }
        }
        return;
    }
    changeCaptured = false;
    const bool ready = levelSettled && player && necro::LocalRole() == 0 && !cutscene::Following() && !cutscene::SequenceRunningNow() && Alive(player) &&
                       !RestartPending();
    if (!ready) { settledSince = 0; return; }
    if (!settledSince) settledSince = Stamp(now);   // not now | 1: an even tick read as 49 days and skipped the 5 s
    const bool first = !sessionApplied || sessionHost != hostId;
    if (now - settledSince < (first ? firstApplyMs : 5000)) return;
    if (first) {
        const int r = ReadFile(hostId, loaded);
        InterlockedIncrement(r == 0 ? &loads : r == 1 ? &noFile : &loadFailures);
        sessionApplied = true;
        sessionHost = hostId;
        lastCapture = now;
        captureWanted = false;
        if (r == 0) Apply(player, loaded, "the session's start");
        else Event("client progress: %s for host %08X%08X, starting from what the level gives", r == 1 ? "nothing saved yet" : "the saved file did not read",
                   static_cast<unsigned>(hostId >> 32), static_cast<unsigned>(hostId));
        return;
    }
    if (!captureWanted && now - lastCapture < 120000) return;
    const char* why = captureWanted ? captureReason : "every two minutes";
    captureWanted = false;
    lastCapture = now;
    if (!Capture(player, captured)) return;
    if (WriteFile(captured)) {
        InterlockedIncrement(&writes);
        if (InterlockedCompareExchange(&writes, 0, 0) <= 40 || std::strcmp(why, "every two minutes") != 0)
            Event("client progress written (%s): credits=%d nodes=%d guns=%X stacks=%u trees=%u", why, captured.credits, captured.nodes, captured.guns, captured.stackCount,
                  captured.treeCount);
    } else {
        InterlockedIncrement(&writeFailures);
        Event("client progress: the file could not be written (%s)", folder.c_str());
    }
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "ClientProgress", 1, ini.c_str()) != 0;
    firstApplyMs = static_cast<DWORD>(GetPrivateProfileIntA("Coop", "ClientProgressFirstApplyMs", 1000, ini.c_str()));
    const size_t slash = ini.find_last_of("\\/");
    folder = (slash == std::string::npos ? std::string() : ini.substr(0, slash + 1)) + "DS2ClientProgress\\";
    for (int k = 0; k < bot::kGunKinds; ++k) gunNames[k] = bot::kGunCatalogue[k].name;
    std::fprintf(log, "client progress %s: a client keeps its player per host in %s; every machine keeps Isaac's state across a necro transform\n",
                 enabled ? "on" : "off ([Coop] ClientProgress=0)", folder.c_str());
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG state = captures + writes + loads + applies + carries + writeFailures + loadFailures + hostKeyChanges;
    if (state == last) return;
    last = state;
    std::fprintf(log, "client progress host=%08X%08X session_applied=%d captures=%ld writes=%ld write_failures=%ld loads=%ld no_file=%ld load_failures=%ld applies=%ld "
                      "carries=%ld carries_applied=%ld given guns=%ld stacks=%ld failures=%ld bench_nodes=%ld suits=%ld host_saves=%ld restart_point_stored=%ld\n",
                 static_cast<unsigned>(hostId >> 32), static_cast<unsigned>(hostId), sessionApplied ? 1 : 0, captures, writes, writeFailures, loads, noFile, loadFailures,
                 applies, carries, carriesApplied, givenGuns, givenStacks, giveFailures, nodesFilled, suitChanges, hostKeyChanges, reapplies);
}
}  // namespace clientprog
