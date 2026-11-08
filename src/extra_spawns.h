// Dead Space 2 United by Richard Hafer.
#pragma once
// Extra spawns: enemies a spawner brings along, placed where the designer
// stood. One row per placement in plugins\DS2ExtraSpawns.tsv:
//
//   source_guid  kind  count  chance  x  y  z  yaw  arrival
//
// source_guid is the spawner's identity (the placement readout lists the
// nearest ones), kind a family (Guardian, Stalker, Zombie ...) or a pool
// package name (super_slasher, stalker_crawling ...), count how many, chance
// in percent decided by the seed, x y z yaw the spot and facing from the
// readout, arrival "direct" (placed at x y z) or "here" (left wherever the
// spawner puts it, for vent spawners). Columns after chance may be empty;
// an empty arrival means direct when x y z are given and here otherwise.
//
// A row whose source_guid is `near` or `near:<metres>` (20.09.2026) is bound
// to no spawner at all: it fires once when the player comes within that many
// metres (default 20: close enough to be in the room, far enough not to
// watch it appear) of x y z, driven through whichever live spawner the
// session has seen whose own target still resolves. For rooms where nothing
// spawns when the door opens. Position required, arrival is always direct;
// the rules re-arm when the level's streams are torn down.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "spawner_rules.h"

namespace ds2 {
enum class ExtraArrival { Here, Direct };
struct ExtraSpawnRule {
    std::string kind;
    unsigned count = 1, chance = 100;
    bool hasPosition = false;
    float position[3]{};
    bool hasYaw = false;
    float yaw = 0.0f;
    ExtraArrival arrival = ExtraArrival::Here;
};
using ExtraSpawnRules = std::map<Guid, std::vector<ExtraSpawnRule>>;
struct ExtraAreaRule { ExtraSpawnRule rule; float radius = 20.0f; };
using ExtraAreaRules = std::vector<ExtraAreaRule>;

inline bool parseExtraNumber(const std::string& text, float& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    value = std::strtof(text.c_str(), &end);
    return end && *end == '\0';
}

inline bool parseExtraSpawns(const std::string& text, ExtraSpawnRules& output, ExtraAreaRules* areasOut = nullptr) {
    std::istringstream input(text);
    std::string line;
    auto next = [&]() {
        if (!std::getline(input, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!next() || line != "source_guid\tkind\tcount\tchance\tx\ty\tz\tyaw\tarrival") return false;
    ExtraSpawnRules parsed;
    ExtraAreaRules areas;
    size_t rows = 0;
    while (next()) {
        if (line.empty() || line[0] == '#') continue;
        if (++rows > 2048) return false;
        std::string columns[9];
        size_t count = 0, start = 0;
        for (;;) {
            const auto tab = line.find('\t', start);
            if (count >= 9) return false;
            columns[count++] = line.substr(start, tab == std::string::npos ? tab : tab - start);
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (count < 2) return false;
        Guid guid{};
        bool area = false;
        float radius = 20.0f;
        if (columns[0] == "near" || columns[0].rfind("near:", 0) == 0) {
            area = true;
            if (columns[0].size() > 5) {
                if (!parseExtraNumber(columns[0].substr(5), radius) || radius < 0.5f || radius > 100.0f) return false;
            }
        } else if (!parseSpawnerGuid(columns[0], guid)) return false;
        ExtraSpawnRule rule;
        rule.kind = columns[1];
        if (rule.kind.empty() || rule.kind.size() > 32) return false;
        for (char c : rule.kind) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        if (!columns[2].empty()) {
            const long value = std::strtol(columns[2].c_str(), nullptr, 10);
            if (value < 1 || value > 8) return false;
            rule.count = static_cast<unsigned>(value);
        }
        if (!columns[3].empty()) {
            const long value = std::strtol(columns[3].c_str(), nullptr, 10);
            if (value < 1 || value > 100) return false;
            rule.chance = static_cast<unsigned>(value);
        }
        const bool anyCoordinate = !columns[4].empty() || !columns[5].empty() || !columns[6].empty();
        if (anyCoordinate) {
            for (unsigned i = 0; i < 3; ++i)
                if (!parseExtraNumber(columns[4 + i], rule.position[i])) return false;
            rule.hasPosition = true;
        }
        if (!columns[7].empty()) {
            if (!parseExtraNumber(columns[7], rule.yaw)) return false;
            rule.hasYaw = true;
        }
        // An explicit "here" wins over a position: the editor fills x y z from
        // the readout whatever arrival is picked.
        if (columns[8] == "direct") rule.arrival = ExtraArrival::Direct;
        else if (columns[8] == "here") rule.arrival = ExtraArrival::Here;
        else if (columns[8].empty()) rule.arrival = rule.hasPosition ? ExtraArrival::Direct : ExtraArrival::Here;
        else return false;
        // "direct" without a spot has nowhere to go
        if (rule.arrival == ExtraArrival::Direct && !rule.hasPosition) return false;
        if (area) {
            // a spot is the whole point of an area rule, and it is placed there
            if (!rule.hasPosition) return false;
            rule.arrival = ExtraArrival::Direct;
            areas.push_back(ExtraAreaRule{std::move(rule), radius});
            continue;
        }
        parsed[guid].push_back(std::move(rule));
    }
    output = std::move(parsed);
    if (areasOut) *areasOut = std::move(areas);
    return true;
}

inline size_t extraSpawnCount(const ExtraSpawnRules& rules) {
    size_t total = 0;
    for (const auto& entry : rules) total += entry.second.size();
    return total;
}
}  // namespace ds2
