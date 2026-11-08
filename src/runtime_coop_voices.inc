// Dead Space 2 United by Richard Hafer.
// Voice packs ([Voices] in DS2United.ini; coop_voices.h). Every player and bot speaks
// with recorded lines from plugins\voices\<pack>\, played by our own XAudio2 engine
// (xaudio2_9.dll, loaded at start; X3DAudio comes from the same DLL) at the
// speaker's head, panned from the camera:
// - a wall between the local Isaac's head and the speaker's mouth
//   (CollisionCamera::CastingTest through bot_sense::CastRaw) muffles the voice;
// - then it comes along the NavPower walk around the wall (bot_nav::Query), from the
//   walk's first turn and as loud as the walk is long;
// - behind the camera it is a little duller.
// Who speaks with which pack: [Voices] Pack when a player picked one, else the
// defaults in join order (humans by machine index, the host first, then the host's
// bots). Each machine sends its pick every 2 s ('VOXP', coop_voices::Packet), the
// host its bots' packs and how many humans it counts; the host passes the clients'
// on. Events come in through Say() from any thread; the lines play on the game
// thread (Tick).
// Isaac's own voice (analysis/ISAAC_VOICE_SOUNDS_2026-10-16.md): every vocal of a
// Player comes from its animation events and ends in Entity::PlaySound(hash, opts)
// (Player vtable +0xA4, 0x007D1610, thiscall ret 8). While voices run the slot is
// ours: a vocal hash plays nothing (0, which its callers expect) and says what
// happened, the grabber read from the paired coordinator (+0x2A8). The Lua CCT
// handler (0x00FF6AD0) tells a death and starts two 2D vocals, the repeated stomp's
// yelling and the shock scream, which are skipped for a Player. A reload has no
// vocal: PlayerReloadSM::PlayReloadAnim (0x00874470) tells it. Deaths and reloads
// of the own Isaac and the host's bots also go to the partners ('VOXE').
namespace turret { uint32_t SimMs(); }   // runtime_coop_turret.inc: game time, stands still in the pause
namespace marks { unsigned NecroGhosts(uint32_t* out, unsigned cap); }   // runtime_coop_marks.inc: necro_spotted
namespace voices {
bool enabled = true, subtitles = true, installed = false, muteIsaac = true;
std::string folder;                       // plugins\voices
std::vector<coop_voices::Pack> packs;
std::vector<int> defaults;
std::string ownPick;                      // [Voices] Pack
int volumePercent = 100;
// Follow the game's own Voice and SFX sliders, and fall silent while the game is not focused.
bool followGameVolume = true, muteUnfocused = true;
coop_voices::GameVolumes gameVolumes;
uint64_t settingsWriteTime = 0;
DWORD settingsCheckedAt = 0, focusAt = 0;
float focusGain = 1.0f, masterNow = -1.0f;
volatile LONG settingsReads = 0, settingsFailures = 0, unfocusedTicks = 0;
float nearM = 3.0f, farM = 60.0f;
coop_voices::Rule rules[coop_voices::kEventCount]{};

// --- the audio device ---------------------------------------------------------------------------
using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
using X3DInitFn = HRESULT(__cdecl*)(UINT32, FLOAT32, X3DAUDIO_HANDLE);
using X3DCalcFn = void(__cdecl*)(const X3DAUDIO_HANDLE, const X3DAUDIO_LISTENER*, const X3DAUDIO_EMITTER*, UINT32, X3DAUDIO_DSP_SETTINGS*);
HMODULE dll = nullptr;
IXAudio2* engine = nullptr;
IXAudio2MasteringVoice* master = nullptr;
X3DAUDIO_HANDLE x3d{};
X3DCalcFn calc = nullptr;
unsigned outChannels = 2;
DWORD channelMask = 0;
bool engineRunning = true;
volatile LONG faults = 0;

// --- sounds, loaded off the game thread -------------------------------------------------------------
struct Load {
    std::string path;
    coop_voices::Sound sound;
    std::string error;
    volatile LONG state = 0;   // 0 waiting, 1 loaded, 2 failed
    DWORD usedAt = 0;
    size_t Bytes() const { return sound.samples.size() * 2; }
};
SRWLOCK loadLock = SRWLOCK_INIT;
std::map<std::string, std::shared_ptr<Load>> cache;   // by path
std::vector<std::shared_ptr<Load>> todo;
HANDLE wake = nullptr;
constexpr size_t kCacheBytes = 24u << 20;
volatile LONG loadsOk = 0, loadsFailed = 0, cacheDrops = 0;
DWORD WINAPI LoadThread(void*) {
    for (;;) {
        WaitForSingleObject(wake, INFINITE);
        for (;;) {
            std::shared_ptr<Load> job;
            AcquireSRWLockExclusive(&loadLock);
            if (!todo.empty()) { job = todo.front(); todo.erase(todo.begin()); }
            ReleaseSRWLockExclusive(&loadLock);
            if (!job) break;
            std::vector<uint8_t> bytes;
            FILE* f = nullptr;
            if (fopen_s(&f, job->path.c_str(), "rb") == 0 && f) {
                uint8_t chunk[16384];
                size_t got = 0;
                while (bytes.size() < (64u << 20) && (got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
                std::fclose(f);
            }
            bool ok = false;
            if (bytes.empty()) job->error = "file missing or empty";
            else ok = coop_voices::ParseWav(bytes.data(), bytes.size(), job->sound, job->error);
            InterlockedIncrement(ok ? &loadsOk : &loadsFailed);
            InterlockedExchange(&job->state, ok ? 1 : 2);
        }
    }
}
// The game thread asks; the answer is ready when state turns 1 or 2.
std::shared_ptr<Load> Request(const std::string& path, DWORD now) {
    AcquireSRWLockExclusive(&loadLock);
    auto it = cache.find(path);
    std::shared_ptr<Load> load;
    if (it != cache.end()) {
        load = it->second;
    } else {
        // over the budget: drop what nobody plays, oldest first
        size_t total = 0;
        for (const auto& kv : cache) total += kv.second->Bytes();
        while (total > kCacheBytes) {
            auto oldest = cache.end();
            for (auto c = cache.begin(); c != cache.end(); ++c)
                if (c->second.use_count() == 1 && c->second->state != 0 && (oldest == cache.end() || c->second->usedAt < oldest->second->usedAt)) oldest = c;
            if (oldest == cache.end()) break;
            total -= oldest->second->Bytes();
            cache.erase(oldest);
            InterlockedIncrement(&cacheDrops);
        }
        load = std::make_shared<Load>();
        load->path = path;
        cache[path] = load;
        todo.push_back(load);
    }
    load->usedAt = now;
    ReleaseSRWLockExclusive(&loadLock);
    if (load->state == 0) SetEvent(wake);
    return load;
}

// --- who speaks ----------------------------------------------------------------------------------------
std::string picks[256];          // by key: what that machine sent ("" = its default)
unsigned hostHumans = 0;          // what the host counts (clients order the bots by it)
DWORD lastSpoke[256][coop_voices::kEventCount]{};
size_t lastLine[256][coop_voices::kEventCount]{};
uint32_t lastGrab[256]{};        // the coordinator of the grab last spoken for
int packSeen[256];               // the pack last logged per key, -2 never
unsigned Humans() {
    if (!network) return 1;
    if (IsHost()) return 1 + peerTable.Count();
    return hostHumans ? hostHumans : 1 + peerTable.Count();
}
unsigned KeyOf(uint32_t entity) {
    if (!entity) return 0xFFFFFFFFu;
    if (entity == LocalSlotZeroEntity()) return machineIndex & 0xFF;
    for (unsigned i = 0; i < kMaxBots; ++i)
        if (botEntities[i] == entity) return coop_voices::kBotKey + i;
    const uint32_t m = MachineOf(entity);
    return m == kNoMachine ? 0xFFFFFFFFu : (m & 0xFF);
}
uint32_t EntityOf(unsigned key) {
    if (key == (machineIndex & 0xFF)) return LocalSlotZeroEntity();
    if (coop_voices::IsBotKey(key) && key - coop_voices::kBotKey < kMaxBots && botEntities[key - coop_voices::kBotKey]) return botEntities[key - coop_voices::kBotKey];
    return GhostForMachine(key);
}
int PackOf(unsigned key) {
    const std::string& chosen = key == (machineIndex & 0xFF) ? ownPick : picks[key & 0xFF];
    const int p = coop_voices::PackFor(packs, defaults, chosen, key, Humans());
    if (p != packSeen[key & 0xFF]) {
        packSeen[key & 0xFF] = p;
        gate::Event("voices: %s %u speaks with %s (%s, join order %u of %u humans)", coop_voices::IsBotKey(key) ? "bot" : "machine",
                    coop_voices::IsBotKey(key) ? key - coop_voices::kBotKey : key, p >= 0 ? packs[p].folder.c_str() : "NO PACK",
                    !chosen.empty() && coop_voices::FindPack(packs, chosen) >= 0 ? "picked" : chosen.empty() ? "default" : "picked pack missing here, default",
                    coop_voices::OrderOf(key, Humans()), Humans());
    }
    return p;
}

// --- events ----------------------------------------------------------------------------------------
struct Ask { uint32_t entity; int event; char detail[24]; DWORD at; uint32_t context; };
SRWLOCK askLock = SRWLOCK_INIT;
std::vector<Ask> asks;
volatile LONG asked = 0, askDropped = 0;
// Any thread. detail: the grabbing enemy's kind and the like ("slasher"), may be null.
// context: the grab's paired coordinator, so one grab speaks once.
void Say(uint32_t entity, int event, const char* detail, DWORD delayMs = 0, uint32_t context = 0) {
    if (!enabled || !installed || event < 0 || event >= coop_voices::kEventCount) return;
    Ask a{entity, event, {}, GetTickCount() + delayMs, context};
    if (detail) std::snprintf(a.detail, sizeof(a.detail), "%s", detail);
    AcquireSRWLockExclusive(&askLock);
    if (asks.size() < 64) asks.push_back(a);
    else InterlockedIncrement(&askDropped);
    ReleaseSRWLockExclusive(&askLock);
    InterlockedIncrement(&asked);
}

// --- playing ------------------------------------------------------------------------------------------
struct Active {
    IXAudio2SourceVoice* voice = nullptr;
    std::shared_ptr<Load> load;
    unsigned key = 0;
    int event = 0, pack = -1;
    std::string subtitle;
    DWORD startedAt = 0, castAt = 0, walkAt = 0, updateAt = 0;
    bool started = false, blocked = false, haveVia = false;
    float gain = 0.0f, cutoff = coop_voices::kOpenCutoff, walk = -1.0f;
    float via[3]{}, last[3]{};
    bool haveLast = false;
    // a phantom (the Dementia mutator): heard on this machine only, from a fixed spot, at its own gain
    bool phantom = false;
    float phantomGain = 1.0f;
    std::string who;
    bool sfx = false;   // the game's SFX slider instead of its Voice slider
};
constexpr unsigned kMaxActive = 8;
std::vector<Active> active;
volatile LONG played = 0, skippedChance = 0, skippedCooldown = 0, skippedBusy = 0, noPack = 0, noLine = 0, noSpeaker = 0, voiceFailures = 0;
volatile LONG blockedCasts = 0, openCasts = 0, walks = 0, walksFound = 0, interrupted = 0, lineLines = 0;
volatile LONG phantomsAsked = 0, phantomsPlayed = 0, phantomsRefused = 0;
constexpr unsigned kPhantomKey = 0x1000;   // outside every speaker key, so no line ever waits for it
unsigned rng = 0x1234567u;
float Roll() {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<float>(rng >> 8) / 16777216.0f;
}
void Stop(Active& a) {
    if (a.voice) {
        a.voice->DestroyVoice();
        a.voice = nullptr;
    }
    a.load.reset();
}
void StopAll() {
    for (Active& a : active) Stop(a);
    active.clear();
    overlay::HideSubtitles();
}
// The speaker's head; false when the body is gone (the voice keeps its last place).
bool HeadOf(unsigned key, float* out) {
    const uint32_t e = EntityOf(key);
    float p[3]{};
    if (!e || !Read(e + 0xC0, p, sizeof(p)) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    out[0] = p[0];
    out[1] = p[1] + 1.6f;
    out[2] = p[2];
    return true;
}
bool Blocked(const float* ear, const float* mouth, uint32_t self, uint32_t speaker) {
    if (!bot_sense::losReady) return false;
    float d[3] = {mouth[0] - ear[0], mouth[1] - ear[1], mouth[2] - ear[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0f) return false;
    // a little short of the mouth, so the speaker's own body is no wall
    const float k = (len - 0.4f) / len;
    alignas(16) float from[4] = {ear[0], ear[1], ear[2], 1.0f}, to[4] = {ear[0] + d[0] * k, ear[1] + d[1] * k, ear[2] + d[2] * k, 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    if (!bot_sense::CastRaw(from, to, self, fraction, hit)) { bot_sense::NoteLosFault(); return false; }
    return fraction < 0.98f && hit != speaker;
}
// The walk from the listener's feet to the speaker's: its length and its first turn at least 2.5 m away.
bool WalkAround(const float* earFeet, const float* mouthFeet, float& length, float* via) {
    coop_bot::Vec3 path[24];
    int count = 0;
    InterlockedIncrement(&walks);
    if (!bot_nav::Query(coop_bot::Vec3{earFeet[0], earFeet[1], earFeet[2]}, coop_bot::Vec3{mouthFeet[0], mouthFeet[1], mouthFeet[2]}, path, 24, count) || count < 2)
        return false;
    length = 0.0f;
    bool haveVia = false;
    for (int i = 1; i < count; ++i) {
        length += coop_bot::Distance(path[i - 1], path[i]);
        if (!haveVia && coop_bot::Distance(path[0], path[i]) >= 2.5f) {
            via[0] = path[i].x;
            via[1] = path[i].y + 1.6f;
            via[2] = path[i].z;
            haveVia = true;
        }
    }
    if (!haveVia) { via[0] = mouthFeet[0]; via[1] = mouthFeet[1] + 1.6f; via[2] = mouthFeet[2]; }
    InterlockedIncrement(&walksFound);
    return true;
}
// The game's world is right handed, X3DAudio left handed: x is mirrored on the way in.
X3DAUDIO_VECTOR Mirror(const float* v) { return X3DAUDIO_VECTOR{-v[0], v[1], v[2]}; }
void Place(Active& a, DWORD now) {
    float mouth[3]{};
    if (a.phantom) std::memcpy(mouth, a.last, sizeof(mouth));
    else if (HeadOf(a.key, mouth)) { std::memcpy(a.last, mouth, sizeof(mouth)); a.haveLast = true; }
    else if (a.haveLast) std::memcpy(mouth, a.last, sizeof(mouth));
    else return;
    coop_marks::Camera cam{};
    const bool haveCam = overlay::CameraNow(cam, 0, 0);
    const uint32_t self = LocalSlotZeroEntity();
    float ear[3]{};
    const bool own = !a.phantom && a.key == (machineIndex & 0xFF);
    if (self && Read(self + 0xC0, ear, sizeof(ear))) ear[1] += 1.6f;
    else if (haveCam) std::memcpy(ear, cam.pos, sizeof(ear));
    else std::memcpy(ear, mouth, sizeof(ear));
    const float dx = mouth[0] - ear[0], dy = mouth[1] - ear[1], dz = mouth[2] - ear[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    // walls: a cast now and then, the walk around them less often (a phantom is in the head)
    if (!own && !a.phantom && now - a.castAt >= 150) {
        a.castAt = now;
        const bool was = a.blocked;
        a.blocked = Blocked(ear, mouth, self, EntityOf(a.key));
        InterlockedIncrement(a.blocked ? &blockedCasts : &openCasts);
        if (a.blocked && (!was || now - a.walkAt >= 500)) {
            a.walkAt = now;
            const float earFeet[3] = {ear[0], ear[1] - 1.6f, ear[2]}, mouthFeet[3] = {mouth[0], mouth[1] - 1.6f, mouth[2]};
            a.haveVia = WalkAround(earFeet, mouthFeet, a.walk, a.via);
            if (!a.haveVia) a.walk = -1.0f;
        }
        if (!a.blocked) { a.haveVia = false; a.walk = -1.0f; }
    }
    // where it seems to come from, seen from the camera
    const float* from = a.blocked && a.haveVia ? a.via : mouth;
    float dir[3] = {from[0] - (haveCam ? cam.pos[0] : ear[0]), from[1] - (haveCam ? cam.pos[1] : ear[1]), from[2] - (haveCam ? cam.pos[2] : ear[2])};
    float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-3f) { dir[0] = haveCam ? cam.axis[2][0] : 0.0f; dir[1] = haveCam ? cam.axis[2][1] : 0.0f; dir[2] = haveCam ? cam.axis[2][2] : 1.0f; len = 1.0f; }
    for (float& v : dir) v /= len;
    const bool behind = haveCam && dir[0] * cam.axis[2][0] + dir[1] * cam.axis[2][1] + dir[2] * cam.axis[2][2] < -0.3f;
    const coop_voices::Hearing h = coop_voices::Hear(own ? 0.0f : distance, a.blocked, a.walk, behind && !own, nearM, farM);
    const float step = a.started ? 0.08f : 1.0f;
    a.gain = a.phantom ? a.phantomGain : coop_voices::Approach(a.gain, h.gain, step);
    a.cutoff = a.phantom ? coop_voices::kOpenCutoff : coop_voices::Approach(a.cutoff, h.cutoff, a.started ? 0.06f : 1.0f);
    float matrix[XAUDIO2_MAX_AUDIO_CHANNELS]{};
    if (calc && haveCam) {
        const float front[3] = {cam.axis[2][0], cam.axis[2][1], cam.axis[2][2]}, top[3] = {cam.axis[1][0], cam.axis[1][1], cam.axis[1][2]};
        X3DAUDIO_LISTENER listener{};
        listener.OrientFront = Mirror(front);
        listener.OrientTop = Mirror(top);
        X3DAUDIO_EMITTER emitter{};
        emitter.Position = Mirror(dir);   // a metre away in its direction: the distance is ours
        emitter.ChannelCount = 1;
        emitter.CurveDistanceScaler = 1.0f;
        emitter.DopplerScaler = 0.0f;
        X3DAUDIO_DSP_SETTINGS dsp{};
        dsp.SrcChannelCount = 1;
        dsp.DstChannelCount = outChannels;
        dsp.pMatrixCoefficients = matrix;
        calc(x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
    } else {
        for (unsigned c = 0; c < outChannels && c < 2; ++c) matrix[c] = 0.707f;
    }
    a.voice->SetOutputMatrix(master, 1, outChannels, matrix);
    a.voice->SetVolume(a.gain * (followGameVolume ? (a.sfx ? gameVolumes.sfx : gameVolumes.voice) : 1.0f));
    XAUDIO2_FILTER_PARAMETERS filter{LowPassFilter, a.cutoff, 1.0f};
    a.voice->SetFilterParameters(&filter);
}
bool Start(Active& a, DWORD now) {
    const coop_voices::Sound& s = a.load->sound;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = s.rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = s.rate * 2;
    if (FAILED(engine->CreateSourceVoice(&a.voice, &format, XAUDIO2_VOICE_USEFILTER, 4.0f))) {
        a.voice = nullptr;
        InterlockedIncrement(&voiceFailures);
        return false;
    }
    Place(a, now);
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(s.samples.size() * 2);
    buffer.pAudioData = reinterpret_cast<const BYTE*>(s.samples.data());
    if (FAILED(a.voice->SubmitSourceBuffer(&buffer)) || FAILED(a.voice->Start(0))) {
        Stop(a);
        InterlockedIncrement(&voiceFailures);
        return false;
    }
    a.started = true;
    a.startedAt = now;
    InterlockedIncrement(&played);
    if (a.phantom) {
        InterlockedIncrement(&phantomsPlayed);
        if (InterlockedIncrement(&lineLines) <= 200) gate::Event("voices: phantom %s (%s), %.0f%%", a.load->path.c_str(), a.who.c_str(), a.gain * 100.0f);
        return true;
    }
    if (InterlockedIncrement(&lineLines) <= 200)
        gate::Event("voices: %s %u (%s) says %s: %s, %.0f%%%s%s", coop_voices::IsBotKey(a.key) ? "bot" : "machine",
                    coop_voices::IsBotKey(a.key) ? a.key - coop_voices::kBotKey : a.key, packs[a.pack].folder.c_str(), coop_voices::EventName(a.event),
                    a.load->path.c_str() + (a.load->path.size() > folder.size() ? folder.size() + 1 : 0), a.gain * 100.0f, a.blocked ? ", behind a wall" : "",
                    a.blocked && a.haveVia ? ", heard around it" : "");
    return true;
}
uint32_t SubtitleColor(unsigned key) {
    static const uint32_t kColors[4] = {0xFFFFD27Au, 0xFF8FD4FFu, 0xFFB8F28Cu, 0xFFFF9ECFu};
    return kColors[coop_voices::OrderOf(key, Humans()) % 4];
}
// A phantom's subtitle may come in parts, one "ms|text" line each: the part due now.
void PartNow(const std::string& subtitle, DWORD elapsed, char* out, size_t size) {
    out[0] = 0;
    size_t at = 0;
    while (at < subtitle.size()) {
        size_t end = subtitle.find('\n', at);
        if (end == std::string::npos) end = subtitle.size();
        const std::string line = subtitle.substr(at, end - at);
        const size_t bar = line.find('|');
        const unsigned long from = bar == std::string::npos ? 0ul : std::strtoul(line.c_str(), nullptr, 10);
        if (from > elapsed && out[0]) break;
        std::snprintf(out, size, "%s", bar == std::string::npos ? line.c_str() : line.c_str() + bar + 1);
        at = end + 1;
    }
}
void ShowSubtitles(DWORD now) {
    if (!subtitles) return;
    overlay::Subtitles s{};
    for (const Active& a : active) {
        if (!a.started || a.subtitle.empty() || s.count >= 3) continue;
        overlay::SubtitleRow& r = s.rows[s.count++];
        std::snprintf(r.who, sizeof(r.who), "%s", a.phantom ? a.who.c_str() : packs[a.pack].name.c_str());
        if (a.phantom) PartNow(a.subtitle, now - a.startedAt, r.text, sizeof(r.text));
        else std::snprintf(r.text, sizeof(r.text), "%s", a.subtitle.c_str());
        r.color = a.phantom ? 0xFFB0E8F0u : SubtitleColor(a.key);
        r.until = now + 400;
    }
    if (s.count) overlay::SetSubtitles(s);
    else overlay::HideSubtitles();
}
// One asked line: rules, the speaker's pack, the line; then it waits for its sound.
void Handle(const Ask& ask, DWORD now) {
    const unsigned key = KeyOf(ask.entity);
    if (key > 0xFF) { InterlockedIncrement(&noSpeaker); return; }
    const int pack = PackOf(key);
    if (pack < 0) { InterlockedIncrement(&noPack); return; }
    if (ask.event == coop_voices::kGrab && ask.context) {
        if (lastGrab[key] == ask.context) { InterlockedIncrement(&skippedCooldown); return; }
        lastGrab[key] = ask.context;
    }
    const coop_voices::Rule& rule = rules[ask.event];
    DWORD& last = lastSpoke[key][ask.event];
    if (last && now - last < rule.cooldownMs) { InterlockedIncrement(&skippedCooldown); return; }
    const bool always = ask.event == coop_voices::kLowAmmo && _stricmp(ask.detail, "empty") == 0;
    if (rule.chance < 100 && !always && Roll() * 100.0f >= static_cast<float>(rule.chance)) { InterlockedIncrement(&skippedChance); return; }
    const auto* lines = coop_voices::Lines(packs[pack], ask.event == coop_voices::kTest && !coop_voices::Lines(packs[pack], ask.event, nullptr) ? coop_voices::kPain : ask.event,
                                           ask.detail);
    if (!lines) { InterlockedIncrement(&noLine); return; }
    // one line per speaker at a time; a more urgent one cuts in
    for (Active& a : active)
        if (a.key == key) {
            if (coop_voices::Priority(ask.event) <= coop_voices::Priority(a.event)) { InterlockedIncrement(&skippedBusy); return; }
            Stop(a);
            InterlockedIncrement(&interrupted);
        }
    active.erase(std::remove_if(active.begin(), active.end(), [](const Active& a) { return !a.voice && a.started; }), active.end());
    if (active.size() >= kMaxActive) { InterlockedIncrement(&skippedBusy); return; }
    size_t& previous = lastLine[key][ask.event];
    const size_t pick = coop_voices::Pick(lines->size(), lines->size() > 1 ? previous : SIZE_MAX, Roll());
    previous = pick;
    last = now;
    Active a;
    a.key = key;
    a.event = ask.event;
    a.pack = pack;
    a.subtitle = (*lines)[pick].subtitle;
    a.load = Request(folder + "\\" + packs[pack].folder + "\\" + (*lines)[pick].file, now);
    a.startedAt = now;   // the wait for the file counts from here
    active.push_back(std::move(a));
}
// The Dementia mutator: a sound only this machine hears, from `at` (world position), at `gain`
// of the voice volume; `who` and `subtitle` show as a subtitle line when given. Game thread.
bool PlayPhantom(const std::string& path, const float* at, float gain, const char* who, const char* subtitle, bool sfx = false) {
    InterlockedIncrement(&phantomsAsked);
    if (!installed || !engine || !inLevel || !at) { InterlockedIncrement(&phantomsRefused); return false; }
    active.erase(std::remove_if(active.begin(), active.end(), [](const Active& a) { return a.started && !a.voice; }), active.end());
    if (active.size() >= kMaxActive) { InterlockedIncrement(&phantomsRefused); return false; }
    const DWORD now = GetTickCount();
    Active a;
    a.phantom = true;
    a.phantomGain = gain < 0.0f ? 0.0f : gain > 2.0f ? 2.0f : gain;
    a.sfx = sfx;
    a.key = kPhantomKey;
    a.who = who ? who : "";
    a.subtitle = subtitle ? subtitle : "";
    std::memcpy(a.last, at, sizeof(a.last));
    a.haveLast = true;
    a.load = Request(path, now);
    a.startedAt = now;
    active.push_back(std::move(a));
    return true;
}
volatile LONG pausedTicks = 0;
uint32_t lastSim = 0;
DWORD simStillSince = 0;
void Pause(DWORD now) {
    const uint32_t sim = turret::SimMs();
    if (sim != lastSim) { lastSim = sim; simStillSince = 0; }
    else if (!simStillSince) simStillSince = Stamp(now);
    const bool paused = simStillSince && now - simStillSince > 250;
    if (paused == !engineRunning) return;
    if (paused) { engine->StopEngine(); engineRunning = false; InterlockedIncrement(&pausedTicks); }
    else if (SUCCEEDED(engine->StartEngine())) engineRunning = true;
}

// --- the wire -------------------------------------------------------------------------------------------
DWORD sentAt = 0;
volatile LONG packetsSent = 0, packetsIn = 0;
void Send(DWORD now) {
    if (!network || !peerReadiness.token || now - sentAt < 2000) return;
    sentAt = now;
    coop_voices::Packet p{};
    p.magic = coop_voices::kMagic;
    p.version = coop_voices::kVersion;
    p.token = peerReadiness.token;
    p.humans = static_cast<uint16_t>(Humans());
    p.entries[p.count].key = static_cast<uint8_t>(machineIndex);
    std::snprintf(p.entries[p.count].pack, sizeof(p.entries[0].pack), "%s", ownPick.c_str());
    ++p.count;
    if (IsHost())
        for (unsigned i = 0; i < kMaxBots && p.count < 5; ++i) {
            if (!botEntities[i]) continue;
            const int pack = PackOf(coop_voices::kBotKey + i);
            p.entries[p.count].key = static_cast<uint8_t>(coop_voices::kBotKey + i);
            std::snprintf(p.entries[p.count].pack, sizeof(p.entries[0].pack), "%s", pack >= 0 ? packs[pack].folder.c_str() : "");
            ++p.count;
        }
    if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&packetsSent);
}
// From the side channel's receive loop. True when it was ours.
bool OnPacket(const coop_voices::Packet& p, bool fromHost) {
    if (!coop_voices::Valid(p) || !KnownPeerToken(p.token)) return false;
    InterlockedIncrement(&packetsIn);
    if (fromHost && p.humans) hostHumans = p.humans;
    for (unsigned i = 0; i < p.count; ++i) {
        const unsigned key = p.entries[i].key;
        if (key == (machineIndex & 0xFF)) continue;
        char name[20]{};
        std::memcpy(name, p.entries[i].pack, sizeof(p.entries[i].pack));
        if (picks[key] != name) {
            picks[key] = name;
            gate::Event("voices: %s %u picked %s", coop_voices::IsBotKey(key) ? "bot" : "machine", coop_voices::IsBotKey(key) ? key - coop_voices::kBotKey : key,
                        name[0] ? name : "the default");
        }
    }
    return true;
}

// --- the game's sliders and the focus -------------------------------------------------------------------
std::string SettingsPath() {
    char local[MAX_PATH]{};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH)) return {};
    return std::string(local) + "\\EA Games\\Dead Space 2\\settings.txt";
}
// The game rewrites settings.txt when its options change; the time stamp tells when.
void ReadGameVolumes(DWORD now) {
    if (!followGameVolume || (settingsCheckedAt && now - settingsCheckedAt < 1000)) return;
    settingsCheckedAt = Stamp(now);
    const std::string path = SettingsPath();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return;
    const uint64_t written = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    if (written == settingsWriteTime) return;
    settingsWriteTime = written;
    std::string text;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
        char chunk[1024];
        size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0 && text.size() < 65536) text.append(chunk, got);
        std::fclose(f);
    }
    coop_voices::GameVolumes v;
    if (!coop_voices::ParseGameVolumes(text, v)) {
        InterlockedIncrement(&settingsFailures);
        gate::Event("voices: no Audio volumes in %s; full volume", path.c_str());
        return;
    }
    InterlockedIncrement(&settingsReads);
    if (v.voice != gameVolumes.voice || v.sfx != gameVolumes.sfx || v.found != gameVolumes.found)
        gate::Event("voices: the game's sliders voice=%.2f sfx=%.2f music=%.2f (settings.txt)", v.voice, v.sfx, v.music);
    gameVolumes = v;
}
void ApplyMaster(DWORD now) {
    if (!master) return;
    const unsigned dt = focusAt ? static_cast<unsigned>(std::min<DWORD>(now - focusAt, 200)) : 0u;
    focusAt = Stamp(now);
    const bool focused = !muteUnfocused || necro::GameWindowFocused();
    if (!focused) InterlockedIncrement(&unfocusedTicks);
    focusGain = coop_voices::FocusStep(focusGain, focused, dt, 150);
    const float want = static_cast<float>(volumePercent) / 100.0f * focusGain;
    if (std::fabs(want - masterNow) < 0.001f) return;
    masterNow = want;
    master->SetVolume(want);
}

// --- the game thread ---------------------------------------------------------------------------------
void Situations(DWORD now);   // below
void Tick(DWORD now) {
    if (!installed) return;
    Send(now);
    ReadGameVolumes(now);
    ApplyMaster(now);
    if (!inLevel) {
        if (!active.empty()) StopAll();
        AcquireSRWLockExclusive(&askLock);
        asks.clear();
        ReleaseSRWLockExclusive(&askLock);
        return;
    }
    Pause(now);
    Situations(now);
    std::vector<Ask> due;
    AcquireSRWLockExclusive(&askLock);
    for (size_t i = 0; i < asks.size();)
        if (static_cast<LONG>(now - asks[i].at) >= 0) { due.push_back(asks[i]); asks.erase(asks.begin() + static_cast<std::ptrdiff_t>(i)); }
        else ++i;
    ReleaseSRWLockExclusive(&askLock);
    for (const Ask& a : due) Handle(a, now);
    for (Active& a : active) {
        if (!a.started) {
            const LONG state = InterlockedCompareExchange(&a.load->state, 0, 0);
            if (state == 2) {
                if (InterlockedIncrement(&lineLines) <= 200) gate::Event("voices: %s NOT played: %s", a.load->path.c_str(), a.load->error.c_str());
                a.started = true;   // dropped below
                a.load.reset();
            } else if (state == 1) {
                if (!Start(a, now)) { a.started = true; a.load.reset(); }
            } else if (now - a.startedAt > 3000) {
                a.started = true;   // the disk never answered
                a.load.reset();
            }
            continue;
        }
        if (!a.voice) continue;
        XAUDIO2_VOICE_STATE s{};
        a.voice->GetState(&s, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (!s.BuffersQueued) { Stop(a); continue; }
        if (now - a.updateAt >= 30) {
            a.updateAt = now;
            Place(a, now);
        }
    }
    active.erase(std::remove_if(active.begin(), active.end(), [](const Active& a) { return a.started && !a.voice; }), active.end());
    ShowSubtitles(now);
}
// The debug menu: every character says its test line (or a pain line), one after the other.
unsigned TestAll() {
    std::vector<uint32_t> who;
    if (const uint32_t self = LocalSlotZeroEntity()) who.push_back(self);
    for (unsigned m = 0; m < 16; ++m)
        if (m != machineIndex)
            if (const uint32_t g = GhostForMachine(m)) who.push_back(g);
    for (unsigned i = 0; i < kMaxBots; ++i)
        if (const uint32_t b = botEntities[i] ? botEntities[i] : GhostForMachine(coop_voices::kBotKey + i)) who.push_back(b);
    for (size_t i = 0; i < who.size(); ++i) Say(who[i], coop_voices::kTest, nullptr, static_cast<DWORD>(1800 * i));
    return static_cast<unsigned>(who.size());
}

// --- Isaac's own voice ----------------------------------------------------------------------------------
constexpr uint32_t kPlayerSoundSlotRva = 0x1729574, kPlayerPlaySoundRva = 0x3D1610, kCctSiteRva = 0xBF6B1B, kReloadAnimRva = 0x474470;
constexpr uint32_t kPairedCoordinator = 0x2A8, kPairAttacker = 0x88, kPairDefender = 0x90;
using PlaySoundFn = void*(__thiscall*)(void*, uint32_t, void*);
PlaySoundFn originalPlaySound = nullptr;
bool slotOurs = false, cctHooked = false, reloadHooked = false;
safetyhook::MidHook cctHook;
safetyhook::InlineHook reloadHook;
std::vector<std::pair<uint32_t, coop_voices::Vocal>> vocals;   // sorted by hash
volatile LONG muted = 0, mutedKind[8]{}, mutedLocal = 0, mutedBot = 0, mutedGhost = 0, mutedOther = 0, grabsSeen = 0, grabbersUnknown = 0;
volatile LONG cctDeaths = 0, cctStomps = 0, cctShocks = 0, reloadsSeen = 0, eventsSent = 0, eventsIn = 0;
bool Muting() { return installed && muteIsaac; }
coop_voices::Vocal VocalOf(uint32_t hash) {
    size_t lo = 0, hi = vocals.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (vocals[mid].first == hash) return vocals[mid].second;
        if (vocals[mid].first < hash) lo = mid + 1;
        else hi = mid;
    }
    return coop_voices::kVoNone;
}
// The grabbing enemy's kind from its primary vtable (analysis/ds2_classmap_live_2026-09-17.json).
const char* KindOfVtable(uint32_t vtable) {
    struct Kind { uint32_t va; const char* name; };
    static const Kind kKinds[] = {
        {0x01B83948u, "slasher"}, {0x01B86DF0u, "pack"}, {0x01B6C400u, "leaper"}, {0x01B6CA40u, "lurker"}, {0x01B6D428u, "brute"},
        {0x01B6BE80u, "infector"}, {0x01B8CCA8u, "stalker"}, {0x01B89018u, "puker"}, {0x01B87970u, "tripod"}, {0x01B73CE8u, "tripod"},
        {0x01B6EAD8u, "hunter"}, {0x01B84410u, "divider"}, {0x01B84930u, "divider"}, {0x01B864F8u, "swarm"}, {0x01B733E8u, "tormentor"},
        {0x01B74B78u, "nicole"}, {0x01B42208u, "tentacle"},
    };
    for (const Kind& k : kKinds)
        if (vtable == base + (k.va - 0x400000u)) return k.name;
    return nullptr;
}
// Grabbed: the coordinator, and the attacker's kind when this Player is its defender.
uint32_t GrabOf(uint32_t player, const char*& kind) {
    kind = nullptr;
    uint32_t coord = 0, defender = 0, attacker = 0, vtable = 0;
    if (!Read(player + kPairedCoordinator, &coord, 4) || !coord) return 0;
    if (!Read(coord + kPairDefender, &defender, 4) || defender - 0x10 != player) return 0;
    if (Read(coord + kPairAttacker, &attacker, 4) && attacker > 0x10010 && Read(attacker - 0x10, &vtable, 4)) kind = KindOfVtable(vtable);
    if (!kind) InterlockedIncrement(&grabbersUnknown);
    return coord;
}
void NoteSpeaker(uint32_t player) {
    if (player == LocalSlotZeroEntity()) InterlockedIncrement(&mutedLocal);
    else if (IsBotEntity(player)) InterlockedIncrement(&mutedBot);
    else if (IsGhostPlayer(player)) InterlockedIncrement(&mutedGhost);
    else InterlockedIncrement(&mutedOther);
}
void* __fastcall OnPlayerSound(void* self, void*, uint32_t hash, void* opts) {
    if (Muting()) {
        const coop_voices::Vocal v = VocalOf(hash);
        if (v != coop_voices::kVoNone) {
            const uint32_t player = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
            InterlockedIncrement(&muted);
            InterlockedIncrement(&mutedKind[v & 7]);
            NoteSpeaker(player);
            const char* kind = nullptr;
            const uint32_t coord = GrabOf(player, kind);
            const int event = coop_voices::EventFor(v, coord != 0);
            if (event == coop_voices::kGrab) InterlockedIncrement(&grabsSeen);
            if (event >= 0) Say(player, event, kind, 0, coord);
            return nullptr;
        }
    }
    return originalPlaySound(self, hash, opts);
}
DWORD teamSaidAt[coop_voices::kEventCount]{};   // a partner's line of that situation (OnEventPacket)
// Deaths and reloads the partners do not see on their own: 'VOXE', 12 bytes.
constexpr uint32_t kEventMagic = 0x45584F56u;   // 'VOXE'
#pragma pack(push, 1)
struct EventPacket { uint32_t magic; uint8_t version, key, event, reserved; uint32_t token; };
#pragma pack(pop)
static_assert(sizeof(EventPacket) == 12, "voice event packet size");
void SendEvent(uint32_t entity, int event, unsigned detail = 0) {
    if (!network || !peerReadiness.token) return;
    if (necro::LocalRole() == 3) return;   // a necro watching a scene as Isaac is not there for the humans
    const unsigned key = KeyOf(entity);
    if (key > 0xFF || (key != (machineIndex & 0xFF) && !coop_voices::IsBotKey(key))) return;   // only what this machine owns
    EventPacket p{kEventMagic, 1, static_cast<uint8_t>(key), static_cast<uint8_t>(event), static_cast<uint8_t>(detail < coop_voices::kDetailCount ? detail : 0),
                  peerReadiness.token};
    if (SendToPeers(&p, sizeof(p)) > 0) InterlockedIncrement(&eventsSent);
}
bool OnEventPacket(const void* data, int size) {
    if (size != static_cast<int>(sizeof(EventPacket))) return false;
    EventPacket p{};
    std::memcpy(&p, data, sizeof(p));
    if (p.magic != kEventMagic || p.version != 1 || p.event >= coop_voices::kEventCount || !KnownPeerToken(p.token)) return false;
    InterlockedIncrement(&eventsIn);
    if (coop_voices::IsSituation(p.event)) teamSaidAt[p.event] = Stamp(GetTickCount());
    const char* detail = coop_voices::DetailName(p.reserved);
    if (p.key != (machineIndex & 0xFF))
        if (const uint32_t e = EntityOf(p.key)) Say(e, p.event, detail[0] ? detail : nullptr);
    return true;
}
bool IsPlayerEntity(uint32_t e) {
    uint32_t vt = 0;
    return e && Read(e, &vt, 4) && vt == base + placement::kPlayerPrimaryVtableRva;
}
__declspec(noinline) int CctKindSafe(const char* name) {
    __try {
        return coop_voices::CctKindOf(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return coop_voices::kCctNone;
    }
}
// HandleCCTEvent at 0x00FF6B1B: eax the event name, [esp+0x10] the component, [component+0x10] its owner.
void OnCct(safetyhook::Context& ctx) {
    if (!Muting() || !ctx.eax) return;
    uint32_t component = 0, owner = 0;
    if (!Read(static_cast<uint32_t>(ctx.esp) + 0x10, &component, 4) || !component || !Read(component + 0x10, &owner, 4) || !IsPlayerEntity(owner)) return;
    const int kind = CctKindSafe(reinterpret_cast<const char*>(static_cast<uintptr_t>(ctx.eax)));
    if (kind == coop_voices::kCctDeath) {
        InterlockedIncrement(&cctDeaths);
        Say(owner, coop_voices::kDeath, nullptr);
        SendEvent(owner, coop_voices::kDeath);
    } else if (kind == coop_voices::kCctStompStream) {
        InterlockedIncrement(&cctStomps);
        ctx.eax = 0;   // the original test eax,eax / jz skips the Lua call
    } else if (kind == coop_voices::kCctShock) {
        InterlockedIncrement(&cctShocks);
        Say(owner, coop_voices::kPain, nullptr);
        ctx.eax = 0;
    }
}
void __fastcall OnReloadAnim(void* sm, void*) {
    reloadHook.thiscall<void>(sm);
    uint32_t player = 0;
    if (!Read(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sm)) + 0x50, &player, 4) || !player) return;
    laserdiag::NoteReload(player);
    if (!Muting()) return;
    // only where the character is simulated: the partners hear it through 'VOXE'
    if (player != LocalSlotZeroEntity() && !IsBotEntity(player)) return;
    InterlockedIncrement(&reloadsSeen);
    Say(player, coop_voices::kReload, nullptr);
    SendEvent(player, coop_voices::kReload);
}
// --- situations ([Voices] Situations; analysis/VOICE_SITUATIONS_2026-10-16.md) ---------------------------
// Six lines the game has no vocal for, read on the game thread from what the plugin
// already knows: low health, low ammo, a new room, a big enemy in sight, three kills
// in a row and a necro player in sight. Only the own Isaac and the host's bots are
// judged here; the partners hear it through 'VOXE' with the detail in its spare byte.
bool situations = true, spareReady = false;
constexpr DWORD kTeamGapMs = 8000;   // a partner's line of the same situation keeps ours back
volatile LONG sitFired[coop_voices::kEventCount]{}, sitTeamHeld = 0, sitLines = 0;
volatile LONG lowHealthArmed = 0, lowHealthGrabWaits = 0, lowAmmoFaults = 0, areaCandidates = 0, areaNotNew = 0, areaWrongKind = 0, areaMoved = 0,
              areaPartnerThere = 0, bigSeen = 0, bigOutOfRange = 0, bigOutOfView = 0, bigBlocked = 0, bigKindGap = 0, bigNoLos = 0, killsCounted = 0,
              killsNotCounted = 0, killsNotOurs = 0, necroSeen = 0, necroBlocked = 0;
std::string sitMap;
void SaySituation(uint32_t speaker, int event, const char* detail, DWORD delayMs = 0) {
    const DWORD now = GetTickCount();
    if ((event == coop_voices::kNewArea || event == coop_voices::kBigEnemy || event == coop_voices::kNecroSpotted) && teamSaidAt[event] &&
        now - teamSaidAt[event] < kTeamGapMs) {
        InterlockedIncrement(&sitTeamHeld);
        return;
    }
    InterlockedIncrement(&sitFired[event]);
    if (InterlockedIncrement(&sitLines) <= 80)
        gate::Event("voices situation %s%s%s for %08X", coop_voices::EventName(event), detail && *detail ? " " : "", detail ? detail : "", speaker);
    Say(speaker, event, detail, delayMs);
    SendEvent(speaker, event, coop_voices::DetailCode(detail));
}
// Something visible from the camera; without a known camera yet only the cast decides.
bool InView(const float* p) {
    coop_marks::Camera cam{};
    if (!overlay::cameraLogged || !overlay::CameraNow(cam, 0, 0)) return true;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    coop_marks::ToView(cam, p, x, y, z);
    return z > 0.5f && std::fabs(x) <= z * cam.tanX && std::fabs(y) <= z * cam.tanY;
}
// A clear line from Isaac's eyes to that point; a hit on the target itself counts as clear.
bool ClearTo(uint32_t self, const float* selfFeet, const float* at, uint32_t target) {
    alignas(16) float from[4] = {selfFeet[0], selfFeet[1] + 1.6f, selfFeet[2], 1.0f}, to[4] = {at[0], at[1], at[2], 1.0f};
    float fraction = 1.0f;
    uint32_t hit = 0;
    if (!bot_sense::CastRaw(from, to, self, fraction, hit)) { bot_sense::NoteLosFault(); return false; }
    return fraction >= 0.98f || (hit && (hit == target || hit - 0x10 == target));
}
bool EntityAlive(uint32_t entity) {
    const uint32_t hc = hits::FindHealthComponent(entity);
    float health = 0.0f;
    return hc && Read(hc + hits::kHealthCurrent, &health, 4) && health > 0.0f;
}
bool HiddenEntity(uint32_t entity) {
    uint8_t hidden = 0;
    return !Read(entity + hits::kEntityHiddenCount, &hidden, 1) || hidden != 0;
}

// Below a quarter of the health once, again after it was above a half. The pain line of the
// hit usually plays, so the line waits 1.2 s; a grab lets it wait up to 10 s more.
struct HealthWatch { uint32_t body = 0; bool armed = true; DWORD at = 0, pendingAt = 0; };
HealthWatch healthWatch;
void LowHealthTick(uint32_t player, DWORD now) {
    if (now - healthWatch.at < 200) return;
    healthWatch.at = now;
    if (player != healthWatch.body) { healthWatch = HealthWatch{}; healthWatch.body = player; healthWatch.at = now; }
    const uint32_t hc = hits::FindHealthComponent(player);
    float current = 0.0f, most = 0.0f;
    if (!hc || !Read(hc + hits::kHealthCurrent, &current, 4) || !Read(hc + hits::kHealthMax, &most, 4) || !(most > 0.0f) || !std::isfinite(current)) return;
    if (current <= 0.0f) { healthWatch.armed = false; healthWatch.pendingAt = 0; return; }   // the death has its own line
    const float ratio = current / most;
    if (ratio > 0.5f) { healthWatch.armed = true; healthWatch.pendingAt = 0; return; }
    if (healthWatch.armed && ratio < 0.25f) {
        healthWatch.armed = false;
        healthWatch.pendingAt = Stamp(now + 1200);
        InterlockedIncrement(&lowHealthArmed);
    }
    if (!healthWatch.pendingAt || static_cast<LONG>(now - healthWatch.pendingAt) < 0) return;
    const char* kind = nullptr;
    if (GrabOf(player, kind)) {
        if (now - healthWatch.pendingAt > 10000) healthWatch.pendingAt = 0;
        InterlockedIncrement(&lowHealthGrabWaits);
        return;
    }
    healthWatch.pendingAt = 0;
    SaySituation(player, coop_voices::kLowHealth, nullptr);
}

// The gun in hand: low when clip and spare hold one magazine or less ("empty" at none),
// again once they hold two. Per ammo type; a switched gun counts after 1 s.
struct AmmoWatch { uint32_t gun = 0; DWORD gunSince = 0, at = 0; std::map<uint32_t, bool> armed; };
AmmoWatch ammoWatch;
void LowAmmoTick(uint32_t player, DWORD now) {
    if (!spareReady || now - ammoWatch.at < 250) return;
    ammoWatch.at = now;
    uint32_t ref = 0;
    const uint32_t gun = Read(player + bot::kActiveItem, &ref, 4) && ref > 0x10010 ? ref - 0x10 : 0;
    if (gun != ammoWatch.gun) { ammoWatch.gun = gun; ammoWatch.gunSince = now; return; }
    if (!gun || now - ammoWatch.gunSince < 1000) return;
    const coop_bot::Gun kind = bot::GunKind(gun);
    if (kind == coop_bot::Gun::None || kind == coop_bot::Gun::Other) return;
    int clip = 0, capacity = 0;
    uint32_t attrs = 0, type = 0;
    if (!Read(gun + 0x39C, &clip, 4) || !Read(gun + 0x3E4, &capacity, 4) || capacity <= 0 || capacity > 1000 || clip < 0 || clip > 1000 || !Read(gun + 0xC, &attrs, 4) ||
        !attrs || !Read(attrs + 0x300, &type, 4) || !type)
        return;
    const int spare = bot::SpareRaw(player, type);
    if (spare < 0) { InterlockedIncrement(&lowAmmoFaults); return; }
    bool& armed = ammoWatch.armed.emplace(type, true).first->second;
    const int total = clip + spare;
    if (total >= 2 * capacity) { armed = true; return; }
    if (!armed || total > capacity) return;
    armed = false;
    SaySituation(player, coop_voices::kLowAmmo, total == 0 ? "empty" : nullptr);
}

// A stream set nobody completed before on this map, asked for by a door, a lift, the
// script or a follow, still the room 2 s later; a partner standing in it was there first.
struct AreaWatch { uint32_t last[4]{}, candidate[4]{}; DWORD candidateAt = 0; std::string map; std::vector<std::array<uint32_t, 4>> seen; };
AreaWatch areaWatch;
bool AreaSeen(const uint32_t* guid) {
    for (const auto& g : areaWatch.seen) if (std::memcmp(g.data(), guid, 16) == 0) return true;
    std::array<uint32_t, 4> g{};
    std::memcpy(g.data(), guid, 16);
    if (areaWatch.seen.size() < 512) areaWatch.seen.push_back(g);
    return false;
}
void NewAreaTick(uint32_t player, DWORD now) {
    if (mpmap::InVisit() || arena::RunningNow() || !streams::AreaValid()) return;
    if (_stricmp(streams::areaMap, areaWatch.map.c_str()) != 0) {
        // a new map: its first room is where the level put us, no line
        areaWatch = AreaWatch{};
        areaWatch.map = streams::areaMap;
        std::memcpy(areaWatch.last, streams::area, 16);
        AreaSeen(streams::area);
        return;
    }
    if (std::memcmp(streams::area, areaWatch.last, 16) != 0) {
        std::memcpy(areaWatch.last, streams::area, 16);
        const uint32_t kind = streams::areaKind;
        if (AreaSeen(streams::area)) InterlockedIncrement(&areaNotNew);
        else if ((kind == streams::kKindDoor || kind == streams::kKindLift || kind == streams::kKindScript || kind == streams::kKindFollow) && !streams::SceneActivation(now)) {
            std::memcpy(areaWatch.candidate, streams::area, 16);
            areaWatch.candidateAt = Stamp(now);
            InterlockedIncrement(&areaCandidates);
        } else {
            InterlockedIncrement(&areaWrongKind);
        }
    }
    if (!areaWatch.candidateAt || now - areaWatch.candidateAt < 2000) return;
    areaWatch.candidateAt = 0;
    if (std::memcmp(areaWatch.candidate, streams::area, 16) != 0) { InterlockedIncrement(&areaMoved); return; }
    for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
        if (peerTable.peers[s].used && std::memcmp(streams::peerAreas[s].area, areaWatch.candidate, 16) == 0) { InterlockedIncrement(&areaPartnerThere); return; }
    SaySituation(player, coop_voices::kNewArea, coop_voices::MapKey(areaWatch.map.c_str()).c_str());
}

// A boss or big enemy within 25 m, on the screen and in clear sight: once per enemy, once
// a minute per kind. The NetReflectable chain holds every NPC, a client's ghosts included.
struct BigWatch { DWORD at = 0; std::vector<uint32_t> told; DWORD kindAt[coop_voices::kDetailCount]{}; bool firstLogged[coop_voices::kDetailCount]{}; };
BigWatch bigWatch;
const char* BigKind(uint32_t vtable) {
    if (vtable == base + 0x1789F90u) return "guardian";
    const char* kind = KindOfVtable(vtable);
    if (!kind) return nullptr;
    for (const char* k : {"brute", "stalker", "tripod", "hunter", "tormentor", "nicole"})
        if (std::strcmp(kind, k) == 0) return kind;
    return nullptr;
}
void BigEnemyTick(uint32_t player, DWORD now) {
    if (now - bigWatch.at < 500) return;
    bigWatch.at = now;
    float self[3]{};
    if (!Read(player + 0xC0, self, sizeof(self))) return;
    uint32_t node = 0;
    if (!Read(base + 0x1C5B744, &node, 4)) return;
    unsigned casts = 0;
    for (unsigned guard = 0; node && guard < 1024; ++guard) {
        const uint32_t entity = node - 0x5C;
        uint32_t vtable = 0, next = 0;
        if (!Read(node + 4, &next, 4)) break;
        const char* kind = node > 0x5C && Read(entity, &vtable, 4) ? BigKind(vtable) : nullptr;
        node = next;
        if (!kind) continue;
        const unsigned code = coop_voices::DetailCode(kind);
        if (!bigWatch.firstLogged[code]) {
            bigWatch.firstLogged[code] = true;
            gate::Event("voices situation: first %s on the chain, %08X (vtable %08X)", kind, entity, vtable);
        }
        if (std::find(bigWatch.told.begin(), bigWatch.told.end(), entity) != bigWatch.told.end() || HiddenEntity(entity) || !EntityAlive(entity)) continue;
        float pos[3]{};
        if (!Read(entity + 0xC0, pos, sizeof(pos)) || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) continue;
        InterlockedIncrement(&bigSeen);
        const float dx = pos[0] - self[0], dy = pos[1] - self[1], dz = pos[2] - self[2];
        if (dx * dx + dy * dy + dz * dz > 25.0f * 25.0f) { InterlockedIncrement(&bigOutOfRange); continue; }
        if (bigWatch.kindAt[code] && now - bigWatch.kindAt[code] < 60000) { InterlockedIncrement(&bigKindGap); continue; }
        const bool tall = code == coop_voices::DetailCode("tripod") || code == coop_voices::DetailCode("tormentor") || code == coop_voices::DetailCode("nicole");
        const float chest[3] = {pos[0], pos[1] + (tall ? 1.5f : 1.2f), pos[2]};
        if (!InView(chest)) { InterlockedIncrement(&bigOutOfView); continue; }
        if (!bot_sense::losReady) { InterlockedIncrement(&bigNoLos); return; }
        if (casts++ >= 2) return;
        if (!ClearTo(player, self, chest, entity)) { InterlockedIncrement(&bigBlocked); continue; }
        if (bigWatch.told.size() < 256) bigWatch.told.push_back(entity);
        bigWatch.kindAt[code] = now;
        SaySituation(player, coop_voices::kBigEnemy, kind);
        return;
    }
}

// Three different enemies in 4 s by the same hand (the own Isaac or a bot of the host). From
// the health thunk; the host never knows who did a client's kills, so each machine counts its own.
struct KillRing { uint32_t victim[4]; DWORD at[4]; unsigned count; };
KillRing rings[1 + kMaxBots]{};
void OnHit(const void* damage, uint32_t component, float before, float after) {
    if (!installed || !situations || !inLevel || !(before > 0.0f) || after > 0.0f) return;
    uint32_t victim = 0, vtable = 0;
    if (!Read(component + hits::kComponentOwner, &victim, 4) || !victim || !Read(victim, &vtable, 4) || phantom::Is(victim)) return;
    const coop_bot::EnemyKind kind = bot_sense::KindOf(vtable);
    using K = coop_bot::EnemyKind;
    const bool counts = IsNecroEntity(victim) ||
                        (kind != K::Unknown && kind != K::Swarm && kind != K::DividerPart && kind != K::GuardianTentacle && kind != K::Nest && kind != K::Hunter);
    if (!counts) { InterlockedIncrement(&killsNotCounted); return; }
    uint32_t killer = 0;
    unsigned ring = 0;
    const uint32_t human = bot::HumanBehind(damage);
    if (human && human == LocalCampaignPlayer() && necro::LocalRole() == 0) {
        killer = human;
    } else if (const uint32_t b = bot::BotBehind(damage)) {
        for (unsigned i = 0; i < kMaxBots; ++i) if (botEntities[i] == b) { killer = b; ring = 1 + i; }
    }
    if (!killer) { InterlockedIncrement(&killsNotOurs); return; }
    InterlockedIncrement(&killsCounted);
    const DWORD now = GetTickCount();
    KillRing& r = rings[ring];
    unsigned kept = 0;
    for (unsigned i = 0; i < r.count; ++i)
        if (now - r.at[i] <= 4000) { r.victim[kept] = r.victim[i]; r.at[kept++] = r.at[i]; }
    r.count = kept;
    for (unsigned i = 0; i < r.count; ++i) if (r.victim[i] == victim) return;
    if (r.count < 4) { r.victim[r.count] = victim; r.at[r.count++] = now; }
    if (r.count < 3 || turret::CutsceneNow()) return;
    r.count = 0;
    SaySituation(killer, coop_voices::kMultiKill, nullptr);
}

// A partner's living necro in clear sight within 20 m (story invasion): once a life,
// again after 20 s out of sight.
struct NecroSeen { uint32_t entity = 0; DWORD lastSeen = 0; bool told = false; };
NecroSeen necroSeenBy[4]{};
DWORD necroScanAt = 0;
void NecroSpottedTick(uint32_t player, DWORD now) {
    if (now - necroScanAt < 250) return;
    necroScanAt = now;
    if (!necro::invasion || play::VersusMode() || !play::StoryCoop()) return;
    uint32_t ghosts[4]{};
    const unsigned n = marks::NecroGhosts(ghosts, 4);
    if (!n) return;
    float self[3]{};
    if (!Read(player + 0xC0, self, sizeof(self))) return;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t g = ghosts[i], machine = MachineOf(g);
        if (machine > 255 || roles::WatchingMachine(machine, now)) continue;
        bool living = false;
        for (unsigned s = 0; s < coop_peers::kMaxPeers; ++s)
            if (peerTable.peers[s].used && peerTable.peers[s].machine == machine && roles::RoleOf(s, now) == 1) living = true;
        if (!living || HiddenEntity(g) || !EntityAlive(g)) continue;
        float feet[3]{};
        if (!Read(g + placement::kPlayerPosition, feet, sizeof(feet)) || !std::isfinite(feet[0]) || !std::isfinite(feet[1]) || !std::isfinite(feet[2])) continue;
        const float head[3] = {feet[0], feet[1] + 1.6f, feet[2]};
        const float dx = head[0] - self[0], dy = head[1] - self[1], dz = head[2] - self[2];
        if (dx * dx + dy * dy + dz * dz > 20.0f * 20.0f || !InView(head) || !bot_sense::losReady) continue;
        if (!ClearTo(player, self, head, g)) { InterlockedIncrement(&necroBlocked); continue; }
        InterlockedIncrement(&necroSeen);
        NecroSeen* slot = nullptr;
        for (NecroSeen& e : necroSeenBy) if (e.entity == g) slot = &e;
        if (!slot)
            for (NecroSeen& e : necroSeenBy)
                if (!slot || e.lastSeen < slot->lastSeen) slot = &e;
        if (slot->entity != g) *slot = NecroSeen{g, 0, false};
        if (slot->told && now - slot->lastSeen > 20000) slot->told = false;
        slot->lastSeen = now;
        if (slot->told) continue;
        slot->told = true;
        SaySituation(player, coop_voices::kNecroSpotted, nullptr);
        return;
    }
}

void Situations(DWORD now) {
    if (!situations || !turret::WorldReady() || turret::CutsceneNow()) return;
    if (_stricmp(level::reportMap, sitMap.c_str()) != 0) {
        sitMap = level::reportMap;
        bigWatch = BigWatch{};
        ammoWatch.armed.clear();
        for (NecroSeen& e : necroSeenBy) e = NecroSeen{};
    }
    const uint32_t player = LocalCampaignPlayer();
    if (!player || necro::LocalRole() != 0 || !checkpoint::LocalIsaacAlive()) return;
    LowHealthTick(player, now);
    LowAmmoTick(player, now);
    NewAreaTick(player, now);
    BigEnemyTick(player, now);
    NecroSpottedTick(player, now);
}
void InstallMute(FILE* log) {
    for (const auto& v : coop_voices::kVocals) vocals.push_back({v.hash, v.kind});
    std::sort(vocals.begin(), vocals.end());
    if (!muteIsaac) { std::fprintf(log, "voices: Isaac keeps his own voice ([Voices] MuteIsaac=0)\n"); return; }
    // sub esp,30h / push ebx / mov ebx,[esp+38h] / xor eax,eax / push edi / mov edi,ecx / test ebx,ebx / j..
    static const unsigned char kPlaySound[] = {0x83, 0xEC, 0x30, 0x53, 0x8B, 0x5C, 0x24, 0x38, 0x33, 0xC0, 0x57, 0x8B, 0xF9, 0x85, 0xDB, 0x0F};
    unsigned char head[sizeof(kPlaySound)]{};
    uint32_t slot = 0;
    const bool slotOk = Read(base + kPlayerSoundSlotRva, &slot, 4) && slot == base + kPlayerPlaySoundRva && Read(base + kPlayerPlaySoundRva, head, sizeof(head)) &&
                        std::memcmp(head, kPlaySound, sizeof(head)) == 0;
    if (slotOk) {
        originalPlaySound = reinterpret_cast<PlaySoundFn>(static_cast<uintptr_t>(slot));
        // The vtable sits in read-only image data (16.10.: WriteBytes alone failed): lift the protection for the dword.
        const uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnPlayerSound));
        void* at = reinterpret_cast<void*>(static_cast<uintptr_t>(base + kPlayerSoundSlotRva));
        DWORD old = 0, ignored = 0;
        if (VirtualProtect(at, 4, PAGE_READWRITE, &old) || VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) {
            slotOurs = WriteBytes(base + kPlayerSoundSlotRva, &ours, 4);
            VirtualProtect(at, 4, old, &ignored);
        }
    }
    std::fprintf(log, "voices: Player PlaySound slot %08X %s (held %08X)\n", static_cast<unsigned>(base + kPlayerSoundSlotRva),
                 slotOurs ? "ours: Isaac's vocals are muted and reported" : slotOk ? "WRITE FAILED" : "NOT the expected function: Isaac keeps his voice", slot);
    // mov eax,[eax+14h] / test eax,eax / jz / cmp byte [eax],0
    static const unsigned char kCct[] = {0x85, 0xC0, 0x0F, 0x84, 0xAE, 0x00, 0x00, 0x00, 0x80, 0x38, 0x00};
    cctHooked = hook_site::InstallMid(log, "voices CCT event (deaths, stomp stream, shock scream)", base + kCctSiteRva, kCct, sizeof(kCct), OnCct, cctHook);
    // push ecx / push ebx / push esi / mov esi,ecx / mov eax,[esi+50h] / mov ebx,[eax+468h]
    static const unsigned char kReload[] = {0x51, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x50, 0x8B, 0x98, 0x68, 0x04, 0x00, 0x00};
    reloadHooked = hook_site::Install(log, "voices reload start", base + kReloadAnimRva, kReload, sizeof(kReload), reinterpret_cast<void*>(OnReloadAnim), reloadHook);
}

// --- setup -----------------------------------------------------------------------------------------------
void ScanPacks(FILE* log) {
    WIN32_FIND_DATAA f{};
    const HANDLE h = FindFirstFileA((folder + "\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == '.' || f.cFileName[0] == '_') continue;   // _template
        const std::string dir = folder + "\\" + f.cFileName;
        FILE* in = nullptr;
        if (fopen_s(&in, (dir + "\\pack.ini").c_str(), "rb") != 0 || !in) continue;
        std::string text;
        char chunk[4096];
        size_t got = 0;
        while (text.size() < (1u << 20) && (got = std::fread(chunk, 1, sizeof(chunk), in)) > 0) text.append(chunk, got);
        std::fclose(in);
        coop_voices::Pack p;
        p.folder = f.cFileName;
        coop_voices::ParsePack(text, p);
        unsigned missing = 0;
        std::string events;
        for (const auto& kv : p.lines) {
            events += (events.empty() ? "" : " ") + kv.first + "=" + std::to_string(kv.second.size());
            for (const auto& l : kv.second)
                if (GetFileAttributesA((dir + "\\" + l.file).c_str()) == INVALID_FILE_ATTRIBUTES && ++missing <= 5)
                    std::fprintf(log, "voices pack %s: %s is missing\n", p.folder.c_str(), l.file.c_str());
        }
        std::fprintf(log, "voices pack %s (\"%s\"): %zu lines, %u missing; %s\n", p.folder.c_str(), p.name.c_str(), p.Count(), missing, events.c_str());
        packs.push_back(std::move(p));
    } while (FindNextFileA(h, &f));
    FindClose(h);
    std::sort(packs.begin(), packs.end(), [](const coop_voices::Pack& a, const coop_voices::Pack& b) { return coop_voices::Lower(a.folder) < coop_voices::Lower(b.folder); });
}
bool OpenDevice(FILE* log) {
    dll = LoadLibraryW(L"xaudio2_9.dll");
    if (!dll) { std::fprintf(log, "voices: xaudio2_9.dll not found (error %lu): no voices\n", GetLastError()); return false; }
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "XAudio2Create"));
    const auto init = reinterpret_cast<X3DInitFn>(GetProcAddress(dll, "X3DAudioInitialize"));
    calc = reinterpret_cast<X3DCalcFn>(GetProcAddress(dll, "X3DAudioCalculate"));
    if (!create) { std::fprintf(log, "voices: XAudio2Create missing in xaudio2_9.dll: no voices\n"); return false; }
    HRESULT hr = create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !engine) { std::fprintf(log, "voices: XAudio2Create failed (%08lX): no voices\n", static_cast<unsigned long>(hr)); engine = nullptr; return false; }
    hr = engine->CreateMasteringVoice(&master);
    if (FAILED(hr) || !master) {
        std::fprintf(log, "voices: no output device (%08lX): no voices\n", static_cast<unsigned long>(hr));
        engine->Release();
        engine = nullptr;
        master = nullptr;
        return false;
    }
    XAUDIO2_VOICE_DETAILS d{};
    master->GetVoiceDetails(&d);
    outChannels = d.InputChannels ? d.InputChannels : 2;
    if (outChannels > XAUDIO2_MAX_AUDIO_CHANNELS) outChannels = XAUDIO2_MAX_AUDIO_CHANNELS;
    master->GetChannelMask(&channelMask);
    master->SetVolume(volumePercent / 100.0f);
    if (!init || !calc || FAILED(init(channelMask ? channelMask : SPEAKER_STEREO, X3DAUDIO_SPEED_OF_SOUND, x3d))) {
        calc = nullptr;
        std::fprintf(log, "voices: X3DAudio missing: voices play without direction\n");
    }
    std::fprintf(log, "voices: XAudio2 ready, %u output channels (mask %08lX, %lu Hz), 3D %s\n", outChannels, channelMask, d.InputSampleRate, calc ? "on" : "OFF");
    return true;
}
void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Voices", "Enabled", 1, ini.c_str()) != 0;
    subtitles = GetPrivateProfileIntA("Voices", "Subtitles", 1, ini.c_str()) != 0;
    muteIsaac = GetPrivateProfileIntA("Voices", "MuteIsaac", 1, ini.c_str()) != 0;
    situations = GetPrivateProfileIntA("Voices", "Situations", 1, ini.c_str()) != 0;
    volumePercent = GetPrivateProfileIntA("Voices", "Volume", 100, ini.c_str());
    followGameVolume = GetPrivateProfileIntA("Voices", "FollowGameVolume", 1, ini.c_str()) != 0;
    muteUnfocused = GetPrivateProfileIntA("Voices", "MuteUnfocused", 1, ini.c_str()) != 0;
    if (volumePercent < 0) volumePercent = 0;
    if (volumePercent > 200) volumePercent = 200;
    const int carry = GetPrivateProfileIntA("Voices", "MaxDistance", 60, ini.c_str());
    farM = static_cast<float>(carry < 10 ? 10 : carry > 300 ? 300 : carry);
    char text[256]{};
    GetPrivateProfileStringA("Voices", "Pack", "", text, sizeof(text), ini.c_str());
    ownPick = coop_voices::Trim(text);
    std::string rulesText;
    for (int e = 0; e < coop_voices::kEventCount; ++e) {
        rules[e] = coop_voices::DefaultRule(e);
        if (e == coop_voices::kTest) continue;
        char key[16];
        std::snprintf(key, sizeof(key), "%c%s", coop_voices::EventName(e)[0] - 32, coop_voices::EventName(e) + 1);
        GetPrivateProfileStringA("Voices", key, "", text, sizeof(text), ini.c_str());
        if (text[0] && !coop_voices::ParseRule(text, rules[e])) std::fprintf(log, "voices: [Voices] %s=%s is not \"chance,cooldown_ms\"; kept %u,%u\n", key, text, rules[e].chance, rules[e].cooldownMs);
        rulesText += std::string(rulesText.empty() ? "" : " ") + coop_voices::EventName(e) + "=" + std::to_string(rules[e].chance) + "%/" + std::to_string(rules[e].cooldownMs) + "ms";
    }
    for (int& p : packSeen) p = -2;
    for (auto& row : lastLine)
        for (size_t& l : row) l = SIZE_MAX;
    if (!enabled) { std::fprintf(log, "voices off ([Voices] Enabled=0)\n"); return; }
    const size_t slash = ini.find_last_of("\\/");
    folder = (slash == std::string::npos ? std::string(".") : ini.substr(0, slash)) + "\\voices";
    ScanPacks(log);
    GetPrivateProfileStringA("Voices", "Defaults", "", text, sizeof(text), ini.c_str());
    defaults = coop_voices::ParseDefaults(packs, text);
    std::string order;
    for (int d : defaults) order += (order.empty() ? "" : ", ") + packs[d].folder;
    std::fprintf(log, "voices: %zu pack(s) in %s; defaults in join order: %s; own pick: %s%s; volume %d%%, carries %.0f m, subtitles %d; %s\n", packs.size(),
                 folder.c_str(), order.empty() ? "none" : order.c_str(), ownPick.empty() ? "none (the default by join order)" : ownPick.c_str(),
                 !ownPick.empty() && coop_voices::FindPack(packs, ownPick) < 0 ? " (NOT FOUND here, the default plays)" : "", volumePercent, farM, subtitles ? 1 : 0,
                 rulesText.c_str());
    if (packs.empty()) { std::fprintf(log, "voices: no pack with a pack.ini in %s: nothing to say\n", folder.c_str()); return; }
    if (!OpenDevice(log)) return;
    wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    const HANDLE thread = wake ? CreateThread(nullptr, 0, LoadThread, nullptr, 0, nullptr) : nullptr;
    if (!thread) { std::fprintf(log, "voices: no loader thread: no voices\n"); return; }
    CloseHandle(thread);
    rng ^= GetTickCount();
    installed = true;
    InstallMute(log);
    if (situations) {
        // push ecx / push ebx / push ebp / mov ebp,[ecx+0BD8h] / push esi / push edi / lea edi,[ecx+0BD0h]
        static const unsigned char kSpare[] = {0x51, 0x53, 0x55, 0x8B, 0xA9, 0xD8, 0x0B, 0x00, 0x00, 0x56, 0x57, 0x8D, 0xB9, 0xD0, 0x0B, 0x00};
        unsigned char site[sizeof(kSpare)]{};
        spareReady = Read(base + bot::kCountSpareAmmoRva, site, sizeof(site)) && std::memcmp(site, kSpare, sizeof(kSpare)) == 0;
        if (!bot_sense::losVerified) bot_sense::InstallLineOfSight(log);   // big_enemy and necro_spotted need a clear line
    }
    std::fprintf(log, "voices situations %s: low_health low_ammo%s new_area big_enemy%s multi_kill (needs the health hooks) necro_spotted%s\n",
                 situations ? "on" : "off ([Voices] Situations=0)", spareReady ? "" : " (OFF: CountSpareAmmo bytes differ)",
                 bot_sense::losReady ? "" : " (no line of sight)", bot_sense::losReady ? "" : " (no line of sight)");
}
// The game's listeners for "the host hears almost nothing, only the pause menu" (19.10.):
// AMicrophoneManager [0x020467E4] (+0x1C mics, +0x1D active, mic pointers at +4; a mic has
// its position at +0x40, scene mask +0x88, flags +0x8C), the active camera
// (CameraManager [0x0202131C], +0x164 count, blender [+0x10] position at +0) and our Isaac.
// One line when the picture changes, else once a minute.
constexpr uint32_t kMicManagerRva = 0x1C467E4, kCameraManagerRva = 0x1C2131C;
int DistanceBucket(float d) { return d < 2.0f ? 0 : d < 10.0f ? 1 : d < 50.0f ? 2 : 3; }
void MicDiagnostics(FILE* log) {
    uint32_t mm = 0, cm = 0, count = 0, blender = 0;
    if (!Read(base + kMicManagerRva, &mm, 4) || !mm) return;
    uint8_t head[0x20]{};
    if (!Read(mm, head, sizeof(head))) return;
    float cam[3]{}, isaac[3]{};
    const bool haveCam = Read(base + kCameraManagerRva, &cm, 4) && cm && Read(cm + 0x164, &count, 4) && count && Read(cm + 0x10, &blender, 4) && blender &&
                         Read(blender, cam, 12);
    const uint32_t player = LocalCampaignPlayer();
    const bool haveIsaac = player && Read(player + 0xC0, isaac, 12);
    char text[600];
    int n = std::snprintf(text, sizeof(text), "voices listener mics=%u active=%u on=%u camera=%s(%.1f %.1f %.1f) cameras=%u isaac=%s(%.1f %.1f %.1f)", head[0x1C], head[0x1D],
                          head[0x18], haveCam ? "" : "none ", cam[0], cam[1], cam[2], count, haveIsaac ? "" : "none ", isaac[0], isaac[1], isaac[2]);
    uint32_t signature = head[0x1C] | head[0x1D] << 4 | head[0x18] << 8;
    for (unsigned i = 0; i < 4 && i < head[0x1C] && n > 0 && n < static_cast<int>(sizeof(text)); ++i) {
        uint32_t mic = 0, mask = 0, flags = 0;
        float pos[3]{};
        std::memcpy(&mic, head + 4 + i * 4, 4);
        if (!mic || !Read(mic + 0x40, pos, 12) || !Read(mic + 0x88, &mask, 4) || !Read(mic + 0x8C, &flags, 4)) continue;
        const float dx = pos[0] - cam[0], dy = pos[1] - cam[1], dz = pos[2] - cam[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        signature = signature * 31 + mask * 7 + flags * 13 + static_cast<uint32_t>(DistanceBucket(d));
        n += std::snprintf(text + n, sizeof(text) - n, " | mic%u (%.1f %.1f %.1f) to camera %.1f m mask %08X flags %08X", i, pos[0], pos[1], pos[2],
                           haveCam ? d : -1.0f, mask, flags);
    }
    static uint32_t lastSignature = 0;
    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    if (signature == lastSignature && now - lastAt < 60000) return;
    lastSignature = signature;
    lastAt = now;
    std::fprintf(log, "%s\n", text);
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    MicDiagnostics(log);
    static LONG lastOutput = -1;
    const LONG outputSum = settingsReads + settingsFailures * 3 + (unfocusedTicks ? 1 : 0) * 7 + static_cast<LONG>(focusGain * 10.0f) * 11;
    if (outputSum != lastOutput) {
        lastOutput = outputSum;
        std::fprintf(log, "voices output follow_game=%d voice=%.2f sfx=%.2f reads=%ld failures=%ld | mute_unfocused=%d focus_gain=%.2f unfocused_ticks=%ld master=%.2f\n",
                     followGameVolume ? 1 : 0, gameVolumes.voice, gameVolumes.sfx, settingsReads, settingsFailures, muteUnfocused ? 1 : 0, focusGain, unfocusedTicks,
                     masterNow);
    }
    static LONG lastPhantoms = -1;
    const LONG phantomSum = phantomsAsked + phantomsPlayed * 7 + phantomsRefused * 13;
    if (phantomSum != lastPhantoms) {
        lastPhantoms = phantomSum;
        std::fprintf(log, "voices phantoms asked=%ld played=%ld refused=%ld\n", phantomsAsked, phantomsPlayed, phantomsRefused);
    }
    static LONG last = -1;
    const LONG sum = asked + played * 3 + loadsFailed * 5 + packetsIn * 7 + voiceFailures * 11 + noLine * 13 + skippedBusy * 17 + muted * 19 + eventsIn * 23 +
                     (killsCounted + bigSeen / 10 + areaCandidates + necroSeen / 10 + sitTeamHeld) * 29;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "voices asked=%ld dropped=%ld played=%ld skipped chance=%ld cooldown=%ld busy=%ld interrupted=%ld no_pack=%ld no_line=%ld no_speaker=%ld "
                      "voice_failures=%ld loads ok=%ld failed=%ld cache_drops=%ld casts open=%ld blocked=%ld walks=%ld found=%ld pauses=%ld packets sent=%ld in=%ld "
                      "humans=%u active=%zu\n",
                 asked, askDropped, played, skippedChance, skippedCooldown, skippedBusy, interrupted, noPack, noLine, noSpeaker, voiceFailures, loadsOk,
                 loadsFailed, cacheDrops, openCasts, blockedCasts, walks, walksFound, pausedTicks, packetsSent, packetsIn, Humans(), active.size());
    std::fprintf(log, "voices isaac slot=%d cct=%d reload=%d muted=%ld (pain %ld struggle %ld grunt %ld stomp %ld melee %ld death %ld quiet %ld; local %ld bot %ld ghost %ld "
                      "other %ld) grabs=%ld grabber_unknown=%ld cct deaths=%ld stomp_streams=%ld shocks=%ld reloads=%ld events sent=%ld in=%ld\n",
                 slotOurs ? 1 : 0, cctHooked ? 1 : 0, reloadHooked ? 1 : 0, muted, mutedKind[coop_voices::kVoPain], mutedKind[coop_voices::kVoStruggle],
                 mutedKind[coop_voices::kVoGrunt], mutedKind[coop_voices::kVoStomp], mutedKind[coop_voices::kVoMelee], mutedKind[coop_voices::kVoDeath],
                 mutedKind[coop_voices::kVoQuiet], mutedLocal, mutedBot, mutedGhost, mutedOther, grabsSeen, grabbersUnknown, cctDeaths, cctStomps, cctShocks, reloadsSeen,
                 eventsSent, eventsIn);
    if (situations)
        std::fprintf(log, "voices situations fired low_health=%ld low_ammo=%ld new_area=%ld big_enemy=%ld multi_kill=%ld necro_spotted=%ld team_held=%ld | low_health armed=%ld "
                          "grab_waits=%ld | low_ammo spare_check=%d faults=%ld | new_area candidates=%ld not_new=%ld wrong_kind=%ld moved=%ld partner_there=%ld | big_enemy "
                          "seen=%ld out_of_range=%ld out_of_view=%ld blocked=%ld kind_gap=%ld no_los=%ld | multi_kill kills=%ld not_counted=%ld not_ours=%ld health_hooks=%d | "
                          "necro_spotted seen=%ld blocked=%ld\n",
                     sitFired[coop_voices::kLowHealth], sitFired[coop_voices::kLowAmmo], sitFired[coop_voices::kNewArea], sitFired[coop_voices::kBigEnemy],
                     sitFired[coop_voices::kMultiKill], sitFired[coop_voices::kNecroSpotted], sitTeamHeld, lowHealthArmed, lowHealthGrabWaits, spareReady ? 1 : 0,
                     lowAmmoFaults, areaCandidates, areaNotNew, areaWrongKind, areaMoved, areaPartnerThere, bigSeen, bigOutOfRange, bigOutOfView, bigBlocked, bigKindGap,
                     bigNoLos, killsCounted, killsNotCounted, killsNotOurs, static_cast<bool>(hits::damageHookHc) ? 1 : 0, necroSeen, necroBlocked);
}
}  // namespace voices
