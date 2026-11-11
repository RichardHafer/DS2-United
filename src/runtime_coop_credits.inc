// Dead Space 2 United by Richard Hafer.
// The mod's own block at the start of the credits (18.10., user). The original credits stay
// untouched and follow it.
//
// UICredits::Initialize (0x00A589C0) reads the section count from the text "CREDITS_Num" (307).
// UICredits::Update (0x00A71450) then builds section i from text keys:
//     name  = text("CREDITS_%d", i)         e.g. "Credits_ExecP"
//     num   = atoi(text("%s_Num", name))    0 or missing: one big line text(name) ("Dead Space 2")
//     head  = text(name)                    the role line ("Franchise Executive Producer")
//     cols  = atoi(text("%s_Cols", name))   default 1
//     entry = text("%s_%d", name, j)        j = 1..num ("Steve Papoutsis")
// Every text goes through HashString_SDBM (lower case, h * 0x1003F + c) and
// LocaleManager::GetLocalizedTextStatic (0x01371280, cdecl(hash), char* or null). The text table
// cannot grow, so the seven calls of that lookup inside the two credits functions are pointed at
// Text below: it counts our sections into CREDITS_Num, answers CREDITS_1..K with our section
// names and moves the original CREDITS_i to i + K. Nothing outside the credits is touched.
namespace modcredits {
constexpr uint32_t kLookupRva = 0xF71280;                 // GetLocalizedTextStatic 0x01371280
constexpr uint32_t kInitializeSite = 0x658A74;            // the CREDITS_Num lookup in Initialize
constexpr uint32_t kCountStringRva = 0x1730474;           // "CREDITS_Num", pushed at 0x00A58A55
constexpr uint32_t kSectionStringRva = 0x1731DB8;         // "CREDITS_%d", pushed at 0x00A717D3
constexpr uint32_t kSites[] = {kInitializeSite, 0x6717FA, 0x671834, 0x67185C, 0x671919, 0x6719EF, 0x671A5F};
constexpr unsigned kSiteCount = sizeof(kSites) / sizeof(kSites[0]);
constexpr unsigned kMaxNames = 8, kMaxBlank = 4, kMaxOriginal = 4000;
constexpr const char* kTitleKey = "DS2Coop_Credits_Title";
constexpr const char* kTeamKey = "DS2Coop_Credits_Team";
constexpr const char* kBlankKey = "Credits_Blank_Line";   // the game's own empty line

using Lookup = const char*(__cdecl*)(uint32_t);
bool enabled = true, installed = false, active = false;
char title[96] = "", role[96] = "", names[kMaxNames][96]{};
char countText[16] = "", teamCountText[8] = "";
unsigned nameCount = 0, blankLines = 2, ourCount = 0;
const char* ourSections[2 + kMaxBlank]{};
uint32_t countHash = 0, titleHash = 0, teamHash = 0, teamCountHash = 0, nameHashes[kMaxNames]{};
std::vector<std::pair<uint32_t, uint32_t>> sectionIndex;  // (hash of "CREDITS_i", i), sorted by hash
std::vector<uint32_t> originalHash;                       // [i] = hash of "CREDITS_(i - K)"
int originalCount = 0;
volatile LONG starts = 0, lookups = 0, ourServed = 0, shifted = 0, countRejected = 0, served = 0;

uint32_t Hash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        uint32_t c = static_cast<unsigned char>(*s);
        if (c - 'A' < 26u) c += 0x20;
        h = h * 0x1003F + c;
    }
    return h;
}
const char* Original(uint32_t hash) { return reinterpret_cast<Lookup>(base + kLookupRva)(hash); }

void Build(int count) {
    originalCount = count;
    const unsigned total = static_cast<unsigned>(count) + ourCount;
    sectionIndex.clear();
    sectionIndex.reserve(total);
    originalHash.assign(total + 1, 0);
    char key[32];
    for (unsigned i = 1; i <= total; ++i) {
        std::snprintf(key, sizeof(key), "CREDITS_%u", i);
        sectionIndex.emplace_back(Hash(key), i);
        if (i > ourCount) {
            std::snprintf(key, sizeof(key), "CREDITS_%u", i - ourCount);
            originalHash[i] = Hash(key);
        }
    }
    std::sort(sectionIndex.begin(), sectionIndex.end());
}

const char* Served(const char* key, const char* text) {
    InterlockedIncrement(&ourServed);
    if (InterlockedIncrement(&served) <= 16) gate::Event("credits: %s -> '%s'", key, text ? text : "(empty)");
    return text;
}

// Called by the credits code in place of GetLocalizedTextStatic.
const char* __cdecl Text(uint32_t hash) {
    InterlockedIncrement(&lookups);
    if (hash == countHash) {
        const char* text = Original(hash);
        const int count = text ? std::atoi(text) : 0;
        if (count <= 0 || count > static_cast<int>(kMaxOriginal)) {
            active = false;
            if (InterlockedIncrement(&countRejected) <= 3)
                gate::Event("credits: CREDITS_Num is '%s'; our block left out", text ? text : "(missing)");
            return text;
        }
        Build(count);
        std::snprintf(countText, sizeof(countText), "%d", count + static_cast<int>(ourCount));
        active = true;
        InterlockedExchange(&served, 0);
        InterlockedIncrement(&starts);
        gate::Event("credits start: CREDITS_Num %d -> %s, %u sections of ours first", count, countText, ourCount);
        return countText;
    }
    if (!active) return Original(hash);
    const auto at = std::lower_bound(sectionIndex.begin(), sectionIndex.end(), std::make_pair(hash, 0u));
    if (at != sectionIndex.end() && at->first == hash) {
        const uint32_t i = at->second;
        if (i <= ourCount) return Served("section", ourSections[i - 1]);
        InterlockedIncrement(&shifted);
        return Original(originalHash[i]);
    }
    if (hash == titleHash) return Served("title", title);
    if (hash == teamHash) return Served("role", role[0] ? role : nullptr);
    if (hash == teamCountHash) return teamCountText;
    for (unsigned j = 0; j < nameCount; ++j)
        if (hash == nameHashes[j]) return Served("name", names[j]);
    return Original(hash);
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Credits", "Enabled", 1, ini.c_str()) != 0;
    if (!enabled) {
        std::fprintf(log, "credits block off ([Credits] Enabled=0)\n");
        return;
    }
    GetPrivateProfileStringA("Credits", "Title", "Dead Space 2 United", title, sizeof(title), ini.c_str());
    if (!std::strcmp(title, "Dead Space 2 Co-op Mod") || !std::strcmp(title, "Dead Space 2 Dead Space 2 United"))
        std::snprintf(title, sizeof(title), "Dead Space 2 United");
    GetPrivateProfileStringA("Credits", "Role", "Created by", role, sizeof(role), ini.c_str());
    char list[sizeof(names)]{};
    GetPrivateProfileStringA("Credits", "Names", "Richard Hafer", list, sizeof(list), ini.c_str());
    if (!std::strcmp(list, "Hafer")) std::snprintf(list, sizeof(list), "Richard Hafer");
    char* rest = nullptr;
    for (char* token = strtok_s(list, "|", &rest); token && nameCount < kMaxNames; token = strtok_s(nullptr, "|", &rest)) {
        while (*token == ' ') ++token;
        size_t length = std::strlen(token);
        while (length && token[length - 1] == ' ') token[--length] = 0;
        if (length) std::snprintf(names[nameCount++], sizeof(names[0]), "%s", token);
    }
    const int blank = GetPrivateProfileIntA("Credits", "BlankLines", 2, ini.c_str());
    blankLines = blank < 0 ? 0 : (blank > static_cast<int>(kMaxBlank) ? kMaxBlank : static_cast<unsigned>(blank));

    if (title[0]) ourSections[ourCount++] = kTitleKey;
    if (nameCount) ourSections[ourCount++] = kTeamKey;
    if (!ourCount) {
        std::fprintf(log, "credits block off: [Credits] Title and Names are empty\n");
        return;
    }
    for (unsigned i = 0; i < blankLines; ++i) ourSections[ourCount++] = kBlankKey;
    countHash = Hash("CREDITS_Num");
    titleHash = Hash(kTitleKey);
    teamHash = Hash(kTeamKey);
    char key[48];
    std::snprintf(key, sizeof(key), "%s_Num", kTeamKey);
    teamCountHash = Hash(key);
    std::snprintf(teamCountText, sizeof(teamCountText), "%u", nameCount);
    for (unsigned j = 0; j < nameCount; ++j) {
        std::snprintf(key, sizeof(key), "%s_%u", kTeamKey, j + 1);
        nameHashes[j] = Hash(key);
    }

    // All or nothing: every site must still call the lookup, and both credits strings must be where
    // the two functions push them.
    char count[12]{}, section[11]{};
    const bool strings = Read(base + kCountStringRva, count, sizeof(count)) && std::memcmp(count, "CREDITS_Num", 12) == 0 &&
                         Read(base + kSectionStringRva, section, sizeof(section)) && std::memcmp(section, "CREDITS_%d", 11) == 0;
    unsigned matching = 0;
    for (unsigned i = 0; i < kSiteCount; ++i) {
        unsigned char call[5]{};
        int32_t rel = 0;
        if (Read(base + kSites[i], call, 5)) std::memcpy(&rel, call + 1, 4);
        const uint32_t target = static_cast<uint32_t>(base + kSites[i] + 5 + rel);
        if (call[0] == 0xE8 && target == base + kLookupRva) ++matching;
        else std::fprintf(log, "credits site %08X: %02X -> %08X, expected a call to %08X\n", static_cast<unsigned>(base + kSites[i]), call[0], target,
                          static_cast<unsigned>(base + kLookupRva));
    }
    if (!strings || matching != kSiteCount) {
        std::fprintf(log, "credits block NOT installed: strings=%d sites %u/%u\n", strings ? 1 : 0, matching, kSiteCount);
        return;
    }
    unsigned patched = 0;
    for (unsigned i = 0; i < kSiteCount; ++i) {
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Text) - (base + kSites[i] + 5));
        if (WriteBytes(base + kSites[i] + 1, &rel, 4)) ++patched;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    installed = patched == kSiteCount;
    std::fprintf(log, "credits block %s: sites %u/%u, title='%s' role='%s' names=%u blank_lines=%u -> %u sections before the original credits\n",
                 installed ? "on" : "PARTLY patched", patched, kSiteCount, title, role, nameCount, blankLines, ourCount);
    for (unsigned j = 0; j < nameCount; ++j) std::fprintf(log, "credits name %u: '%s'\n", j + 1, names[j]);
}

void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    const LONG sum = starts + lookups * 3 + countRejected * 7;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "credits starts=%ld original_sections=%d ours=%u lookups=%ld our_texts=%ld shifted=%ld rejected_counts=%ld\n", starts, originalCount,
                 ourCount, lookups, ourServed, shifted, countRejected);
}
}  // namespace modcredits
