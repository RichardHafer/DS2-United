// Dead Space 2 United by Richard Hafer.
// The INI check between the machines of a session ([Coop] ConfigCheck): each
// machine sends a fingerprint of its DS2United.ini and its plugin build to its
// peers (coop_configcheck.h), on a new peer three times and then once a minute,
// and lists every shared setting that differs from what a peer sent. A client
// compares with the host, the host with every client. Settings are those read at
// start; personal ones (keys, names, addresses, display, logs) are left out. The
// game files every machine needs alike ride along: the randomizer streams by
// size and DS2DAT12.DAT (the Outbreak maps) by size or absence.
namespace configcheck {
bool enabled = true;
coop_config::Fingerprint own;
uint32_t ownBuild = 0;
std::map<uint32_t, coop_config::Assembly> assemblies;   // by sender token
std::map<uint32_t, uint64_t> reported;                  // sender token -> digest and build last reported
bool stateAdded = false;   // what really runs (survival ready or not) joins the list once known
volatile LONG mustMismatches = 0;
struct Schedule { uint32_t token = 0; DWORD next = 0; int burst = 0; };
Schedule schedules[coop_peers::kMaxPeers]{};
volatile LONG partsSent = 0, partsReceived = 0, partsRejected = 0, listsWhole = 0, matches = 0, mismatches = 0, buildMismatches = 0;
unsigned lines = 0;

void Line(const char* format, ...) {
    if (++lines > 160) return;
    char buffer[300];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("config check: %s", buffer);
}

void SendAll(DWORD now) {
    const unsigned parts = coop_config::PartsFor(own);
    for (unsigned part = 0; part < parts; ++part) {
        const coop_config::Packet p = coop_config::MakePart(own, part, peerReadiness.token, ownBuild);
        if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&partsSent);
    }
    (void)now;
}
// Game thread, with the poll: a new peer gets the list now, in 3 s and in 10 s
// (the side channel has no retries), everybody once a minute after that.
void Tick(DWORD now) {
    if (enabled && !stateAdded && !own.entries.empty()) {
        char why[240], fix[200];
        const int ready = lobby::SurvivalCheck("", 0, why, fix);
        if (ready >= 0) {
            stateAdded = true;
            coop_config::AddState(own, "survival", ready ? "ready" : "not ready");
            coop_config::Finish(own);
            for (auto& s : schedules) s.next = now;   // the partners get the new list now
            Line("own state added: survival %s%s%s; digest now %08X", ready ? "ready" : "NOT ready", ready ? "" : ": ", ready ? "" : why, own.digest);
        }
    }
    if (!enabled || !network || own.entries.empty() || !peerReadiness.token) return;
    // A copy still in its intro has no survival state yet; its list would make every partner
    // report "state survival" as a must-match difference (17.10.). It waits for its own state,
    // a minute at most (a randomizer half that never answers).
    static DWORD firstTick = 0;
    if (!firstTick) firstTick = now;
    if (!stateAdded && now - firstTick < 60000) return;
    bool due = false;
    for (unsigned i = 0; i < coop_peers::kMaxPeers; ++i) {
        const auto& peer = peerTable.peers[i];
        Schedule& s = schedules[i];
        if (!peer.used || !peer.token) { s = Schedule{}; continue; }
        if (s.token != peer.token) { s = Schedule{peer.token, now, 0}; }
        if (static_cast<LONG>(now - s.next) < 0) continue;
        due = true;
        ++s.burst;
        s.next = now + (s.burst == 1 ? 3000 : s.burst == 2 ? 7000 : 60000);
    }
    if (due) SendAll(now);
}

void Report(uint32_t token, const coop_config::Assembly& a) {
    const char* who = IsHost() ? "a partner" : "the host";
    const char* whoUpper = IsHost() ? "A PARTNER" : "THE HOST";
    if (a.build != ownBuild) {
        InterlockedIncrement(&buildMismatches);
        Line("%s (token %08X) runs another Dead Space 2 United build: %08X, ours %08X", who, token, a.build, ownBuild);
        char text[96];
        std::snprintf(text, sizeof(text), "%s RUNS ANOTHER DS2 UNITED BUILD", whoUpper);
        notice::Enqueue(text);
        char why[96];
        std::snprintf(why, sizeof(why), "%s runs a different DS2United.asi than this PC", who);
        overlay::ShowHint("Another Dead Space 2 United build", why, "use the same DS2United.asi on every PC (the same update)", 15000);
    }
    const auto diffRaw = coop_config::Compare(own.entries, a.entries);
    if (diffRaw.empty()) {
        InterlockedIncrement(&matches);
        Line("the INI matches %s's (token %08X): %zu shared settings, digest %08X", who, token, own.entries.size(), own.digest);
        char text[96];
        std::snprintf(text, sizeof(text), "INI MATCHES %s (%zu SETTINGS)", whoUpper, own.entries.size());
        notice::Enqueue(text);
        return;
    }
    InterlockedIncrement(&mismatches);
    // the ones that break co-op first (13.10.: FIRST was whatever came first by hash)
    auto severityOf = [&](const coop_config::Difference& d) {
        const auto s = own.severity.find(d.key);
        return s != own.severity.end() ? s->second : 1;
    };
    std::vector<coop_config::Difference> diff = diffRaw;
    std::stable_sort(diff.begin(), diff.end(), [&](const auto& a, const auto& b) { return severityOf(a) > severityOf(b); });
    unsigned must = 0;
    for (const auto& d : diff) if (severityOf(d) == 2) ++must;
    if (must) InterlockedIncrement(&mustMismatches);
    unsigned differ = 0, onlyHere = 0, onlyThere = 0;
    for (const auto& d : diff) (d.kind == 0 ? differ : d.kind == 1 ? onlyHere : onlyThere)++;
    Line("the INI differs from %s's (token %08X, digest %08X, ours %08X): %u value(s) differ, %u setting(s) only here, %u only there", who, token, a.digest,
         own.digest, differ, onlyHere, onlyThere);
    for (const auto& d : diff) {
        const auto name = own.names.find(d.key);
        const char* level = severityOf(d) == 2 ? "MUST MATCH" : severityOf(d) == 1 ? "should match" : "host only";
        if (d.kind == 0) Line("  differs (%s): %s (ours)", level, name != own.names.end() ? name->second.c_str() : "?");
        else if (d.kind == 1) Line("  only here (%s): %s", level, name != own.names.end() ? name->second.c_str() : "?");
        else Line("  only on %s's side: setting %08X (not in our INI)", who, d.key);
    }
    Line("%u must match, %u other difference(s)", must, static_cast<unsigned>(diff.size()) - must);
    char text[120];
    if (must) std::snprintf(text, sizeof(text), "INI DIFFERS FROM %s: %u MUST MATCH, SEE DS2UNITED.LOG", whoUpper, must);
    else std::snprintf(text, sizeof(text), "INI DIFFERS FROM %s: %u SETTING(S), NONE CRITICAL", whoUpper, static_cast<unsigned>(diff.size()));
    notice::Enqueue(text);
    if (must) {
        const auto first = own.names.find(diff[0].key);
        std::string shown = first != own.names.end() ? first->second.substr(0, first->second.find(" = ")) : std::string("a setting only the other side has");
        char what[96], why[240];
        std::snprintf(what, sizeof(what), "Settings differ from %s", who);
        std::snprintf(why, sizeof(why), "%s must be the same on every PC%s; DS2United.log lists all %u", shown.c_str(), must > 1 ? " (and more)" : "", must);
        overlay::ShowHint(what, why, shown.compare(0, 6, "state ") == 0
                                          ? "survival is not ready on one PC: the hint on that PC names the setting"
                                          : "copy the host's plugins\\DS2United.ini to this PC (names and keys stay personal) and restart the game",
                          15000);
    }
    const auto first = own.names.find(diff[0].key);
    if (diff[0].kind != 2 && first != own.names.end()) {
        std::string shown = first->second.substr(0, first->second.find(" = "));
        for (char& c : shown) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::snprintf(text, sizeof(text), "FIRST: %s", shown.c_str());
        notice::Enqueue(text);
    }
}
// Receive loop: a part of a peer's list.
bool OnBytes(const void* data, int size) {
    if (!enabled || size != static_cast<int>(sizeof(coop_config::Packet))) return false;
    coop_config::Packet p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != coop_config::kMagic || !p.token || p.token == peerReadiness.token) return false;
    InterlockedIncrement(&partsReceived);
    coop_config::Assembly& a = assemblies[p.token];
    if (!a.Add(p)) {
        if (!(p.version == coop_config::kVersion && p.part < p.parts)) InterlockedIncrement(&partsRejected);
        return true;
    }
    InterlockedIncrement(&listsWhole);
    const uint64_t stamp = (static_cast<uint64_t>(a.digest) << 32) | a.build;
    auto seen = reported.find(p.token);
    if (seen != reported.end() && seen->second == stamp) return true;
    reported[p.token] = stamp;
    if (!own.entries.empty()) Report(p.token, a);
    return true;
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Coop", "ConfigCheck", 1, ini.c_str()) != 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(&__ImageBase);
    ownBuild = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(dos) + dos->e_lfanew)->FileHeader.TimeDateStamp;
    std::ifstream file(ini, std::ios::binary);
    unsigned files = 0;
    if (enabled && file) {
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        own = coop_config::Make(text);
        // the game folder is the INI's folder's parent (plugins\..)
        std::string game = ini.substr(0, ini.find_last_of("\\/") == std::string::npos ? 0 : ini.find_last_of("\\/"));
        game = game.substr(0, game.find_last_of("\\/") == std::string::npos ? 0 : game.find_last_of("\\/") + 1);
        static const char* const kFiles[] = {"randomizer\\ds2_randomizer_pool.str", "randomizer\\ds2_randomizer_pool.toc", "randomizer\\ds2_ubermorph.str",
                                             "randomizer\\ds2_ubermorph.toc",         "randomizer\\ds2_leaper.str",         "randomizer\\ds2_leaper.toc",
                                             "randomizer\\ds2coop_frontend.str",      "randomizer\\ds2coop_mpskins.lua",    "DS2DAT12.DAT"};
        for (const char* name : kFiles) {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            char size[32] = "absent";
            if (GetFileAttributesExA((game + name).c_str(), GetFileExInfoStandard, &a))
                std::snprintf(size, sizeof(size), "%llu bytes", (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow);
            coop_config::AddFile(own, name, size);
            ++files;
        }
        coop_config::Finish(own);
    }
    std::fprintf(log, "config check %s: %s, %zu shared settings and files (%u files, %u personal settings left out), %u part(s) of %zu bytes, digest %08X, "
                      "build %08X\n",
                 enabled ? "on" : "off ([Coop] ConfigCheck=0)", ini.c_str(), own.entries.size(), files, own.personal, coop_config::PartsFor(own),
                 sizeof(coop_config::Packet), own.digest, ownBuild);
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&partsSent, 0, 0) / 15 + InterlockedCompareExchange(&partsReceived, 0, 0) / 15 +
                     InterlockedCompareExchange(&matches, 0, 0) * 7 + InterlockedCompareExchange(&mismatches, 0, 0) * 11 +
                     InterlockedCompareExchange(&buildMismatches, 0, 0) * 13 + InterlockedCompareExchange(&partsRejected, 0, 0);
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "config check must_match_mismatches=%ld state_added=%d\n", InterlockedCompareExchange(&mustMismatches, 0, 0), stateAdded ? 1 : 0);
    std::fprintf(log, "config check parts sent=%ld received=%ld rejected=%ld lists_whole=%ld matches=%ld mismatches=%ld build_mismatches=%ld\n",
                 InterlockedCompareExchange(&partsSent, 0, 0), InterlockedCompareExchange(&partsReceived, 0, 0), InterlockedCompareExchange(&partsRejected, 0, 0),
                 InterlockedCompareExchange(&listsWhole, 0, 0), InterlockedCompareExchange(&matches, 0, 0), InterlockedCompareExchange(&mismatches, 0, 0),
                 InterlockedCompareExchange(&buildMismatches, 0, 0));
}
}  // namespace configcheck
