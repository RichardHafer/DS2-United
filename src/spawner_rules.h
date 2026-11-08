// Dead Space 2 United by Hafer.
#pragma once
#include "selection.h"
#include <charconv>
#include <sstream>

namespace ds2 {
enum class SpawnerMode { Default, Block, Allow };

// Which difficulty a rule is in force on. The game writes the player's choice
// into its own settings file as `Game.PDiff`, so this needs no code in the game
// at all: 0 Casual, 1 Normal, 2 Survivalist, 3 Zealot, 4 Hard Core. The internal
// names behind them are Easy, Hard, Impossible and Hardcore, which is why the
// menu entry for Zealot is `$UIMenu_Impossible`.
enum : uint8_t { kAnyDifficulty = 0xFF, kDifficultyCount = 5, kEveryDifficulty = 0x1F };
inline const char* difficultyName(uint8_t value) {
    static constexpr const char* names[] = {"casual", "normal", "survivalist", "zealot", "hardcore"};
    return value < kDifficultyCount ? names[value] : "any";
}
inline uint8_t difficultyValue(const std::string& text) {
    for (uint8_t i = 0; i < kDifficultyCount; ++i)
        if (text == difficultyName(i)) return i;
    return kAnyDifficulty;
}
// "zealot;hardcore" into a bit per difficulty. Empty, unknown or repeated
// names refuse the row, the same way a bad family name does.
inline bool parseDifficulties(const std::string& text, uint8_t& mask) {
    uint8_t parsed = 0;
    size_t start = 0;
    for (;;) {
        const auto separator = text.find(';', start);
        const auto end = separator == std::string::npos ? text.size() : separator;
        const uint8_t value = difficultyValue(text.substr(start, end - start));
        if (value == kAnyDifficulty || (parsed >> value) & 1) return false;
        parsed |= static_cast<uint8_t>(1 << value);
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    mask = parsed;
    return true;
}
inline std::string difficultyList(uint8_t mask) {
    std::string text;
    for (uint8_t i = 0; i < kDifficultyCount; ++i) {
        if (!((mask >> i) & 1)) continue;
        if (!text.empty()) text += ';';
        text += difficultyName(i);
    }
    return text;
}

// One rule for one spawner: what to do with it, and optionally which enemy
// families this particular spawner can actually cope with.
//
// Not every spawner works with every enemy, and nothing offline says which.
// `spawn_slasher_hospital_triage_1` takes a ceiling-vent donor through the
// level's own vent, so a Super Puker passes every authored check, and then
// never comes alive: the watchdog found an actor with no readable state
// machines and killed it after ninety seconds, with nothing ever visible in the
// room. Only playing it finds that out, so the answer has to be writable.
//
// The list only ever narrows. Anything the INI switched off stays off, and a
// family named here still has to pass every gate in buildPlan; a spawner can
// never be talked into an enemy the configuration already refused.
struct SpawnerRule {
    SpawnerMode mode = SpawnerMode::Default;
    std::vector<std::string> families;
    // One bit per difficulty the rule is in force on; all five bits set means
    // always. A scripted encounter that survives Normal can fall apart on
    // Zealot, where the same room holds more and tougher enemies, so the answer
    // has to be per rule, and one rule may cover several difficulties at once.
    uint8_t difficulties = kEveryDifficulty;

    bool allowsFamily(const std::string& family) const {
        if (families.empty()) return true;
        for (const auto& allowed : families) if (allowed == family) return true;
        return false;
    }
    bool general() const { return difficulties == kEveryDifficulty; }
    bool appliesTo(uint8_t current) const {
        return general() || (current < kDifficultyCount && ((difficulties >> current) & 1));
    }
    bool operator==(const SpawnerRule& other) const {
        return mode == other.mode && families == other.families &&
               difficulties == other.difficulties;
    }
    bool operator!=(const SpawnerRule& other) const { return !(*this == other); }
};

// Every rule written for one spawner. At most one of them is general, the
// others name difficulties that do not overlap, so for any difficulty there
// is exactly one answer: the row written for it, or else the general one.
struct SpawnerRuleSet {
    std::vector<SpawnerRule> rows;

    const SpawnerRule* forDifficulty(uint8_t current) const {
        const SpawnerRule* fallback = nullptr;
        for (const auto& rule : rows) {
            if (rule.general()) fallback = &rule;
            else if (rule.appliesTo(current)) return &rule;
        }
        return fallback;
    }
    // False when the row would answer a difficulty another row already covers.
    bool accepts(const SpawnerRule& rule) const {
        for (const auto& existing : rows) {
            if (existing.general() && rule.general()) return false;
            if (!existing.general() && !rule.general() && (existing.difficulties & rule.difficulties)) return false;
        }
        return true;
    }
    bool operator==(const SpawnerRuleSet& other) const { return rows == other.rows; }
    bool operator!=(const SpawnerRuleSet& other) const { return !(*this == other); }
};
using SpawnerRules = std::map<Guid, SpawnerRuleSet>;
inline size_t spawnerRuleCount(const SpawnerRules& rules) {
    size_t count = 0;
    for (const auto& entry : rules) count += entry.second.rows.size();
    return count;
}

inline bool parseSpawnerGuid(const std::string& text, Guid& guid) {
    if (text.size() != 35) return false;
    Guid parsed{};
    for (unsigned i = 0; i < 4; ++i) {
        const char* start = text.data() + i * 9;
        const auto result = std::from_chars(start, start + 8, parsed[i], 16);
        if (result.ec != std::errc{} || result.ptr != start + 8 ||
            (i < 3 && text[i * 9 + 8] != '-')) return false;
    }
    if (parsed == Guid{}) return false;
    guid = parsed;
    return true;
}

// "Zombie;Puker". Family names are compared against the catalog's own spelling,
// so anything that cannot be a family name is a typo and refuses the file.
inline bool parseSpawnerFamilies(const std::string& text, std::vector<std::string>& output) {
    std::vector<std::string> parsed;
    size_t start = 0;
    for (;;) {
        const auto separator = text.find(';', start);
        const auto end = separator == std::string::npos ? text.size() : separator;
        const auto name = text.substr(start, end - start);
        if (name.empty() || name.size() > 32 || parsed.size() >= 64) return false;
        for (const char letter : name)
            if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z') ||
                  (letter >= '0' && letter <= '9') || letter == '_')) return false;
        for (const auto& seen : parsed) if (seen == name) return false;
        parsed.push_back(name);
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    output = std::move(parsed);
    return true;
}

inline bool parseSpawnerRules(const std::string& text, SpawnerRules& output) {
    if (text.size() > 1024 * 1024) return false;
    std::istringstream input(text);
    std::string line;
    auto next = [&]() {
        if (!std::getline(input, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    // Three headers are accepted, one per generation of the file. Anything
    // written before families or difficulties existed keeps working unchanged.
    if (!next()) return false;
    const bool withDifficulty = line == "source_guid\tmode\tfamilies\tdifficulty";
    const bool withFamilies = withDifficulty || line == "source_guid\tmode\tfamilies";
    if (!withFamilies && line != "source_guid\tmode") return false;
    SpawnerRules parsed;
    size_t rows = 0;
    while (next()) {
        if (line.empty()) continue;
        if (rows >= 8192) return false;
        std::string columns[4];
        size_t count = 0, start = 0;
        for (;;) {
            const auto tab = line.find('\t', start);
            if (count >= 4) return false;
            columns[count++] = line.substr(start, tab == std::string::npos ? tab : tab - start);
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (count < 2) return false;
        if (count > 3 && !withDifficulty) return false;
        if (count > 2 && !withFamilies) return false;
        Guid guid{};
        if (!parseSpawnerGuid(columns[0], guid)) return false;
        const std::string& mode = columns[1];
        const std::string& families = columns[2];
        SpawnerRule rule;
        if (mode == "block") rule.mode = SpawnerMode::Block;
        else if (mode == "allow") rule.mode = SpawnerMode::Allow;
        else if (mode == "default" && withFamilies) rule.mode = SpawnerMode::Default;
        else return false;
        if (!columns[3].empty() && !parseDifficulties(columns[3], rule.difficulties)) return false;
        if (!families.empty() && !parseSpawnerFamilies(families, rule.families)) return false;
        // A blocked spawner never selects anything, so a list on it would be a
        // contradiction rather than a refinement.
        if (rule.mode == SpawnerMode::Block && !rule.families.empty()) return false;
        // And a row that neither changes the mode nor restricts anything is a
        // mistake, not a rule.
        if (rule.mode == SpawnerMode::Default && rule.families.empty()) return false;
        // One spawner may carry several rows, as long as no two of them
        // answer the same difficulty.
        auto& set = parsed[guid];
        if (!set.accepts(rule)) return false;
        set.rows.push_back(std::move(rule));
        ++rows;
    }
    output = std::move(parsed);
    return true;
}

inline SelectionPlan buildAllowedSpawnerPlan(PoolSettings settings, EnemyRecord source,
    const std::vector<EnemyRecord>& pool, unsigned maxDonors) {
    source.sourceEnabled = true;
    settings.chance = 100;
    return buildPlan(settings, source, pool, maxDonors);
}
}
