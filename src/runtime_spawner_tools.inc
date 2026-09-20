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

// Which difficulty the player picked, straight out of the game's own settings.
//
// `Game.PDiff` in `%LOCALAPPDATA%\EA Games\Dead Space 2\settings.txt` is written
// by the game itself: 0 Casual, 1 Normal, 2 Survivalist, 3 Zealot, 4 Hard Core.
// Reading a file the game maintains beats hunting for the value in memory, and
// it cannot be wrong about what the menu was set to. It is re-read whenever the
// rules are, so switching difficulty needs no restart of anything but the level.
uint8_t currentDifficulty = ds2::kAnyDifficulty;

std::string PlayerSettingsPath() {
    char local[MAX_PATH]{};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) return {};
    return std::string(local) + "\\EA Games\\Dead Space 2\\settings.txt";
}

// Last write time of settings.txt, or zero when it cannot be read. The game
// rewrites the file when the player picks a difficulty for a new game, which
// happens after this plugin has already loaded the rules once. Watching the
// time stamp is what keeps a difficulty scoped rule honest in the very first
// level of a fresh game.
uint64_t PlayerSettingsWriteTime() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const std::string path = PlayerSettingsPath();
    if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

uint8_t ReadPlayerDifficulty() {
    const std::string path = PlayerSettingsPath();
    if (path.empty()) return ds2::kAnyDifficulty;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = line.substr(0, equals);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key != "Game.PDiff") continue;
        const int value = std::atoi(line.c_str() + equals + 1);
        return value >= 0 && value < ds2::kDifficultyCount
            ? static_cast<uint8_t>(value) : ds2::kAnyDifficulty;
    }
    return ds2::kAnyDifficulty;
}

ds2::SpawnerRule RuleForSpawner(uint32_t object, uint32_t attributes) {
    Guid guid{};
    if (!spawnerToolsEnabled || !ReadSpawnerIdentity(object, attributes, guid)) return {};
    const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
    const auto set = spawnerRules.find(guid);
    if (set == spawnerRules.end()) return {};
    // The row written for this difficulty, else the general one. A rule
    // written for other difficulties only is simply not there.
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
    // The rules only reload when their file changes, but the difficulty lives
    // in the game's own settings file and changes on its own schedule: a new
    // game picks it after the first load above has already happened. So the
    // settings file gets its own check, by time stamp, every poll.
    static uint64_t settingsWriteTime = 0;
    const uint64_t writeTime = PlayerSettingsWriteTime();
    if (writeTime && writeTime != settingsWriteTime) {
        settingsWriteTime = writeTime;
        const uint8_t difficulty = ReadPlayerDifficulty();
        bool changed = false;
        {
            const std::lock_guard<std::mutex> guard(spawnerToolsMutex);
            changed = difficulty != currentDifficulty;
            currentDifficulty = difficulty;
        }
        if (changed)
            std::fprintf(log, "spawner_rules tick=%lu result=difficulty_changed playing=%s\n",
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
