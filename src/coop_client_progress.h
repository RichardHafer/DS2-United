// Dead Space 2 United by Richard Hafer.
#pragma once
// A co-op client's own progress beside the host's campaign (user, 16.10.2026: every client
// session started with 0 credits and the four starting items, because every client save is
// held). One small text file per host, keyed by the host's id:
//
//   DS2ClientProgress 1
//   host 0123456789ABCDEF
//   map ds_2
//   credits 12000
//   nodes 4
//   guns PlasmaCutter,LineGun
//   suit 38CB666F:E989A6F7:54524542:314D4152
//   stack 39CABFB9:12345678:9ABCDEF0:11223344 3
//   tree 1A2B3C4D 5F6E7D8C,01020304
//   end 1A2B3C4D
//
// "stack" is one general inventory entry (item GUID, amount); "tree" a bench upgrade tree
// (its key) and the keys of its filled nodes. The last line carries FNV-1a over every byte
// before it. Unknown gun names are skipped and counted.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop_client_progress {
constexpr unsigned kVersion = 1;
constexpr unsigned kMaxStacks = 32, kMaxTrees = 64, kMaxTreeNodes = 48;
constexpr int kMaxGuns = 32;
constexpr long kMaxCredits = 9999999, kMaxNodes = 100000, kMaxAmount = 100000;

struct Stack { uint32_t guid[4]; uint32_t amount; };
struct Tree { uint32_t key; unsigned count; uint32_t nodes[kMaxTreeNodes]; };
struct File {
    unsigned version = kVersion;
    uint64_t host = 0;
    char map[32] = "";
    int32_t credits = 0, nodes = 0;
    uint32_t guns = 0;   // bits over the caller's gun names
    bool hasSuit = false;
    uint32_t suit[4]{};
    unsigned stackCount = 0;
    Stack stacks[kMaxStacks]{};
    unsigned treeCount = 0;
    Tree trees[kMaxTrees]{};
};
enum class Error { None, Empty, NotOurs, Version, Checksum, Syntax };

inline const char* ErrorName(Error e) {
    switch (e) {
    case Error::None: return "ok";
    case Error::Empty: return "empty";
    case Error::NotOurs: return "not_client_progress";
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

inline std::string FileName(uint64_t host) {
    char name[48];
    std::snprintf(name, sizeof(name), "host_%08X%08X.ds2client", static_cast<unsigned>(host >> 32), static_cast<unsigned>(host & 0xFFFFFFFFu));
    return name;
}

inline std::string Write(const File& f, const char* const* gunNames, int gunCount) {
    std::string out;
    char line[640];
    char map[32];
    size_t n = 0;
    for (const char* c = f.map; *c && n + 1 < sizeof(map); ++c) {
        const bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
        map[n++] = ok ? *c : '_';
    }
    map[n] = 0;
    std::snprintf(line, sizeof(line), "DS2ClientProgress %u\nhost %08X%08X\nmap %s\ncredits %ld\nnodes %ld\n", kVersion, static_cast<unsigned>(f.host >> 32),
                  static_cast<unsigned>(f.host & 0xFFFFFFFFu), map[0] ? map : "-", static_cast<long>(f.credits), static_cast<long>(f.nodes));
    out += line;
    std::string guns;
    for (int g = 0; g < gunCount && g < kMaxGuns; ++g) {
        if (!((f.guns >> g) & 1u) || !gunNames[g]) continue;
        if (!guns.empty()) guns += ',';
        guns += gunNames[g];
    }
    out += "guns ";
    out += guns.empty() ? "-" : guns;
    out += '\n';
    if (f.hasSuit) {
        std::snprintf(line, sizeof(line), "suit %08X:%08X:%08X:%08X\n", static_cast<unsigned>(f.suit[0]), static_cast<unsigned>(f.suit[1]),
                      static_cast<unsigned>(f.suit[2]), static_cast<unsigned>(f.suit[3]));
        out += line;
    }
    for (unsigned i = 0; i < f.stackCount && i < kMaxStacks; ++i) {
        const Stack& s = f.stacks[i];
        std::snprintf(line, sizeof(line), "stack %08X:%08X:%08X:%08X %u\n", static_cast<unsigned>(s.guid[0]), static_cast<unsigned>(s.guid[1]),
                      static_cast<unsigned>(s.guid[2]), static_cast<unsigned>(s.guid[3]), static_cast<unsigned>(s.amount));
        out += line;
    }
    for (unsigned i = 0; i < f.treeCount && i < kMaxTrees; ++i) {
        const Tree& t = f.trees[i];
        if (!t.count) continue;
        std::snprintf(line, sizeof(line), "tree %08X ", static_cast<unsigned>(t.key));
        out += line;
        for (unsigned k = 0; k < t.count && k < kMaxTreeNodes; ++k) {
            std::snprintf(line, sizeof(line), "%s%08X", k ? "," : "", static_cast<unsigned>(t.nodes[k]));
            out += line;
        }
        out += '\n';
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
    for (const char* c = text; *c; ++c)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'F') || (*c >= 'a' && *c <= 'f'))) return false;
    out = static_cast<uint32_t>(std::strtoul(text, nullptr, 16));
    return true;
}
inline bool Guid(char* text, uint32_t* out) {
    char* inner = nullptr;
    int n = 0;
    for (char* p = strtok_s(text, ":", &inner); p; p = strtok_s(nullptr, ":", &inner)) {
        if (n >= 4 || !Hex(p, out[n])) return false;
        ++n;
    }
    return n == 4;
}
}  // namespace detail

inline Error Parse(const char* text, size_t size, const char* const* gunNames, int gunCount, File& out, unsigned* unknownGuns = nullptr) {
    out = File{};
    if (!text || !size) return Error::Empty;
    const std::string all(text, size);
    if (all.compare(0, 18, "DS2ClientProgress ") != 0) return Error::NotOurs;
    const size_t endAt = all.rfind("end ");
    if (endAt == std::string::npos || (endAt && all[endAt - 1] != '\n')) return Error::Syntax;
    uint32_t sum = 0;
    {
        char hex[9]{};
        std::snprintf(hex, sizeof(hex), "%.8s", all.c_str() + endAt + 4);
        if (!detail::Hex(hex, sum)) return Error::Syntax;
    }
    if (sum != Fnv1a(all.data(), endAt)) return Error::Checksum;
    bool sawHost = false, sawCredits = false, sawNodes = false, sawGuns = false, sawVersion = false;
    size_t pos = 0;
    while (pos < endAt) {
        size_t eol = all.find('\n', pos);
        if (eol == std::string::npos || eol > endAt) eol = endAt;
        std::string lineText = all.substr(pos, eol - pos);
        pos = eol + 1;
        if (!lineText.empty() && lineText.back() == '\r') lineText.pop_back();
        if (lineText.empty()) continue;
        char line[1024];
        if (lineText.size() >= sizeof(line)) return Error::Syntax;
        std::memcpy(line, lineText.c_str(), lineText.size() + 1);
        char* rest = nullptr;
        char* word = strtok_s(line, " ", &rest);
        char* value = strtok_s(nullptr, " ", &rest);
        char* extra = strtok_s(nullptr, " ", &rest);
        if (!word) continue;
        long v = 0;
        if (!std::strcmp(word, "DS2ClientProgress")) {
            if (!detail::Number(value, 0, 1000, v)) return Error::Syntax;
            if (v != static_cast<long>(kVersion)) return Error::Version;
            sawVersion = true;
        } else if (!std::strcmp(word, "host")) {
            uint32_t hi = 0, lo = 0;
            char a[9]{}, b[9]{};
            if (!value || std::strlen(value) != 16) return Error::Syntax;
            std::memcpy(a, value, 8);
            std::memcpy(b, value + 8, 8);
            if (!detail::Hex(a, hi) || !detail::Hex(b, lo)) return Error::Syntax;
            out.host = (static_cast<uint64_t>(hi) << 32) | lo;
            sawHost = true;
        } else if (!std::strcmp(word, "map")) {
            std::snprintf(out.map, sizeof(out.map), "%s", value && std::strcmp(value, "-") ? value : "");
        } else if (!std::strcmp(word, "credits")) {
            if (!detail::Number(value, 0, kMaxCredits, v)) return Error::Syntax;
            out.credits = static_cast<int32_t>(v);
            sawCredits = true;
        } else if (!std::strcmp(word, "nodes")) {
            if (!detail::Number(value, 0, kMaxNodes, v)) return Error::Syntax;
            out.nodes = static_cast<int32_t>(v);
            sawNodes = true;
        } else if (!std::strcmp(word, "guns")) {
            sawGuns = true;
            if (!value || !std::strcmp(value, "-")) continue;
            char* inner = nullptr;
            for (char* g = strtok_s(value, ",", &inner); g; g = strtok_s(nullptr, ",", &inner)) {
                int found = -1;
                for (int k = 0; k < gunCount && k < kMaxGuns && found < 0; ++k)
                    if (gunNames[k] && !std::strcmp(gunNames[k], g)) found = k;
                if (found >= 0) out.guns |= 1u << found;
                else if (unknownGuns) ++*unknownGuns;
            }
        } else if (!std::strcmp(word, "suit")) {
            if (!value || !detail::Guid(value, out.suit)) return Error::Syntax;
            out.hasSuit = true;
        } else if (!std::strcmp(word, "stack")) {
            if (out.stackCount >= kMaxStacks) continue;
            Stack& s = out.stacks[out.stackCount];
            if (!value || !detail::Guid(value, s.guid) || !detail::Number(extra, 1, kMaxAmount, v)) return Error::Syntax;
            s.amount = static_cast<uint32_t>(v);
            ++out.stackCount;
        } else if (!std::strcmp(word, "tree")) {
            if (out.treeCount >= kMaxTrees) continue;
            Tree& t = out.trees[out.treeCount];
            t = Tree{};
            if (!detail::Hex(value, t.key) || !extra) return Error::Syntax;
            char* inner = nullptr;
            for (char* k = strtok_s(extra, ",", &inner); k; k = strtok_s(nullptr, ",", &inner)) {
                if (t.count >= kMaxTreeNodes) break;
                if (!detail::Hex(k, t.nodes[t.count])) return Error::Syntax;
                ++t.count;
            }
            if (t.count) ++out.treeCount;
        }
        // other words belong to a later minor addition and are skipped
    }
    if (!sawVersion || !sawHost || !sawCredits || !sawNodes || !sawGuns) return Error::Syntax;
    return Error::None;
}
// The reader returns 0 for a valid file, 1 for a missing file and -1 for a read error.
template<class ReadCopy>
int ReadForHost(uint64_t host, File& out, ReadCopy readCopy) {
    out = {};
    bool invalid = false;
    for (unsigned copy = 0; copy < 2; ++copy) {
        File candidate{};
        const int result = readCopy(copy, candidate);
        if (!result && host && candidate.host == host) {
            out = candidate;
            return 0;
        }
        if (result != 1) invalid = true;
    }
    return invalid ? -1 : 1;
}
}  // namespace coop_client_progress
