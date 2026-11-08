// Dead Space 2 United by Richard Hafer.
#pragma once
// The bots' state beside one campaign save: a small text file, keyed by the
// save's own header hash (SDBM over header and body, header +4) and body size
// (header +0x6C), so it can only ever match the save it was written for.
//
//   DS2BotSave 1
//   key 7B120DE1 0003AC28 ds_2
//   slot 8
//   bot 1 name=Vasquez credits=1200 nodes=3 guns=PulseRifle,LineGun suit=38CB666F:E989A6F7:54524542:314D4152 seeded=1
//   end 1A2B3C4D
//
// The last line carries FNV-1a over every byte before it. Unknown gun names are
// skipped (counted), so a renamed catalogue entry costs that gun and nothing else.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop_bot_save {
constexpr unsigned kVersion = 1;
constexpr unsigned kMaxBots = 3;
constexpr int kMaxGuns = 32;
constexpr int32_t kMaxCredits = 9999999, kMaxNodes = 100000;

struct Key {
    uint32_t hash = 0, bodyUsed = 0;
    char map[32] = "";
};
struct Bot {
    bool present = false;
    char name[32] = "";
    int32_t credits = 0, nodes = 0;
    uint32_t guns = 0;   // bits over the caller's gun names
    bool hasSuit = false;
    uint32_t suit[4]{};
    bool seeded = true;  // its wallet got its starting credits; files without the key are older and had real wallets
};
struct File {
    unsigned version = kVersion;
    Key key;
    int slot = -1;   // 1-based as in ds_slot_NN; informational
    Bot bots[kMaxBots];
};
enum class Error { None, Empty, NotOurs, Version, Checksum, Syntax };

inline const char* ErrorName(Error e) {
    switch (e) {
    case Error::None: return "ok";
    case Error::Empty: return "empty";
    case Error::NotOurs: return "not_a_bot_save";
    case Error::Version: return "version";
    case Error::Checksum: return "checksum";
    default: return "syntax";
    }
}

inline uint32_t Fnv1a(const char* data, size_t size) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < size; ++i) h = (h ^ static_cast<unsigned char>(data[i])) * 0x01000193u;
    return h;
}

// Names go into a space separated line: anything outside [A-Za-z0-9_-] becomes '_'.
inline void CleanName(const char* in, char* out, size_t size) {
    size_t n = 0;
    for (; in && *in && n + 1 < size; ++in) {
        const char c = *in;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        out[n++] = ok ? c : '_';
    }
    out[n] = 0;
}

inline std::string FileName(int slot, const Key& key) {
    char name[64];
    std::snprintf(name, sizeof(name), "slot%02d_%08X_%08X.ds2bots", slot < 0 ? 0 : slot, static_cast<unsigned>(key.hash),
                  static_cast<unsigned>(key.bodyUsed));
    return name;
}

inline std::string Write(const File& f, const char* const* gunNames, int gunCount) {
    std::string out;
    char line[512];
    char map[32];
    CleanName(f.key.map, map, sizeof(map));
    std::snprintf(line, sizeof(line), "DS2BotSave %u\nkey %08X %08X %s\nslot %d\n", kVersion, static_cast<unsigned>(f.key.hash),
                  static_cast<unsigned>(f.key.bodyUsed), map[0] ? map : "-", f.slot);
    out += line;
    for (unsigned i = 0; i < kMaxBots; ++i) {
        const Bot& b = f.bots[i];
        if (!b.present) continue;
        char name[32];
        CleanName(b.name, name, sizeof(name));
        std::string guns;
        for (int g = 0; g < gunCount && g < kMaxGuns; ++g) {
            if (!((b.guns >> g) & 1u) || !gunNames[g]) continue;
            if (!guns.empty()) guns += ',';
            guns += gunNames[g];
        }
        char suit[48] = "-";
        if (b.hasSuit)
            std::snprintf(suit, sizeof(suit), "%08X:%08X:%08X:%08X", static_cast<unsigned>(b.suit[0]), static_cast<unsigned>(b.suit[1]),
                          static_cast<unsigned>(b.suit[2]), static_cast<unsigned>(b.suit[3]));
        std::snprintf(line, sizeof(line), "bot %u name=%s credits=%ld nodes=%ld guns=%s suit=%s seeded=%d\n", i + 1, name[0] ? name : "-",
                      static_cast<long>(b.credits), static_cast<long>(b.nodes), guns.empty() ? "-" : guns.c_str(), suit, b.seeded ? 1 : 0);
        out += line;
    }
    std::snprintf(line, sizeof(line), "end %08X\n", static_cast<unsigned>(Fnv1a(out.data(), out.size())));
    out += line;
    return out;
}

namespace detail {
inline bool Number(const char* text, long lo, long hi, long& out) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (!end || *end || v < lo || v > hi) return false;
    out = v;
    return true;
}
inline bool Hex(const char* text, uint32_t& out) {
    if (!text || std::strlen(text) != 8) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 16);
    if (!end || *end) return false;
    out = static_cast<uint32_t>(v);
    return true;
}
// One "bot N k=v ..." line; tokens are separated by single spaces.
inline bool BotLine(char* text, const char* const* gunNames, int gunCount, File& f, unsigned* unknownGuns) {
    char* rest = nullptr;
    char* token = strtok_s(text, " ", &rest);   // "bot"
    token = strtok_s(nullptr, " ", &rest);
    long index = 0;
    if (!Number(token, 1, kMaxBots, index)) return false;
    Bot& b = f.bots[index - 1];
    if (b.present) return false;
    b = Bot{};
    b.present = true;
    bool credits = false, nodes = false, guns = false, suit = false;
    while ((token = strtok_s(nullptr, " ", &rest)) != nullptr) {
        char* eq = std::strchr(token, '=');
        if (!eq) return false;
        *eq = 0;
        const char* value = eq + 1;
        long v = 0;
        if (!std::strcmp(token, "name")) {
            std::snprintf(b.name, sizeof(b.name), "%s", std::strcmp(value, "-") ? value : "");
        } else if (!std::strcmp(token, "credits")) {
            if (!Number(value, 0, kMaxCredits, v)) return false;
            b.credits = static_cast<int32_t>(v);
            credits = true;
        } else if (!std::strcmp(token, "nodes")) {
            if (!Number(value, 0, kMaxNodes, v)) return false;
            b.nodes = static_cast<int32_t>(v);
            nodes = true;
        } else if (!std::strcmp(token, "guns")) {
            guns = true;
            if (!std::strcmp(value, "-")) continue;
            char list[256];
            std::snprintf(list, sizeof(list), "%s", value);
            char* inner = nullptr;
            for (char* g = strtok_s(list, ",", &inner); g; g = strtok_s(nullptr, ",", &inner)) {
                int found = -1;
                for (int k = 0; k < gunCount && k < kMaxGuns && found < 0; ++k)
                    if (gunNames[k] && !std::strcmp(gunNames[k], g)) found = k;
                if (found >= 0) b.guns |= 1u << found;
                else if (unknownGuns) ++*unknownGuns;
            }
        } else if (!std::strcmp(token, "suit")) {
            suit = true;
            if (!std::strcmp(value, "-")) continue;
            char parts[48];
            std::snprintf(parts, sizeof(parts), "%s", value);
            char* inner = nullptr;
            int n = 0;
            for (char* p = strtok_s(parts, ":", &inner); p; p = strtok_s(nullptr, ":", &inner)) {
                if (n >= 4 || !Hex(p, b.suit[n])) return false;
                ++n;
            }
            if (n != 4) return false;
            b.hasSuit = true;
        } else if (!std::strcmp(token, "seeded")) {
            if (!Number(value, 0, 1, v)) return false;
            b.seeded = v != 0;
        }
        // other keys belong to a later minor addition and are skipped
    }
    return credits && nodes && guns && suit;
}
}  // namespace detail

inline Error Parse(const std::string& text, const char* const* gunNames, int gunCount, File& out, unsigned* unknownGuns = nullptr) {
    if (unknownGuns) *unknownGuns = 0;
    if (text.empty()) return Error::Empty;
    if (text.compare(0, 11, "DS2BotSave ") != 0) return Error::NotOurs;
    {
        long version = 0;
        const size_t eol = text.find('\n');
        if (eol == std::string::npos || !detail::Number(text.substr(11, eol - 11).c_str(), 0, 1000000, version)) return Error::Syntax;
        if (static_cast<unsigned long>(version) != kVersion) return Error::Version;
    }
    // The checksum line ends the file; nothing may follow it.
    const size_t endLine = text.rfind("\nend ");
    if (endLine == std::string::npos) return Error::Checksum;
    const size_t endAt = endLine + 1;
    {
        std::string tail = text.substr(endAt + 4);
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
        uint32_t stored = 0;
        if (!detail::Hex(tail.c_str(), stored) || stored != Fnv1a(text.data(), endAt)) return Error::Checksum;
    }
    File f;
    bool key = false, slot = false;
    size_t at = text.find('\n') + 1;
    while (at < endAt) {
        size_t eol = text.find('\n', at);
        if (eol == std::string::npos || eol > endAt) eol = endAt;
        char line[512];
        const size_t len = eol - at;
        if (len >= sizeof(line)) return Error::Syntax;
        std::memcpy(line, text.data() + at, len);
        line[len] = 0;
        at = eol + 1;
        if (!line[0]) continue;
        if (!std::strncmp(line, "key ", 4)) {
            char* rest = nullptr;
            const char* hash = strtok_s(line + 4, " ", &rest);
            const char* used = strtok_s(nullptr, " ", &rest);
            const char* map = strtok_s(nullptr, " ", &rest);
            if (!map || strtok_s(nullptr, " ", &rest) || std::strlen(map) >= sizeof(f.key.map) || !detail::Hex(hash, f.key.hash) ||
                !detail::Hex(used, f.key.bodyUsed))
                return Error::Syntax;
            std::snprintf(f.key.map, sizeof(f.key.map), "%s", std::strcmp(map, "-") ? map : "");
            key = true;
        } else if (!std::strncmp(line, "slot ", 5)) {
            long v = 0;
            if (!detail::Number(line + 5, -1, 99, v)) return Error::Syntax;
            f.slot = static_cast<int>(v);
            slot = true;
        } else if (!std::strncmp(line, "bot ", 4)) {
            if (!detail::BotLine(line, gunNames, gunCount, f, unknownGuns)) return Error::Syntax;
        } else {
            return Error::Syntax;
        }
    }
    if (!key || !slot) return Error::Syntax;
    out = f;
    return Error::None;
}
}  // namespace coop_bot_save
