// Dead Space 2 United by Richard Hafer.
// The survival status line ([Hud] in DS2United.ini): wave, enemies left and the
// break's countdown, small at the top centre of the overlay instead of RIG
// notices (10.10.: the banners covered the middle of the screen).
//
// The randomizer's director pushes the line and an event line under it through
// DS2Coop_SurvivalHud (DS2RandomizerObserver.asi; it sends no RIG notices while
// DS2Coop_HudReady says 1). A host sends the pair to the partners over the
// survival side channel (survnet kind 4), since a client runs no director. The
// line hides in the pause menu, in a cutscene and outside a level.
namespace zone { bool OverlayText(char* out, size_t size); }   // runtime_coop_zone.inc
namespace hud {
bool enabled = true, installed = false;
int scalePercent = 75, topPermille = 18;
uint32_t textColor = 0, flashColor = 0;   // 0: the overlay's stock colours
constexpr DWORD kPollMs = 100, kLocalFreshMs = 1500, kRemoteFreshMs = 3500, kResendMs = 1000;
SRWLOCK lock = SRWLOCK_INIT;
char localLine[96] = "", localFlash[96] = "", remoteLine[96] = "", remoteFlash[96] = "", ownLine[96] = "";
DWORD localAt = 0, remoteAt = 0, lastPoll = 0, sentAt = 0, simStillSince = 0, ownAt = 0;
uint32_t lastSim = 0;
char sentText[136] = "";
volatile LONG pushes = 0, remoteIn = 0, sent = 0, shownTicks = 0, hiddenPaused = 0, hiddenCutscene = 0, changes = 0, lines = 0;
char shownLine[96] = "";

bool Ready() { return enabled && installed && overlay::ready && !InterlockedCompareExchange(&overlay::disabled, 0, 0); }
// Any thread (the director's).
void SetLocal(const char* line, const char* flash) {
    AcquireSRWLockExclusive(&lock);
    std::snprintf(localLine, sizeof(localLine), "%s", line ? line : "");
    std::snprintf(localFlash, sizeof(localFlash), "%s", flash ? flash : "");
    localAt = GetTickCount();
    if (!localAt) localAt = 1;
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&pushes);
}
// A line for this screen only, over the director's: a host never sends it on (a necro's respawn countdown).
void SetOwn(const char* line) {
    AcquireSRWLockExclusive(&lock);
    std::snprintf(ownLine, sizeof(ownLine), "%s", line ? line : "");
    ownAt = Stamp(GetTickCount());
    ReleaseSRWLockExclusive(&lock);
}
// Game thread, from survnet: "line\x1Fflash".
void SetRemote(const char* text) {
    const char* split = std::strchr(text, '\x1F');
    AcquireSRWLockExclusive(&lock);
    std::snprintf(remoteLine, sizeof(remoteLine), "%.*s", split ? static_cast<int>(split - text) : static_cast<int>(std::strlen(text)), text);
    std::snprintf(remoteFlash, sizeof(remoteFlash), "%s", split ? split + 1 : "");
    remoteAt = GetTickCount();
    if (!remoteAt) remoteAt = 1;
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement(&remoteIn);
}

// The director's thread stamps a later tick than the one this poll took: signed, so it counts as fresh.
bool Fresh(DWORD now, DWORD at, DWORD ms) { return at && static_cast<LONG>(now - at) < static_cast<LONG>(ms); }
void Tick(DWORD now) {
    if (!enabled || !installed || now - lastPoll < kPollMs) return;
    lastPoll = now;
    overlay::Hud h{};
    bool fresh = false;
    AcquireSRWLockShared(&lock);
    // A client's own director pushes empty lines (it runs no waves); those must not
    // hide the host's line (13.10.: remote_in=186, shown_ticks=0 on the client).
    if (Fresh(now, localAt, kLocalFreshMs) && (localLine[0] || !Fresh(now, remoteAt, kRemoteFreshMs))) {
        std::snprintf(h.line, sizeof(h.line), "%s", localLine);
        std::snprintf(h.flash, sizeof(h.flash), "%s", localFlash);
        fresh = true;
    } else if (Fresh(now, remoteAt, kRemoteFreshMs)) {
        std::snprintf(h.line, sizeof(h.line), "%s", remoteLine);
        std::snprintf(h.flash, sizeof(h.flash), "%s", remoteFlash);
        fresh = true;
    }
    const bool fromHere = Fresh(now, localAt, kLocalFreshMs);
    ReleaseSRWLockShared(&lock);
    // The host's line to the partners, on a change and once a second.
    if (fromHere && network && IsHost()) {
        char text[136];
        std::snprintf(text, sizeof(text), "%s\x1F%s", h.line, h.flash);
        // An empty line goes out once, as the change that clears it (12.10.: one a second all story long).
        if (std::strcmp(text, sentText) != 0 || (h.line[0] && now - sentAt >= kResendMs)) {
            std::snprintf(sentText, sizeof(sentText), "%s", text);
            sentAt = now;
            survnet::Queue(4, text, 0);
            InterlockedIncrement(&sent);
        }
    }
    AcquireSRWLockShared(&lock);
    if (Fresh(now, ownAt, kLocalFreshMs) && ownLine[0]) {
        std::snprintf(h.line, sizeof(h.line), "%s", ownLine);
        h.flash[0] = 0;
        fresh = true;
    }
    ReleaseSRWLockShared(&lock);
    // Game time stands still in the pause menu.
    const uint32_t sim = turret::SimMs();
    if (sim != lastSim) { lastSim = sim; simStillSince = now; }
    const bool paused = now - simStillSince > 300;
    const uint32_t player = inLevel ? LocalCampaignPlayer() : 0;
    const bool cutscene = player && turret::CutsceneNow();
    if (!fresh || !h.line[0] || !inLevel || paused || cutscene) {
        if (paused && fresh && h.line[0]) InterlockedIncrement(&hiddenPaused);
        if (cutscene && fresh && h.line[0]) InterlockedIncrement(&hiddenCutscene);
        if (shownLine[0]) shownLine[0] = 0;
        overlay::HideHud();
        return;
    }
    // Hold the Zone: the way to it from our own Isaac, after the director's line
    char way[40];
    if (zone::OverlayText(way, sizeof(way))) {
        const size_t used = std::strlen(h.line);
        std::snprintf(h.line + used, sizeof(h.line) - used, "  |  %s", way);
    }
    h.scalePercent = scalePercent;
    h.topPermille = topPermille;
    h.textColor = textColor;
    h.flashColor = flashColor;
    overlay::SetHud(h);
    InterlockedIncrement(&shownTicks);
    if (std::strcmp(shownLine, h.line) != 0) {
        std::snprintf(shownLine, sizeof(shownLine), "%s", h.line);
        InterlockedIncrement(&changes);
        if (InterlockedIncrement(&lines) <= 60) gate::Event("hud: \"%s\"%s%s%s", h.line, h.flash[0] ? " / \"" : "", h.flash, h.flash[0] ? "\"" : "");
    }
}

// A palette name or RRGGBB as ARGB with the given alpha; 0 for stock or anything else.
uint32_t ParseColor(const char* text, uint32_t alpha, bool& known) {
    struct Named { const char* name; uint32_t rgb; };
    static const Named kNamed[] = {{"red", 0xFF6A5A},    {"orange", 0xFFA552}, {"yellow", 0xFFE45C}, {"lime", 0xC4FF5C},
                                   {"green", 0x6CFF8E},  {"blue", 0x6CB8FF},   {"purple", 0xC08CFF}, {"pink", 0xFF82D0},
                                   {"white", 0xFFFFFF},  {"gray", 0xB4B4B4}};
    known = true;
    if (!text[0] || _stricmp(text, "stock") == 0) return 0;
    for (const Named& n : kNamed)
        if (_stricmp(text, n.name) == 0) return alpha << 24 | n.rgb;
    const char* hex = text[0] == '#' ? text + 1 : text;
    if (std::strlen(hex) == 6 && std::strspn(hex, "0123456789abcdefABCDEF") == 6) return alpha << 24 | static_cast<uint32_t>(std::strtoul(hex, nullptr, 16));
    known = false;
    return 0;
}

void Install(FILE* log, const std::string& ini) {
    enabled = GetPrivateProfileIntA("Hud", "Enabled", 1, ini.c_str()) != 0;
    char color[32]{}, flash[32]{};
    GetPrivateProfileStringA("Hud", "Color", "stock", color, sizeof(color), ini.c_str());
    GetPrivateProfileStringA("Hud", "FlashColor", "", flash, sizeof(flash), ini.c_str());
    bool colorKnown = true, flashKnown = true;
    textColor = ParseColor(color, 0xF0, colorKnown);
    // an empty FlashColor follows Color
    flashColor = flash[0] ? ParseColor(flash, 0xE8, flashKnown) : (textColor ? 0xE8000000u | (textColor & 0xFFFFFF) : 0);
    scalePercent = GetPrivateProfileIntA("Hud", "Scale", 75, ini.c_str());
    if (scalePercent < 40) scalePercent = 40;
    if (scalePercent > 200) scalePercent = 200;
    char text[16]{};
    GetPrivateProfileStringA("Hud", "Top", "1.8", text, sizeof(text), ini.c_str());
    const double top = std::atof(text);
    topPermille = top >= 0.0 && top <= 40.0 ? static_cast<int>(top * 10.0 + 0.5) : 18;
    if (!enabled) { std::fprintf(log, "hud off ([Hud] Enabled=0): the survival director uses RIG notices\n"); return; }
    if (!overlay::ready) overlay::Install(log, ini);
    installed = overlay::ready;
    std::fprintf(log, "hud %s: scale %d%% of the panel font, %.1f%% from the top%s\n", installed ? "on" : "OFF (the overlay did not go in)", scalePercent,
                 topPermille / 10.0, installed ? "; the survival director's RIG notices stay off while it is up" : "");
    std::fprintf(log, "hud colours: line %s -> %08X%s, events %s -> %08X%s (0 = stock)\n", color, textColor, colorKnown ? "" : " (unknown, stock)",
                 flash[0] ? flash : "(follows the line)", flashColor, flashKnown ? "" : " (unknown, stock)");
}
void WriteDiagnostics(FILE* log) {
    if (!enabled) return;
    static LONG last = -1;
    const LONG sum = InterlockedCompareExchange(&changes, 0, 0) + InterlockedCompareExchange(&remoteIn, 0, 0) * 3 + InterlockedCompareExchange(&sent, 0, 0) * 5 +
                     InterlockedCompareExchange(&hiddenPaused, 0, 0) * 7 + InterlockedCompareExchange(&hiddenCutscene, 0, 0) * 11;
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "hud installed=%d pushes=%ld remote_in=%ld sent=%ld changes=%ld shown_ticks=%ld hidden_paused=%ld hidden_cutscene=%ld line='%s'\n", installed ? 1 : 0,
                 InterlockedCompareExchange(&pushes, 0, 0), InterlockedCompareExchange(&remoteIn, 0, 0), InterlockedCompareExchange(&sent, 0, 0),
                 InterlockedCompareExchange(&changes, 0, 0), InterlockedCompareExchange(&shownTicks, 0, 0), InterlockedCompareExchange(&hiddenPaused, 0, 0),
                 InterlockedCompareExchange(&hiddenCutscene, 0, 0), shownLine);
}
}  // namespace hud
