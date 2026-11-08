// Voice packs: pack files, line picks, who speaks with which pack, hearing, WAV, the wire.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../src/coop_voices.h"

namespace {
using namespace coop_voices;

void Packs() {
    Pack p;
    p.folder = "hafer";
    ParsePack("; comment\r\n[Pack]\r\nName = Hafer\r\n[pain]\r\npain_01.wav|Argh!\r\npain_02.wav\r\n# note\r\n[Grab_Slasher]\r\ngrab_slasher_01.wav | Get off me!\r\n"
              "[grab]\r\ngrab_01.wav\r\n[death]\r\n..\\evil.wav\r\nC:\\x.wav\r\n/abs.wav\r\ndeath_01.wav\r\n",
              p);
    assert(p.name == "Hafer" && p.Count() == 5);
    assert(p.lines["pain"].size() == 2 && p.lines["pain"][0].file == "pain_01.wav" && p.lines["pain"][0].subtitle == "Argh!" && p.lines["pain"][1].subtitle.empty());
    assert(p.lines["grab_slasher"][0].subtitle == "Get off me!");
    assert(p.lines["death"].size() == 1 && p.lines["death"][0].file == "death_01.wav");   // no file outside the folder
    assert(Lines(p, kGrab, "Slasher") == &p.lines["grab_slasher"] && Lines(p, kGrab, "leaper") == &p.lines["grab"] && Lines(p, kGrab, nullptr) == &p.lines["grab"]);
    assert(Lines(p, kReload, nullptr) == nullptr && Lines(p, kPain, "x") == &p.lines["pain"]);
    Pack unnamed;
    unnamed.folder = "folder_b";
    ParsePack("[melee]\nmelee.wav\n", unnamed);
    assert(unnamed.name == "folder_b" && unnamed.Count() == 1);
    Pack sub;   // voices\_example_pack: one folder per event
    ParsePack("[grab_slasher]\r\ngrab\\slasher\\grab_slasher_01.wav|Get off!\r\n[pain]\r\npain/pain_01.wav\r\n", sub);
    assert(sub.Count() == 2 && sub.lines["grab_slasher"][0].file == "grab\\slasher\\grab_slasher_01.wav" && sub.lines["pain"][0].file == "pain/pain_01.wav");
    std::printf("packs ok\n");
}

void Picks() {
    assert(Pick(0, 0, 0.5f) == 0 && Pick(1, 0, 0.9f) == 0);
    for (int i = 0; i < 100; ++i) {
        const float r = i / 100.0f;
        assert(Pick(3, 1, r) != 1 && Pick(3, 1, r) < 3);
        assert(Pick(2, 0, r) == 1 && Pick(2, 1, r) == 0);
        assert(Pick(4, SIZE_MAX, r) < 4);
    }
    Rule r = DefaultRule(kReload);
    assert(r.chance == 50 && r.cooldownMs == 6000);
    assert(ParseRule("30,1500", r) && r.chance == 30 && r.cooldownMs == 1500);
    assert(ParseRule("80", r) && r.chance == 80 && r.cooldownMs == 1500);
    assert(!ParseRule("101,5", r) && !ParseRule("x", r) && !ParseRule("50,", r) && !ParseRule("50,7x", r) && !ParseRule("", r) && r.chance == 80);
    assert(Priority(kDeath) > Priority(kGrab) && Priority(kGrab) > Priority(kPain) && Priority(kPain) > Priority(kStomp));
    // situations: after test, named for the pack sections, details on one byte
    assert(kLowHealth > kTest && IsSituation(kNecroSpotted) && !IsSituation(kTest) && !IsSituation(kDeath));
    assert(std::strcmp(EventName(kLowHealth), "low_health") == 0 && std::strcmp(EventName(kNecroSpotted), "necro_spotted") == 0);
    assert(DefaultRule(kLowAmmo).chance == 60 && DefaultRule(kNewArea).cooldownMs == 90000);
    assert(Priority(kLowHealth) == Priority(kPain) && Priority(kGrab) > Priority(kBigEnemy) && Priority(kMultiKill) == 0);
    assert(DetailCode("empty") == 1 && DetailCode("Tormentor") == 6 && DetailCode("ch04") == 12 && DetailCode("nope") == 0 && DetailCode(nullptr) == 0);
    assert(std::strcmp(DetailName(DetailCode("guardian")), "guardian") == 0 && DetailName(0)[0] == 0 && DetailName(200)[0] == 0);
    assert(MapKey("ch04_asc") == "ch04" && MapKey("CH11_MTB") == "ch11" && MapKey("ds_2").empty() && MapKey("ch1").empty() && MapKey("ch045").empty());
    std::printf("picks ok\n");
}

void Assignment() {
    std::vector<Pack> packs(4);
    const char* names[4] = {"alpha", "bravo", "charlie", "delta"};
    for (int i = 0; i < 4; ++i) { packs[i].folder = names[i]; packs[i].name = std::string("Name ") + names[i]; }
    assert(FindPack(packs, "CHARLIE") == 2 && FindPack(packs, "name bravo") == 1 && FindPack(packs, "") == -1 && FindPack(packs, "zulu") == -1);
    const std::vector<int> all = ParseDefaults(packs, "");
    assert(all.size() == 4 && all[3] == 3);
    const std::vector<int> some = ParseDefaults(packs, "delta, alpha,zulu");
    assert(some.size() == 2 && some[0] == 3 && some[1] == 0);
    // humans by machine, then the host's bots; more characters than packs wrap round
    assert(OrderOf(0, 2) == 0 && OrderOf(1, 2) == 1 && OrderOf(kBotKey, 2) == 2 && OrderOf(kBotKey + 2, 2) == 4);
    assert(PackFor(packs, all, "", 0, 1) == 0 && PackFor(packs, all, "", kBotKey, 1) == 1 && PackFor(packs, all, "", kBotKey + 2, 1) == 3);
    assert(PackFor(packs, all, "", 3, 4) == 3 && PackFor(packs, all, "", kBotKey, 4) == 0);   // a fifth character: the first pack again
    assert(PackFor(packs, all, "bravo", 0, 1) == 1 && PackFor(packs, all, "zulu", 2, 3) == 2);   // a pick this machine lacks: the default
    assert(PackFor(packs, all, "alpha", 1, 2) == 0 && PackFor(packs, all, "alpha", 2, 3) == 0);   // the same pack twice is fine
    assert(DefaultPack({}, 3) == -1 && PackFor({}, {}, "x", 0, 1) == -1);
    assert(IsBotKey(kBotKey) && !IsBotKey(15) && !IsBotKey(kBotKey + 16));
    std::printf("assignment ok\n");
}

void Hearings() {
    assert(DistanceGain(2.0f, 3.0f, 60.0f) == 1.0f && DistanceGain(60.0f, 3.0f, 60.0f) == 0.0f && DistanceGain(-1.0f, 3.0f, 60.0f) == 0.0f);
    const float g10 = DistanceGain(10.0f, 3.0f, 60.0f), g30 = DistanceGain(30.0f, 3.0f, 60.0f), g55 = DistanceGain(55.0f, 3.0f, 60.0f);
    assert(g10 < 1.0f && g10 > g30 && g30 > g55 && g55 > 0.0f);
    assert(std::fabs(g10 - std::pow(0.3f, 0.8f)) < 1e-4f);
    const Hearing open = Hear(10.0f, false, -1.0f, false, 3.0f, 60.0f), back = Hear(10.0f, false, -1.0f, true, 3.0f, 60.0f);
    assert(open.gain == g10 && open.cutoff == kOpenCutoff && back.cutoff == kBehindCutoff && back.gain == g10);
    const Hearing wall = Hear(10.0f, true, -1.0f, false, 3.0f, 60.0f), around = Hear(10.0f, true, 30.0f, false, 3.0f, 60.0f);
    assert(wall.cutoff == kWallCutoff && std::fabs(wall.gain - g10 * 0.45f) < 1e-5f);
    assert(around.cutoff == kAroundCutoff && std::fabs(around.gain - g30 * 0.75f) < 1e-5f);
    assert(Hear(10.0f, true, 80.0f, false, 3.0f, 60.0f).cutoff == kWallCutoff);   // a walk past the reach counts as none
    assert(Approach(0.0f, 1.0f, 0.25f) == 0.25f && Approach(1.0f, 0.0f, 0.25f) == 0.75f && Approach(0.9f, 1.0f, 0.25f) == 1.0f && Approach(0.1f, 0.0f, 0.25f) == 0.0f);
    std::printf("hearing ok\n");
}

std::vector<uint8_t> Wav(unsigned format, unsigned channels, unsigned rate, unsigned bits, const std::vector<uint8_t>& data, bool extensible = false) {
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
    const unsigned fmtSize = extensible ? 40 : 16;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    u32(4 + 8 + fmtSize + 8 + 4 + 8 + static_cast<uint32_t>(data.size()));
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(fmtSize);
    u16(static_cast<uint16_t>(extensible ? 0xFFFE : format));
    u16(static_cast<uint16_t>(channels));
    u32(rate);
    u32(rate * channels * bits / 8);
    u16(static_cast<uint16_t>(channels * bits / 8));
    u16(static_cast<uint16_t>(bits));
    if (extensible) {
        u16(22);
        u16(static_cast<uint16_t>(bits));
        u32(0);
        u16(static_cast<uint16_t>(format));
        for (int i = 0; i < 14; ++i) out.push_back(0);
    }
    out.insert(out.end(), {'L', 'I', 'S', 'T'});   // a chunk to skip
    u32(4);
    out.insert(out.end(), {'x', 'y', 'z', 'w'});
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    u32(static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

void Wavs() {
    Sound s;
    std::string error;
    // 16 bit stereo: left 1000, right 3000 -> mono 2000 (within rounding)
    const std::vector<uint8_t> st = Wav(1, 2, 44100, 16, {0xE8, 0x03, 0xB8, 0x0B, 0x18, 0xFC, 0x48, 0xF4});
    assert(ParseWav(st.data(), st.size(), s, error) && s.rate == 44100 && s.samples.size() == 2);
    assert(s.samples[0] >= 1998 && s.samples[0] <= 2001 && s.samples[1] <= -1998 && s.samples[1] >= -2001);
    // 24 bit mono: 0x400000 = half scale
    const std::vector<uint8_t> m24 = Wav(1, 1, 48000, 24, {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0});
    assert(ParseWav(m24.data(), m24.size(), s, error) && s.samples.size() == 2 && s.samples[0] >= 16380 && s.samples[0] <= 16384 && s.samples[1] <= -16380);
    // float 32 through WAVE_FORMAT_EXTENSIBLE: 0.5
    const std::vector<uint8_t> f32 = Wav(3, 1, 22050, 32, {0x00, 0x00, 0x00, 0x3F}, true);
    assert(ParseWav(f32.data(), f32.size(), s, error) && s.rate == 22050 && s.samples.size() == 1 && s.samples[0] >= 16380 && s.samples[0] <= 16384);
    // 8 bit unsigned: 128 is silence
    const std::vector<uint8_t> u8 = Wav(1, 1, 8000, 8, {128, 255, 0});
    assert(ParseWav(u8.data(), u8.size(), s, error) && s.samples[0] == 0 && s.samples[1] > 32000 && s.samples[2] == -32767);
    // refused
    const std::vector<uint8_t> adpcm = Wav(2, 1, 22050, 4, {1, 2, 3, 4});
    assert(!ParseWav(adpcm.data(), adpcm.size(), s, error) && !error.empty());
    const uint8_t junk[16] = {'R', 'I', 'F', 'X'};
    assert(!ParseWav(junk, sizeof(junk), s, error));
    std::vector<uint8_t> cut = st;
    cut.resize(cut.size() - 4);   // a data chunk cut short keeps the whole frames that are there
    assert(ParseWav(cut.data(), cut.size(), s, error) && s.samples.size() == 1);
    cut.resize(cut.size() - 2);
    assert(!ParseWav(cut.data(), cut.size(), s, error));
    std::printf("wav ok\n");
}

void Isaac() {
    // every hash is the engine's hash of its name, none twice, no breathing in the set
    for (size_t i = 0; i < sizeof(kVocals) / sizeof(kVocals[0]); ++i) {
        assert(SoundHash(kVocals[i].name) == kVocals[i].hash);
        assert(std::strstr(kVocals[i].name, "breath") == nullptr && std::strstr(kVocals[i].name, "asphyx") == nullptr);
        for (size_t j = i + 1; j < sizeof(kVocals) / sizeof(kVocals[0]); ++j) assert(kVocals[i].hash != kVocals[j].hash);
    }
    assert(sizeof(kVocals) / sizeof(kVocals[0]) == 54);
    assert(SoundHash("VX_PC_ATTACK_LRG.EMX") == 0x19D5CADBu && SoundHash("k_vx_pc_asphyx_in.emx") == 0x301D875Au);
    assert(EventFor(kVoPain, false) == kPain && EventFor(kVoPain, true) == kGrab && EventFor(kVoStruggle, false) == kGrab);
    assert(EventFor(kVoGrunt, false) == -1 && EventFor(kVoGrunt, true) == kGrab && EventFor(kVoStomp, true) == kStomp && EventFor(kVoMelee, false) == kMelee);
    assert(EventFor(kVoDeath, false) == kDeath && EventFor(kVoQuiet, true) == -1 && EventFor(kVoNone, false) == -1);
    assert(CctKindOf("pc_death") == kCctDeath && CctKindOf("aud_char_pc_asphyxiation_death") == kCctDeath && CctKindOf("aud_isaac_stomp_event") == kCctStompStream);
    assert(CctKindOf("isaac_electrocute_pain") == kCctShock && CctKindOf("aud_vx_pc_grunt_sml") == kCctNone && CctKindOf(nullptr) == kCctNone);
    std::printf("isaac ok\n");
}

void Wire() {
    Packet p{};
    p.magic = kMagic;
    p.version = kVersion;
    p.token = 7;
    p.count = 2;
    p.humans = 2;
    assert(Valid(p));
    p.count = 6;
    assert(!Valid(p));
    p.count = 1;
    p.token = 0;
    assert(!Valid(p));
    p.token = 1;
    p.version = 2;
    assert(!Valid(p));
    assert(sizeof(Packet) == 112 && sizeof(Entry) == 20);
    std::printf("wire ok\n");
}
void GameAudio() {
    // the user's own settings.txt, 19.10.2026 (CRLF)
    const std::string text = "Audio.AltNameColor = false\r\nAudio.MusicVol = 0.03000091\r\nAudio.Output = 0\r\nAudio.SFXVol = 0.25000089\r\n"
                             "Audio.Subtitles = false\r\nAudio.VoiceVol = 0.21000093\r\nControl.InvertX = false\r\n";
    GameVolumes v;
    assert(ParseGameVolumes(text, v));
    assert(v.found == 7);
    assert(std::fabs(v.voice - 0.21f) < 1e-4f && std::fabs(v.sfx - 0.25f) < 1e-4f && std::fabs(v.music - 0.03f) < 1e-4f);
    GameVolumes kept;
    kept.voice = 0.5f;
    assert(!ParseGameVolumes("Window.Width = 1920\n", kept) && kept.voice == 0.5f);   // nothing read: the old values stay
    assert(ParseGameVolumes("Audio.VoiceVol = 7\nAudio.SFXVol = -1\nAudio.MusicVol = x\n", v));
    assert(v.voice == 1.0f && v.sfx == 0.0f && v.music == 1.0f && v.found == 3);   // clamped; an unreadable one is not found
    // the focus fade: 150 ms for a full swing, both ways
    float g = 1.0f;
    g = FocusStep(g, false, 75, 150);
    assert(std::fabs(g - 0.5f) < 1e-5f);
    g = FocusStep(g, false, 200, 150);
    assert(g == 0.0f);
    g = FocusStep(g, true, 0, 150);
    assert(g == 0.0f);
    g = FocusStep(g, true, 300, 150);
    assert(g == 1.0f);
    assert(FocusStep(0.3f, false, 10, 0) == 0.0f);
    std::printf("game audio ok\n");
}
}  // namespace

int main() {
    Packs();
    Picks();
    Assignment();
    Hearings();
    Wavs();
    Isaac();
    Wire();
    GameAudio();
    std::printf("coop_voices_tests: all passed\n");
    return 0;
}
