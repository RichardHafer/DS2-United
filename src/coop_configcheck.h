// Dead Space 2 United by Richard Hafer.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// The INI check between co-op machines: every machine sends a fingerprint of
// its DS2United.ini (one hash pair per shared setting) with its plugin build,
// and the other side lists every setting that differs. Personal settings (key
// bindings, names, addresses, display, logging) are left out.
namespace coop_config {
constexpr uint32_t kMagic = 0x47464E43;   // 'CNFG'
constexpr uint8_t kVersion = 1;
constexpr unsigned kPerPart = 60, kMaxParts = 40;

#pragma pack(push, 1)
struct Entry { uint32_t key, value; };
// 504 bytes: under the receive buffer (512) and a size no other packet has.
struct Packet {
    uint32_t magic;
    uint8_t version, part, parts, pad;
    uint32_t token;    // the sender
    uint32_t build;    // the sender's plugin link time stamp
    uint32_t digest;   // of the whole list, so parts of two lists never mix
    uint16_t count, total;
    Entry entries[kPerPart];
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 504, "config packet size");

inline uint32_t Fnv(const std::string& s) {
    uint32_t h = 0x811C9DC5u;
    for (unsigned char c : s) { h ^= c; h *= 0x01000193u; }
    return h;
}
inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
inline bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}
inline bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Settings that belong to one machine or one player (lower case section and key).
inline bool Personal(const std::string& section, const std::string& key) {
    static const char* const kSections[] = {"debugmenu", "debug", "trace", "vlog", "loadout"};
    for (const char* s : kSections) if (section == s) return true;
    static const char* const kKeys[][2] = {
        {"coop", "network"}, {"coop", "joinaddress"}, {"coop", "port"}, {"coop", "peer"}, {"coop", "autostart"}, {"coop", "machineindex"},
        {"coop", "backgroundfullspeed"}, {"coop", "focusinputonly"}, {"coop", "unpinthreads"}, {"coop", "crashdumps"}, {"coop", "configcheck"},
        {"nameplate", "name"}, {"shop", "scale"}, {"shop", "font"}, {"hud", "scale"}, {"hud", "top"},
        {"marks", "scale"}, {"zone", "ringeffect"}, {"zone", "ringtint"}, {"ds2randomizer", "installdelayms"},
        {"ds2randomizer", "seed"},   // a client plays the host's seed (seed sync)
        {"voices", "pack"}, {"voices", "volume"}, {"voices", "subtitles"},
    };
    for (const auto& k : kKeys) if (section == k[0] && key == k[1]) return true;
    // key bindings, logs and probes
    if (EndsWith(key, "key") || EndsWith(key, "keys") || EndsWith(key, "hotkey")) return true;
    if (key.compare(0, 3, "log") == 0 || EndsWith(key, "trace") || Contains(key, "verbose") || Contains(key, "diagnostic")) return true;
    return false;
}

// What Windows reads: the first section of a name, the first key of a name in it.
struct Setting { std::string section, key, value; };
inline std::vector<Setting> ReadIni(const std::string& text) {
    std::vector<Setting> out;
    std::map<std::string, bool> sectionsSeen;
    std::map<std::string, bool> keysSeen;
    std::string section;
    bool active = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos) { active = false; continue; }
            section = Trim(line.substr(1, close - 1));
            const std::string lower = Lower(section);
            active = !sectionsSeen[lower];
            sectionsSeen[lower] = true;
            continue;
        }
        const size_t eq = line.find('=');
        if (!active || eq == std::string::npos) continue;
        Setting s{section, Trim(line.substr(0, eq)), Trim(line.substr(eq + 1))};
        if (s.key.empty()) continue;
        const std::string id = Lower(section) + "." + Lower(s.key);
        if (keysSeen[id]) continue;
        keysSeen[id] = true;
        out.push_back(s);
    }
    return out;
}

// How much a difference matters (sync audit 13.10.): 2 must match (desync, invisible
// enemies, one-sided waits), 1 should match, 0 only the host's copy counts.
inline int Severity(const std::string& section, const std::string& key) {
    static const char* const kMustSections[] = {"checkpoint", "level", "mirror", "streams", "loading", "physics", "respawn", "necro", "versus", "versuspoints", "gate"};
    for (const char* s : kMustSections) if (section == s) return section == "cutscene" ? 1 : 2;
    if (section == "cutscene") return key == "scenetrace" || key == "hidepartners" ? 1 : 2;
    static const char* const kMustKeys[][2] = {
        {"coop", "enabled"}, {"coop", "hostnpcs"}, {"coop", "mpuniformstreams"}, {"coop", "syncallobjects"}, {"coop", "synconlyplayers"}, {"coop", "ghostplayer"},
        {"coop", "ghostskeleton"}, {"coop", "requirepeerready"}, {"coop", "checkenemytemplates"}, {"coop", "tickmode"}, {"coop", "neutralnpcslocal"},
        {"coop", "localnpcclasses"}, {"coop", "scriptedspawnslocal"}, {"coop", "scriptedspawnwindowms"}, {"coop", "hideneutralghosttwins"}, {"coop", "reporthits"},
        {"coop", "applyhits"}, {"coop", "worldhits"}, {"coop", "worldprophits"}, {"coop", "sharetormentorarm"}, {"coop", "shareregeneration"}, {"coop", "deathsync"},
        {"coop", "mirrordeath"}, {"coop", "ghostsignoretriggers"}, {"coop", "liftwait"}, {"coop", "liftwaitms"}, {"coop", "nearesttarget"}, {"coop", "pausekeepsrunning"},
        {"coop", "suitsync"}, {"coop", "suitstream"}, {"coop", "loadoutsync"}, {"coop", "provisionweapons"}, {"coop", "provisioncreate"}, {"coop", "helmetsync"},
        {"coop", "ghostguidredirect"}, {"kinesis", "streamheld"}, {"zone", "openobjectives"}, {"zone", "openobjectivesstepms"}, {"menu", "enabled"},
        {"menu", "campaigncoop"}, {"menu", "extramaps"}, {"survival", "enabled"}, {"survival", "map"}, {"ds2randomizer", "enabled"},
        {"ds2randomizer", "globaltemplates"}, {"ds2randomizer", "variantbias"}, {"bot", "enabled"}, {"bot", "nicoleignoresbots"},
        {"difficulty", "sync"}, {"difficulty", "lockinmatch"}, {"coop", "seedsync"},
        {"schematicrandomizer", "seed"}, {"benchrandomizer", "seed"}};
    for (const auto& k : kMustKeys) if (section == k[0] && key == k[1]) return 2;
    if (section == "mpmap") {
        static const char* const kMpMust[] = {"enabled", "blockmplogic", "blockendround", "blockroundtimers", "blockobjectivemarkers", "blockteamlogic", "blockmpslashers",
                                              "keepslasherrecords", "allowmpmusic", "blockmpglobals", "blockscriptspawns", "spawnplayer", "usemapcheckpoint", "playertemplate",
                                              "spawnpoint", "settlems", "dropforeignstreams", "registerpcmcodec", "soundbankoverflow", "outbreakrender"};
        for (const char* k : kMpMust) if (key == k) return 2;
        if (key.compare(0, 8, "putonmap") == 0) return 2;
    }
    // read on every machine: each one divides its own hits on objective props and times its own holds
    if (section == "objective" && (key == "prophealthfactor" || key == "holdperplayerpercent")) return 1;
    static const char* const kHostSections[] = {"bot1", "bot2", "bot3", "moreenemies", "watchdogfamilies", "watchdogtimers", "mercenaries", "objective",
                                                "versusslashers", "versusskins"};
    for (const char* s : kHostSections) if (section == s) return 0;
    if (section.compare(0, 4, "wave") == 0) return 0;
    if (section == "bot") return 0;   // the two that matter on a client are above
    if (section == "ds2randomizer" && (key.compare(0, 11, "moreenemies") == 0 || key == "extraspawns" || key == "swarmhorde" || key == "packhorde")) return 0;
    return 1;
}

struct Fingerprint {
    std::vector<Entry> entries;               // sorted by key hash
    std::map<uint32_t, std::string> names;    // key hash -> "[Section] Key = value"
    std::map<uint32_t, int> severity;         // key hash -> Severity()
    uint32_t digest = 0;
    unsigned personal = 0;
};
// A game file both machines need alike, by its size or content hash.
inline void AddFile(Fingerprint& f, const std::string& relative, const std::string& value) {
    const uint32_t k = Fnv("file." + Lower(relative));
    f.entries.push_back(Entry{k, Fnv(value)});
    f.names[k] = "file " + relative + " (" + value + ")";
    f.severity[k] = 2;
}
// What really runs here, beside the INI text (a failed hook looks like the switch off).
inline uint32_t StateKey(const std::string& name) { return Fnv("state." + Lower(name)); }
inline void AddState(Fingerprint& f, const std::string& name, const std::string& value) {
    const uint32_t k = StateKey(name);
    f.entries.push_back(Entry{k, Fnv(value)});
    f.names[k] = "state " + name + " = " + value;
    f.severity[k] = 2;
}
inline void Finish(Fingerprint& f) {
    std::sort(f.entries.begin(), f.entries.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
    f.entries.erase(std::unique(f.entries.begin(), f.entries.end(), [](const Entry& a, const Entry& b) { return a.key == b.key; }), f.entries.end());
    uint32_t h = 0x811C9DC5u;
    for (const Entry& e : f.entries) {
        for (int i = 0; i < 4; ++i) { h ^= (e.key >> (8 * i)) & 0xFF; h *= 0x01000193u; }
        for (int i = 0; i < 4; ++i) { h ^= (e.value >> (8 * i)) & 0xFF; h *= 0x01000193u; }
    }
    f.digest = h ? h : 1;
}
inline Fingerprint Make(const std::string& iniText) {
    Fingerprint f;
    bool syncSeed = true, bench = false, ownBenchSeed = false;
    std::string mainSeed = "1";
    const auto settings = ReadIni(iniText);
    for (const Setting& s : settings) {
        const std::string section = Lower(s.section), key = Lower(s.key);
        if (section == "coop" && key == "seedsync") syncSeed = std::strtoul(s.value.c_str(), nullptr, 0) != 0;
        if (section == "ds2randomizer" && key == "seed") mainSeed = s.value;
        if (section == "benchrandomizer") {
            if (key == "seed") ownBenchSeed = !s.value.empty();
            if (key == "statshuffle" || key == "nodeshuffle") bench |= std::strtoul(s.value.c_str(), nullptr, 0) != 0;
        }
        if (Personal(section, key)) { ++f.personal; continue; }
        const uint32_t k = Fnv(section + "." + key);
        f.entries.push_back(Entry{k, Fnv(Lower(s.value))});
        f.names[k] = "[" + s.section + "] " + s.key + " = " + s.value;
        f.severity[k] = Severity(section, key);
    }
    // The bench builds before joining a lobby and keeps its startup seed.
    if (!syncSeed || (bench && !ownBenchSeed)) {
        const uint32_t k = Fnv("ds2randomizer.seed");
        f.entries.push_back({k, Fnv(Lower(mainSeed))});
        f.names[k] = "[DS2Randomizer] Seed = " + mainSeed;
        f.severity[k] = 2;
    }
    Finish(f);
    return f;
}
inline unsigned PartsFor(const Fingerprint& f) {
    const unsigned n = static_cast<unsigned>((f.entries.size() + kPerPart - 1) / kPerPart);
    return n < 1 ? 1 : n > kMaxParts ? kMaxParts : n;
}
inline Packet MakePart(const Fingerprint& f, unsigned part, uint32_t token, uint32_t build) {
    Packet p{};
    p.magic = kMagic;
    p.version = kVersion;
    p.part = static_cast<uint8_t>(part);
    p.parts = static_cast<uint8_t>(PartsFor(f));
    p.token = token;
    p.build = build;
    p.digest = f.digest;
    p.total = static_cast<uint16_t>(f.entries.size());
    const size_t first = static_cast<size_t>(part) * kPerPart;
    for (size_t i = first; i < f.entries.size() && i < first + kPerPart; ++i) p.entries[p.count++] = f.entries[i];
    return p;
}

// One sender's list, put together from its parts.
struct Assembly {
    uint32_t digest = 0, build = 0;
    uint64_t have = 0;
    unsigned parts = 0, total = 0;
    std::vector<Entry> entries;
    // true once the list is whole
    bool Add(const Packet& p) {
        if (p.magic != kMagic || p.version != kVersion || !p.parts || p.parts > kMaxParts || p.part >= p.parts || p.count > kPerPart) return false;
        if (p.digest != digest) { *this = Assembly{}; digest = p.digest; parts = p.parts; total = p.total; entries.assign(p.total, Entry{}); }
        build = p.build;
        const size_t first = static_cast<size_t>(p.part) * kPerPart;
        if (first + p.count > entries.size()) return false;
        std::memcpy(entries.data() + first, p.entries, sizeof(Entry) * p.count);
        have |= uint64_t{1} << p.part;
        return Whole();
    }
    bool Whole() const { return parts && have == ((parts >= 64 ? ~uint64_t{0} : (uint64_t{1} << parts) - 1)); }
};

struct Difference { uint32_t key; int kind; };   // 0 value differs, 1 only here, 2 only there
inline std::vector<Difference> Compare(const std::vector<Entry>& ours, const std::vector<Entry>& theirs) {
    std::vector<Difference> out;
    size_t i = 0, j = 0;
    while (i < ours.size() || j < theirs.size()) {
        if (j >= theirs.size() || (i < ours.size() && ours[i].key < theirs[j].key)) { out.push_back({ours[i++].key, 1}); continue; }
        if (i >= ours.size() || theirs[j].key < ours[i].key) { out.push_back({theirs[j++].key, 2}); continue; }
        if (ours[i].value != theirs[j].value) out.push_back({ours[i].key, 0});
        ++i;
        ++j;
    }
    return out;
}
}  // namespace coop_config
