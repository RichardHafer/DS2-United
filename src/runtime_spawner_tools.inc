// Dead Space 2 United by Richard Hafer.
bool spawnerToolsEnabled = false, spawnerIdentityVerified = false;
std::mutex spawnerToolsMutex;
ds2::SpawnerRules spawnerRules;
struct RecentSpawner {
    uint64_t sequence = 0;
    DWORD tick = 0;
    Guid source{}, target{}, selected{};
    unsigned status = 0, pointer = 0, actor = 0;
};
std::array<RecentSpawner, 256> recentSpawners{};
uint64_t recentSpawnerSequence = 0;
volatile LONG spawnerIdentityRejected = 0;
std::string spawnerRulesPath, spawnerRecentPath;

std::string SpawnerGuidText(const Guid& guid) {
    char value[36]{};
    std::snprintf(value, sizeof(value), "%08X-%08X-%08X-%08X", guid[0], guid[1], guid[2], guid[3]);
    return value;
}

bool ReadSpawnerIdentity(uint32_t object, uint32_t attributes, Guid& guid) {
    uint32_t vtable = 0, actualAttributes = 0;
    return spawnerIdentityVerified && object && object <= UINT32_MAX - 0x34 && Read(object, &vtable, 4) &&
        vtable == base + 0x1759C58 && Read(object + 0x0C, &actualAttributes, 4) &&
        actualAttributes == attributes && Read(object + 0x24, guid.data(), sizeof(guid)) && guid != Guid{};
}

// A template authored with 0 health spawns as a corpse on purpose
// (tools/find_corpse_templates.py). The ch11 convergence spawns one every few
// seconds and deletes it on a trigger it only reaches as a ragdoll, so a
// living stand-in piles up until the game crashes during the eyepoke scene.
// A spawn target a story scene names as its actor (generated_scene_actors.h).
bool IsSceneActor(const Guid& guid) {
    return std::binary_search(std::begin(ds2::kSceneActors), std::end(ds2::kSceneActors), guid);
}
bool IsCorpseTemplate(const Guid& guid) {
    for (const auto& row : ds2::kCorpseTemplates)
        if (guid[0] == row.guid[0] && guid[1] == row.guid[1] &&
            guid[2] == row.guid[2] && guid[3] == row.guid[3])
            return true;
    return false;
}

// The difficulty being played: 0 Casual, 1 Normal, 2 Survivalist, 3 Zealot, 4 Hard Core.
// The live global Options.difficulty (0x01F6031C, RVA 0x1B6031C), written by a new game,
// a save's load and the in-game change. `Game.PDiff` in settings.txt is not it: the game
// writes that only when a campaign is finished, as the best difficulty finished on
// (analysis/DIFFICULTY_NATIVE_2026-10-20.md section 2).
uint8_t currentDifficulty = ds2::kAnyDifficulty;

uint8_t ReadPlayerDifficulty() {
    int32_t value = -1;
    if (!Read(base + 0x1B6031C, &value, sizeof(value))) return ds2::kAnyDifficulty;
    return value >= 0 && value < ds2::kDifficultyCount ? static_cast<uint8_t>(value) : ds2::kAnyDifficulty;
}

ds2::SpawnerRule RuleForSpawner(uint32_t object, uint32_t attributes) {
    Guid guid{};
    if (!spawnerToolsEnabled || !ReadSpawnerIdentity(object, attributes, guid)) return {};
    const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
    const auto set = spawnerRules.find(guid);
    if (set == spawnerRules.end()) return {};
    // The row written for this difficulty, else the general one. A rule
    // written for other difficulties only is not there.
    const auto* rule = set->second.forDifficulty(currentDifficulty);
    return rule ? *rule : ds2::SpawnerRule{};
}

void RecordRecentSpawner(uint32_t object, uint32_t attributes, uint32_t actor,
    unsigned status, const Guid& selected) {
    if (!spawnerToolsEnabled) return;
    RecentSpawner entry;
    if (!ReadSpawnerIdentity(object, attributes, entry.source) ||
        !Read(static_cast<uintptr_t>(attributes) + 0x0C, entry.target.data(), sizeof(entry.target))) {
        InterlockedIncrement(&spawnerIdentityRejected);
        return;
    }
    entry.tick = GetTickCount();
    entry.pointer = object;
    entry.actor = actor;
    entry.status = status;
    entry.selected = selected;
    const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
    entry.sequence = ++recentSpawnerSequence;
    recentSpawners[(entry.sequence - 1) % recentSpawners.size()] = entry;
}

bool VerifySpawnerIdentity(FILE* log) {
    const unsigned char expected[] = {
        0x8B,0x4B,0x0C,0x8B,0x11,0x89,0x56,0x24,0x8B,0x51,0x04,0x89,0x56,0x28,
        0x8B,0x51,0x08,0x89,0x56,0x2C,0x8B,0x49,0x0C,0x89,0x4E,0x30
    };
    unsigned char actual[sizeof(expected)]{};
    spawnerIdentityVerified = Read(base + 0x3CF28D, actual, sizeof(actual)) &&
        !std::memcmp(actual, expected, sizeof(expected));
    std::fprintf(log, "spawner_tools identity_signature=%s guid_offset=24 rule_scope=source_guid\n",
        spawnerIdentityVerified ? "verified" : "mismatch_disabled");
    return spawnerIdentityVerified;
}

void PollSpawnerTools(FILE* log) {
    if (!spawnerToolsEnabled || !spawnerIdentityVerified) return;
    static std::string previousText;
    static bool loaded = false;
    static bool rulesFileMissing = false;
    static uint64_t writtenSequence = UINT64_MAX;
    static unsigned ruleGeneration = 0;
    static DWORD lastReadError = 0;
    static LONG lastRejected = -1;
    const LONG rejected = InterlockedCompareExchange(&spawnerIdentityRejected, 0, 0);
    if (rejected != lastRejected) {
        std::fprintf(log, "spawner_tools tick=%lu identity_or_target_read_rejected=%ld history_capacity=256\n",
            GetTickCount(), rejected);
        lastRejected = rejected;
    }
    const DWORD attributes = GetFileAttributesA(spawnerRulesPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        if (!loaded || !rulesFileMissing) {
            const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
            spawnerRules.clear();
            previousText.clear();
            loaded = true;
            rulesFileMissing = true;
            std::fprintf(log, "spawner_rules tick=%lu result=missing_use_defaults generation=%u\n",
                GetTickCount(), ++ruleGeneration);
        }
    } else {
        const bool wasMissing = rulesFileMissing;
        rulesFileMissing = false;
        std::ifstream file(spawnerRulesPath, std::ios::binary);
        std::string text;
        char buffer[4096];
        while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
            text.append(buffer, static_cast<size_t>(file.gcount()));
            if (text.size() > 1024 * 1024) break;
        }
        if (file.eof() && (!loaded || wasMissing || text != previousText)) {
            ds2::SpawnerRules candidate;
            if (ds2::parseSpawnerRules(text, candidate)) {
                const size_t count = ds2::spawnerRuleCount(candidate);
                size_t restricted = 0, scoped = 0;
                for (const auto& entry : candidate)
                    for (const auto& rule : entry.second.rows) {
                        restricted += !rule.families.empty();
                        scoped += !rule.general();
                    }
                const uint8_t difficulty = ReadPlayerDifficulty();
                {
                    const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
                    spawnerRules = std::move(candidate);
                    currentDifficulty = difficulty;
                }
                std::fprintf(log, "spawner_rules tick=%lu result=accepted entries=%zu restricted=%zu "
                    "difficulty_scoped=%zu playing=%s generation=%u\n",
                    GetTickCount(), count, restricted, scoped,
                    ds2::difficultyName(difficulty), ++ruleGeneration);
            } else {
                std::fprintf(log, "spawner_rules tick=%lu result=invalid_preserve_previous\n", GetTickCount());
            }
            previousText = std::move(text);
            loaded = true;
        } else if (!file.eof() && GetTickCount() - lastReadError >= 30000) {
            lastReadError = GetTickCount();
            std::fprintf(log, "spawner_rules tick=%lu result=read_failed_or_oversize_preserve_previous\n", lastReadError);
        }
    }
    // The rules only reload when their file changes; the difficulty changes on
    // its own schedule (a new game, a save's load, the pause menu), so the
    // global is read again every poll.
    {
        const uint8_t difficulty = ReadPlayerDifficulty();
        bool changed = false;
        {
            const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
            changed = difficulty != currentDifficulty;
            currentDifficulty = difficulty;
        }
        if (changed)
            std::fprintf(log, "spawner_rules tick=%lu result=difficulty_changed playing=%s source=global_01F6031C\n",
                GetTickCount(), ds2::difficultyName(difficulty));
    }
    std::array<RecentSpawner, 256> snapshot;
    uint64_t sequence = 0;
    {
        const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
        sequence = recentSpawnerSequence;
        if (sequence == writtenSequence) return;
        snapshot = recentSpawners;
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) { return a.sequence > b.sequence; });
    const std::string temporary = spawnerRecentPath + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << "pid\tsequence\ttick\tsource_guid\ttarget_guid\tselected_guid\tresult\tactor_created\n";
    for (const auto& entry : snapshot) {
        if (!entry.sequence) continue;
        file << GetCurrentProcessId() << '\t' << entry.sequence << '\t' << entry.tick << '\t'
             << SpawnerGuidText(entry.source) << '\t' << SpawnerGuidText(entry.target) << '\t'
             << SpawnerGuidText(entry.selected) << '\t' << ds2::statusName(entry.status) << '\t'
             << (entry.actor != 0) << '\n';
    }
    file.close();
    if (file && MoveFileExA(temporary.c_str(), spawnerRecentPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        writtenSequence = sequence;
    } else {
        std::fprintf(log, "spawner_recent tick=%lu result=write_failed win32=%lu\n", GetTickCount(), GetLastError());
    }
}
