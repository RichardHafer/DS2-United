// Dead Space 2 United by Hafer.
#pragma once
// Voice packs for the players' characters (runtime_coop_voices.inc): pure logic,
// no game memory and no audio device. A pack is a folder under plugins\voices
// with a pack.ini: [Pack] Name=..., then one section per event, each line a WAV
// file of that folder and an optional subtitle ("pain_01.wav|Argh!").
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace coop_voices {

// What a character reacts to. A grab may name the enemy ("grab_slasher" before "grab").
// The situations come after test, so an older build drops them as unknown on the wire.
enum Event : int { kDeath = 0, kPain, kGrab, kReload, kStomp, kMelee, kTest, kLowHealth, kLowAmmo, kNewArea, kBigEnemy, kMultiKill, kNecroSpotted, kEventCount };
inline const char* EventName(int e) {
    static const char* const kNames[kEventCount] = {"death", "pain", "grab", "reload", "stomp", "melee", "test",
                                                    "low_health", "low_ammo", "new_area", "big_enemy", "multi_kill", "necro_spotted"};
    return e >= 0 && e < kEventCount ? kNames[e] : "?";
}
inline bool IsSituation(int e) { return e >= kLowHealth && e < kEventCount; }
// Who speaks first when two lines want the same speaker.
inline int Priority(int e) {
    return e == kDeath ? 3 : e == kGrab ? 2 : (e == kPain || e == kLowHealth || e == kBigEnemy || e == kNecroSpotted) ? 1 : 0;
}
// A situation's detail on the wire (the 'VOXE' reserved byte): an index into this table, 0 none.
constexpr unsigned kDetailCount = 25;
inline const char* DetailName(unsigned code) {
    static const char* const kDetails[kDetailCount] = {"",     "empty", "brute", "stalker", "tripod", "hunter", "tormentor", "nicole", "guardian",
                                                       "ch01", "ch02",  "ch03",  "ch04",    "ch05",   "ch06",   "ch07",      "ch08",   "ch09",
                                                       "ch10", "ch11",  "ch12",  "ch13",    "ch14",   "ch15",   "ch16"};
    return code < kDetailCount ? kDetails[code] : "";
}
inline unsigned DetailCode(const char* detail) {
    if (!detail || !*detail) return 0;
    for (unsigned i = 1; i < kDetailCount; ++i) {
        const char* a = DetailName(i);
        const char* b = detail;
        while (*a && (*b | 0x20) == *a) { ++a; ++b; }
        if (!*a && !*b) return i;
    }
    return 0;
}
// The chapter of a level name for new_area's detail: "ch04_asc" -> "ch04", else "".
inline std::string MapKey(const char* map) {
    if (!map || (map[0] | 0x20) != 'c' || (map[1] | 0x20) != 'h' || map[2] < '0' || map[2] > '9' || map[3] < '0' || map[3] > '9') return std::string();
    if (map[4] && map[4] != '_') return std::string();
    return std::string("ch") + map[2] + map[3];
}

// How often an event speaks: chance in percent, and the gap per speaker.
struct Rule { unsigned chance; unsigned cooldownMs; };
inline Rule DefaultRule(int e) {
    switch (e) {
    case kDeath: return {100, 0};
    case kPain: return {100, 2500};
    case kGrab: return {100, 1500};
    case kReload: return {50, 6000};
    case kStomp: return {100, 3000};
    case kMelee: return {100, 3000};
    case kLowHealth: return {100, 30000};
    case kLowAmmo: return {60, 45000};   // "empty" always speaks
    case kNewArea: return {35, 90000};
    case kBigEnemy: return {100, 20000};
    case kMultiKill: return {100, 15000};
    case kNecroSpotted: return {100, 25000};
    default: return {100, 0};
    }
}
// "100,2500": chance percent and cooldown ms. False leaves the rule as it was.
inline bool ParseRule(const char* text, Rule& rule) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const unsigned long chance = std::strtoul(text, &end, 10);
    if (end == text || chance > 100) return false;
    unsigned long cooldown = rule.cooldownMs;
    if (*end == ',') {
        const char* next = end + 1;
        cooldown = std::strtoul(next, &end, 10);
        if (end == next || cooldown > 600000) return false;
    }
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return false;
    rule = Rule{static_cast<unsigned>(chance), static_cast<unsigned>(cooldown)};
    return true;
}

inline std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}
inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

struct Line { std::string file, subtitle; };
struct Pack {
    std::string folder, name;
    std::map<std::string, std::vector<Line>> lines;   // event key, lower case
    size_t Count() const {
        size_t n = 0;
        for (const auto& kv : lines) n += kv.second.size();
        return n;
    }
};
// pack.ini; lines starting with ';' or '#' are comments. A file name may not leave the folder.
inline void ParsePack(const std::string& text, Pack& p) {
    std::string section;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = Trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        if (section == "pack") {
            const size_t eq = line.find('=');
            if (eq != std::string::npos && Lower(Trim(line.substr(0, eq))) == "name") p.name = Trim(line.substr(eq + 1));
            continue;
        }
        if (section.empty()) continue;
        const size_t bar = line.find('|');
        Line l{Trim(line.substr(0, bar)), bar == std::string::npos ? std::string() : Trim(line.substr(bar + 1))};
        if (l.file.empty() || l.file.find("..") != std::string::npos || l.file.find(':') != std::string::npos || l.file[0] == '\\' || l.file[0] == '/')
            continue;
        p.lines[section].push_back(l);
    }
    if (p.name.empty()) p.name = p.folder;
}
// The lines for an event: "<event>_<detail>" first, then the event itself.
inline const std::vector<Line>* Lines(const Pack& p, int e, const char* detail) {
    const std::string stem = EventName(e);
    if (detail && *detail) {
        const auto it = p.lines.find(stem + "_" + Lower(detail));
        if (it != p.lines.end() && !it->second.empty()) return &it->second;
    }
    const auto it = p.lines.find(stem);
    return it != p.lines.end() && !it->second.empty() ? &it->second : nullptr;
}
// No line twice in a row when there is a choice. roll01 in [0,1).
inline size_t Pick(size_t count, size_t last, float roll01) {
    if (count == 0) return 0;
    if (count == 1) return 0;
    if (last >= count) return static_cast<size_t>(roll01 * static_cast<float>(count)) % count;
    const size_t i = static_cast<size_t>(roll01 * static_cast<float>(count - 1)) % (count - 1);
    return i >= last ? i + 1 : i;
}

// --- who speaks with which pack -------------------------------------------------------------------
// Keys as the nameplates have them: 0..15 a machine (its human), 0xF0 + n the host's bot n.
constexpr unsigned kBotKey = 0xF0;
inline bool IsBotKey(unsigned key) { return key >= kBotKey && key < kBotKey + 16; }
// The place in the join order: humans by machine index (the host first), the host's bots after them.
inline unsigned OrderOf(unsigned key, unsigned humans) { return IsBotKey(key) ? humans + (key - kBotKey) : key; }
// A folder name or a pack's Name, any case. -1: not here.
inline int FindPack(const std::vector<Pack>& packs, const std::string& want) {
    const std::string w = Lower(Trim(want));
    if (w.empty()) return -1;
    for (size_t i = 0; i < packs.size(); ++i)
        if (Lower(packs[i].folder) == w) return static_cast<int>(i);
    for (size_t i = 0; i < packs.size(); ++i)
        if (Lower(packs[i].name) == w) return static_cast<int>(i);
    return -1;
}
// The defaults in join order, wrapping round when there are more characters than packs.
inline int DefaultPack(const std::vector<int>& defaults, unsigned order) {
    return defaults.empty() ? -1 : defaults[order % defaults.size()];
}
// The defaults list ("a,b,c,d"); names not found are skipped; empty: every pack in folder order.
inline std::vector<int> ParseDefaults(const std::vector<Pack>& packs, const std::string& list) {
    std::vector<int> out;
    size_t at = 0;
    while (at <= list.size()) {
        size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        const int i = FindPack(packs, list.substr(at, end - at));
        if (i >= 0) out.push_back(i);
        at = end + 1;
    }
    if (out.empty())
        for (size_t i = 0; i < packs.size(); ++i) out.push_back(static_cast<int>(i));
    return out;
}
// A character's pack: the one it chose when this machine has it, else its default.
inline int PackFor(const std::vector<Pack>& packs, const std::vector<int>& defaults, const std::string& chosen, unsigned key, unsigned humans) {
    const int c = FindPack(packs, chosen);
    return c >= 0 ? c : DefaultPack(defaults, OrderOf(key, humans));
}

// --- hearing ------------------------------------------------------------------------------------------
// Full volume within nearM, then falling off gently; nothing beyond farM.
inline float DistanceGain(float meters, float nearM, float farM) {
    if (!(meters >= 0.0f) || meters >= farM) return 0.0f;
    if (meters <= nearM) return 1.0f;
    const float g = std::pow(nearM / meters, 0.8f);
    // the last fifth fades to silence instead of stopping at a step
    const float fade = meters > farM * 0.8f ? (farM - meters) / (farM * 0.2f) : 1.0f;
    return g * fade;
}
// gain: volume factor; cutoff: low pass, 1 = open, smaller = more muffled.
struct Hearing { float gain, cutoff; };
constexpr float kOpenCutoff = 1.0f, kWallCutoff = 0.16f, kAroundCutoff = 0.3f, kBehindCutoff = 0.7f;
// distance: straight; blocked: a wall between the ears and the mouth; walk: the walk
// around it (<= 0: no walk known). Behind the listener a voice is a little duller.
inline Hearing Hear(float distance, bool blocked, float walk, bool behind, float nearM, float farM) {
    if (!blocked) return {DistanceGain(distance, nearM, farM), behind ? kBehindCutoff : kOpenCutoff};
    if (walk > 0.0f && walk < farM) return {DistanceGain(walk, nearM, farM) * 0.75f, kAroundCutoff};
    return {DistanceGain(distance, nearM, farM) * 0.45f, kWallCutoff};
}
// Toward a target by at most `step`, so a voice never jumps in volume or colour.
inline float Approach(float now, float target, float step) {
    if (now < target) return now + step < target ? now + step : target;
    return now - step > target ? now - step : target;
}

// --- sound files ------------------------------------------------------------------------------------
// WAV with PCM 8/16/24/32 bit or float 32, any channel count; out: 16 bit mono.
struct Sound { unsigned rate = 0; std::vector<int16_t> samples; };
inline uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline bool ParseWav(const uint8_t* data, size_t size, Sound& out, std::string& error) {
    out = Sound{};
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) { error = "not a RIFF WAVE file"; return false; }
    unsigned format = 0, channels = 0, rate = 0, bits = 0;
    const uint8_t* samples = nullptr;
    size_t bytes = 0;
    size_t at = 12;
    while (at + 8 <= size) {
        const uint32_t length = U32(data + at + 4);
        const uint8_t* body = data + at + 8;
        const size_t room = size - at - 8;
        const size_t have = length < room ? length : room;
        if (std::memcmp(data + at, "fmt ", 4) == 0 && have >= 16) {
            format = U16(body);
            channels = U16(body + 2);
            rate = U32(body + 4);
            bits = U16(body + 14);
            if (format == 0xFFFE && have >= 26) format = U16(body + 24);   // WAVE_FORMAT_EXTENSIBLE: the sub format's first word
        } else if (std::memcmp(data + at, "data", 4) == 0) {
            samples = body;
            bytes = have;
        }
        at += 8 + static_cast<size_t>(length) + (length & 1);
    }
    if (!format || !samples) { error = "no fmt or data chunk"; return false; }
    if (!channels || channels > 8 || rate < 8000 || rate > 192000) { error = "unusual channels or sample rate"; return false; }
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32), flt = format == 3 && bits == 32;
    if (!pcm && !flt) { error = "only PCM 8/16/24/32 bit or float 32"; return false; }
    const size_t step = bits / 8 * channels;
    const size_t frames = bytes / step;
    if (!frames) { error = "no samples"; return false; }
    out.rate = rate;
    out.samples.resize(frames);
    for (size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            const uint8_t* s = samples + f * step + c * (bits / 8);
            float v = 0.0f;
            if (flt) {
                uint32_t u = U32(s);
                std::memcpy(&v, &u, 4);
            } else if (bits == 8) {
                v = (static_cast<int>(s[0]) - 128) / 128.0f;
            } else if (bits == 16) {
                v = static_cast<int16_t>(U16(s)) / 32768.0f;
            } else if (bits == 24) {
                const int32_t x = static_cast<int32_t>((s[0] << 8) | (s[1] << 16) | (static_cast<uint32_t>(s[2]) << 24)) >> 8;
                v = x / 8388608.0f;
            } else {
                v = static_cast<int32_t>(U32(s)) / 2147483648.0f;
            }
            sum += v;
        }
        float m = sum / static_cast<float>(channels) * 32767.0f;
        if (!(m == m)) m = 0.0f;
        out.samples[f] = static_cast<int16_t>(m > 32767.0f ? 32767 : m < -32768.0f ? -32768 : m);
    }
    return true;
}

// --- Isaac's own voice ------------------------------------------------------------------------------
// The vocal sounds a Player's animation events play (analysis/ISAAC_VOICE_SOUNDS_2026-10-16.md), by the
// hash Entity::PlaySound gets: sdbm32 of the lower case name with ".emx". The suit's breathing and the
// asphyxiation breaths are not in it: they stay.
enum Vocal : uint8_t { kVoNone = 0, kVoPain, kVoStruggle, kVoGrunt, kVoStomp, kVoMelee, kVoDeath, kVoQuiet };
struct VocalName { uint32_t hash; const char* name; Vocal kind; };
constexpr VocalName kVocals[] = {
    {0x9E9F834Eu, "char_pc_vo_crushed_death.emx", kVoDeath},
    {0x922D4631u, "char_pc_vo_electrocution.emx", kVoPain},
    {0x5506F507u, "char_pc_vo_electrocution_grondo.emx", kVoPain},
    {0x0E04E2D3u, "char_pc_vo_exert.emx", kVoQuiet},
    {0xC455D800u, "char_pc_vo_helmetless_exert.emx", kVoQuiet},
    {0x61730EE3u, "char_pc_vo_helmetless_hit_react_hg.emx", kVoPain},
    {0x73766181u, "char_pc_vo_helmetless_hit_react_lrg.emx", kVoPain},
    {0x6D1AB223u, "char_pc_vo_helmetless_panic.emx", kVoStruggle},
    {0x860DB655u, "char_pc_vo_helmetless_struggle.emx", kVoStruggle},
    {0x166C5EF8u, "char_pc_vo_helmetless_struggle_non_spatialized.emx", kVoStruggle},
    {0x9EC90654u, "char_pc_vo_hit_react_lrg.emx", kVoPain},
    {0x78668224u, "char_pc_vo_hit_react_lrg_grondo.emx", kVoPain},
    {0xB5F66EC7u, "char_pc_vo_hit_react_lrg_grondo_non_spatialized.emx", kVoPain},
    {0xCAB032F7u, "char_pc_vo_hit_react_lrg_non_spatialized.emx", kVoPain},
    {0x88EDE79Fu, "char_pc_vo_hit_react_med.emx", kVoPain},
    {0x1E126842u, "char_pc_vo_hit_react_med_non_spatialized.emx", kVoPain},
    {0xB6C9BCF6u, "char_pc_vo_panic.emx", kVoStruggle},
    {0x4CE439C2u, "char_pc_vo_struggle.emx", kVoStruggle},
    {0xA18EEF36u, "char_pc_vo_struggle_grondo.emx", kVoStruggle},
    {0xAE31DC0Eu, "char_pc_vo_struggle_no_helmet.emx", kVoStruggle},
    {0xAC110FD7u, "char_pc_vo_struggle_no_helmet_02.emx", kVoStruggle},
    {0xA456AE65u, "char_pc_vo_struggle_non_spatialized.emx", kVoStruggle},
    {0x19D5CADBu, "vx_pc_attack_lrg.emx", kVoStomp},
    {0x1CC579DDu, "vx_pc_attack_lrg_grondo.emx", kVoStomp},
    {0x1CAA9F38u, "vx_pc_attack_lrg_short.emx", kVoMelee},
    {0xEDA71465u, "vx_pc_grunt_lrg.emx", kVoGrunt},
    {0xED057D08u, "vx_pc_grunt_lrg_non_spatialized.emx", kVoGrunt},
    {0xD7CBF5B0u, "vx_pc_grunt_med.emx", kVoGrunt},
    {0x4067B253u, "vx_pc_grunt_med_non_spatialized.emx", kVoGrunt},
    {0xF81A8138u, "vx_pc_grunt_med_pt5to1.emx", kVoGrunt},
    {0xAD938EB6u, "vx_pc_grunt_sml.emx", kVoGrunt},
    {0x7E983359u, "vx_pc_grunt_sml_non_spatialized.emx", kVoGrunt},
    {0x58035CEDu, "vx_pc_grunt_sml_pt13to21.emx", kVoGrunt},
    {0x67B4D8F1u, "vx_pc_grunt_sml_pt13to25.emx", kVoGrunt},
    {0x7438D2B2u, "vx_pc_grunt_sml_pt5to1.emx", kVoGrunt},
    {0x95A928FDu, "vx_pc_helmet_eye_stab_scream.emx", kVoQuiet},
    {0x83AFA628u, "vx_pc_helmet_eye_stab_scream_02.emx", kVoQuiet},
    {0xAFDF6156u, "vx_pc_helmetless_attack_lrg.emx", kVoStomp},
    {0x2393CD33u, "vx_pc_helmetless_attack_lrg_short.emx", kVoMelee},
    {0xC2E92F2Au, "vx_pc_helmetless_grunt_lrg.emx", kVoGrunt},
    {0xAD0E1075u, "vx_pc_helmetless_grunt_med.emx", kVoGrunt},
    {0xA5F43918u, "vx_pc_helmetless_grunt_med_non_spatialized.emx", kVoGrunt},
    {0xFF03AF33u, "vx_pc_helmetless_grunt_med_pt5to1.emx", kVoGrunt},
    {0x82D5A97Bu, "vx_pc_helmetless_grunt_sml.emx", kVoGrunt},
    {0xE424BA1Eu, "vx_pc_helmetless_grunt_sml_non_spatialized.emx", kVoGrunt},
    {0x1EBF3D68u, "vx_pc_helmetless_grunt_sml_pt13to21.emx", kVoGrunt},
    {0x2E70B96Cu, "vx_pc_helmetless_grunt_sml_pt13to25.emx", kVoGrunt},
    {0x7B2200ADu, "vx_pc_helmetless_grunt_sml_pt5to1.emx", kVoGrunt},
    {0x3FA2F64Du, "vx_pc_helmetless_hit_react_lrg.emx", kVoPain},
    {0xA474B5EBu, "vx_pc_helmetless_hit_react_lrg_grondo.emx", kVoPain},
    {0x4BDA068Eu, "vx_pc_helmetless_hit_react_lrg_grondo_non_spatialized.emx", kVoPain},
    {0x6145BEF0u, "vx_pc_helmetless_hit_react_lrg_non_spatialized.emx", kVoPain},
    {0x29C7D798u, "vx_pc_helmetless_hit_react_med.emx", kVoPain},
    {0xB4A7F43Bu, "vx_pc_helmetless_hit_react_med_non_spatialized.emx", kVoPain},
};
// The engine's sound name hash (0x007423C0).
inline uint32_t SoundHash(const char* name) {
    uint32_t h = 0;
    for (; name && *name; ++name) {
        const unsigned c = static_cast<unsigned char>(*name >= 'A' && *name <= 'Z' ? *name + 32 : *name);
        h = c + (h << 6) + (h << 16) - h;
    }
    return h;
}
// What a muted vocal means: a grabbed character's pain, struggle and grunts are the grab;
// other grunts (zero-g, aim walks) and the rest say nothing. -1: no event.
inline int EventFor(Vocal v, bool grabbed) {
    switch (v) {
    case kVoPain: return grabbed ? kGrab : kPain;
    case kVoStruggle: return kGrab;
    case kVoGrunt: return grabbed ? kGrab : -1;
    case kVoStomp: return kStomp;
    case kVoMelee: return kMelee;
    case kVoDeath: return kDeath;
    default: return -1;
    }
}
// The Lua CCT events (aud_cct.lua) that matter: a death of the character, and the two 2D vocals
// (the repeated stomp's yelling stream, the shock scream) that are skipped. 0 = none of them.
enum CctKind : int { kCctNone = 0, kCctDeath, kCctStompStream, kCctShock };
inline int CctKindOf(const char* event) {
    if (!event) return kCctNone;
    if (std::strcmp(event, "pc_death") == 0 || std::strcmp(event, "aud_char_pc_asphyxiation_death") == 0) return kCctDeath;
    if (std::strcmp(event, "aud_isaac_stomp_event") == 0) return kCctStompStream;
    if (std::strcmp(event, "isaac_electrocute_pain") == 0) return kCctShock;
    return kCctNone;
}

// --- the wire ----------------------------------------------------------------------------------------
// Every 2 s each machine sends its own choice ("" = its default), the host its bots'
// packs too and how many humans it counts, so every machine orders alike.
constexpr uint32_t kMagic = 0x50584F56u;   // 'VOXP'
constexpr uint8_t kVersion = 1;
#pragma pack(push, 1)
struct Entry { uint8_t key; char pack[19]; };
struct Packet { uint32_t magic; uint8_t version, count; uint16_t humans; uint32_t token; Entry entries[5]; };
#pragma pack(pop)
static_assert(sizeof(Packet) == 112, "voice packet size");
inline bool Valid(const Packet& p) { return p.magic == kMagic && p.version == kVersion && p.token && p.count <= 5 && p.humans <= 16; }

// The game's own sliders, from %LOCALAPPDATA%\EA Games\Dead Space 2\settings.txt
// ("Audio.VoiceVol = 0.21000093"), each 0..1. `found` has a bit per slider read.
struct GameVolumes {
    float sfx = 1.0f, voice = 1.0f, music = 1.0f;
    unsigned found = 0;
};
inline bool ParseGameVolumes(const std::string& text, GameVolumes& out) {
    GameVolumes v;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equals));
        float* slot = key == "Audio.SFXVol" ? &v.sfx : key == "Audio.VoiceVol" ? &v.voice : key == "Audio.MusicVol" ? &v.music : nullptr;
        if (!slot) continue;
        const std::string value = Trim(line.substr(equals + 1));
        char* stop = nullptr;
        const float f = std::strtof(value.c_str(), &stop);
        if (stop == value.c_str() || !std::isfinite(f)) continue;
        *slot = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        v.found |= slot == &v.sfx ? 1u : slot == &v.voice ? 2u : 4u;
    }
    if (!v.found) return false;
    out = v;
    return true;
}
// Our sound fades out while the game is not the focused window and back in when it is.
inline float FocusStep(float gain, bool focused, unsigned dtMs, unsigned fadeMs) {
    const float step = fadeMs ? static_cast<float>(dtMs) / static_cast<float>(fadeMs) : 1.0f;
    const float target = focused ? 1.0f : 0.0f;
    if (gain < target) return gain + step >= target ? target : gain + step;
    return gain - step <= target ? target : gain - step;
}

}  // namespace coop_voices
