// Dead Space 2 United by Richard Hafer.
#include "../src/coop_difficulty.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace coop_difficulty;

static void Names() {
    assert(std::strcmp(Name(0), "casual") == 0 && std::strcmp(Name(4), "hard core") == 0 && std::strcmp(Name(5), "?") == 0 && std::strcmp(Name(-1), "?") == 0);
    assert(std::strcmp(HudName(3), "ZEALOT") == 0 && std::strcmp(HudName(2), "SURVIVALIST") == 0 && HudName(9)[0] == 0);
    assert(Valid(0) && Valid(4) && !Valid(5) && !Valid(-1) && !Valid(0xFF));
    std::printf("names ok\n");
}

static void Picker() {
    // rows 0..3 are always on; Hard Core is locked in our route unless AllowHardcore opens it
    assert(PickAllowed(0, true, false) && PickAllowed(3, true, false));
    assert(!PickAllowed(4, false, false) && PickAllowed(4, false, true) && PickAllowed(4, true, false));
    assert(!PickAllowed(2, false, true));   // only Hard Core is opened
    assert(!PickAllowed(5, true, true) && !PickAllowed(-1, true, true));
    // the shared Select: an owned front-end pick never goes native, a match refuses in game
    assert(RouteSelect(true, true, false, true) == Route::Own);
    assert(RouteSelect(true, false, false, true) == Route::Forward);   // the campaign's own SP02
    assert(RouteSelect(true, true, true, true) == Route::Own);         // the front end has no match
    assert(RouteSelect(false, false, true, true) == Route::Refuse);
    assert(RouteSelect(false, true, true, true) == Route::Refuse);     // a stale owned flag changes nothing in game
    assert(RouteSelect(false, false, true, false) == Route::Forward);  // LockInMatch=0
    assert(RouteSelect(false, false, false, true) == Route::Forward);  // story co-op stays native
    assert(LowestAfterChange(1, 3) == 1 && LowestAfterChange(3, 1) == 1 && LowestAfterChange(-1, 2) == 2 && LowestAfterChange(7, 2) == 2);
    std::printf("picker ok\n");
}

static void Fields() {
    assert(LobbyByte(3) == 3 && LobbyByte(-1) == kNone && LobbyByte(5) == kNone);
    assert(FromLobbyByte(4) == 4 && FromLobbyByte(kNone) == -1 && FromLobbyByte(5) == -1);
    assert(CampaignField(0) == 1 && CampaignField(4) == 5 && CampaignField(-1) == 0);
    assert(FromCampaignField(0) == -1 && FromCampaignField(1) == 0 && FromCampaignField(5) == 4 && FromCampaignField(6) == -1);
    for (int d = 0; d < kCount; ++d) assert(FromCampaignField(CampaignField(d)) == d && FromLobbyByte(LobbyByte(d)) == d);
    const uint32_t v = SurvnetValue(0x1234, 3);
    assert(v == 0x123403 && SurvnetDifficulty(v) == 3 && SurvnetGeneration(v) == 0x1234);
    assert(SurvnetDifficulty(SurvnetValue(7, -1)) == -1);
    HostWire h;
    assert(h.zero == 0 && h.difficulty == kNone);
    std::printf("fields ok\n");
}

static void Host() {
    Leader l;
    assert(!l.RefreshDue(0, 2000));            // nothing to refresh before the first value
    assert(l.Observe(1) && l.generation == 1 && l.value == 1);
    assert(!l.Observe(1) && l.generation == 1);  // unchanged
    assert(!l.Observe(-1) && !l.Observe(7) && l.value == 1);   // unreadable or out of range
    assert(l.Observe(3) && l.generation == 2);
    l.Bump();
    assert(l.generation == 3 && l.value == 3);
    assert(l.RefreshDue(1000, 2000) && !l.RefreshDue(2999, 2000) && l.RefreshDue(3000, 2000));
    assert(Judge(2, 1, false) == Verdict::Apply && Judge(1, 1, false) == Verdict::Same);
    assert(Judge(2, 1, true) == Verdict::RefusedLocked && Judge(9, 1, false) == Verdict::RefusedInvalid);
    RequestLog log;
    assert(log.First(0xA, 1) && !log.First(0xA, 1) && log.First(0xA, 2) && log.First(0xB, 1));
    for (uint32_t r = 10; r < 30; ++r) assert(log.First(0xC, r));   // the ring forgets old ones, never new ones
    assert(!log.First(0xC, 29));
    std::printf("host ok\n");
}

static void Client() {
    const uint32_t host = 0x1111, own = 0x2222, hold = 10000;
    Follower f;
    assert(f.OnHost(0, 1, 1, 0, 0, own, 0, hold) == Follower::Take::Ignored);
    assert(f.OnHost(host, 0, 1, 0, 0, own, 0, hold) == Follower::Take::Ignored);
    assert(f.OnHost(host, 1, 9, 0, 0, own, 0, hold) == Follower::Take::Ignored);
    assert(f.OnHost(host, 1, 1, 0, 0, own, 0, hold) == Follower::Take::Apply && f.value == 1 && f.generation == 1);
    // the three copies and the refreshes of the same generation change nothing
    assert(f.OnHost(host, 1, 1, 0, 0, own, 100, hold) == Follower::Take::Stale);
    assert(f.OnHost(host, 3, 3, 0, 0, own, 200, hold) == Follower::Take::Apply && f.value == 3);
    // a late copy of generation 2 never takes back generation 3
    assert(f.OnHost(host, 2, 2, 0, 0, own, 300, hold) == Follower::Take::Stale && f.value == 3);
    // our change (zealot -> casual): the host's older refresh waits, the answer counts
    const uint32_t r = f.Ask(0, 1000);
    assert(f.pending && f.base == 3 && r == 1);
    assert(f.OnHost(host, 3, 3, 0, 0, own, 1500, hold) == Follower::Take::Held && f.value == 3);
    assert(f.OnHost(host, 4, 3, 0, 0, own, 1600, hold) == Follower::Take::Held);   // another change before ours was seen
    assert(f.OnHost(host, 5, 0, own, r, own, 1700, hold) == Follower::Take::Apply && !f.pending && f.value == 0 && f.generation == 5);
    // a refused request: the answer carries the host's value, the client goes back to it
    const uint32_t r2 = f.Ask(2, 2000);
    assert(f.OnHost(host, 6, 0, own, r2, own, 2100, hold) == Follower::Take::Apply && f.value == 0);
    // another client's answer is no answer for us
    f.Ask(4, 3000);
    assert(f.OnHost(host, 7, 4, 0x3333, f.request, own, 3100, hold) == Follower::Take::Held);
    // no answer within the hold: the host's word counts again
    assert(!f.Expired(3000 + hold - 1, hold) && f.Expired(3000 + hold, hold) && !f.pending);
    assert(f.OnHost(host, 7, 0, 0, 0, own, 3000 + hold + 50, hold) == Follower::Take::Apply && f.value == 0);
    f.Ask(1, 20000);
    assert(f.OnHost(host, 8, 0, 0, 0, own, 20000 + hold, hold) == Follower::Take::Apply && !f.pending);   // the wait ran out on a packet too
    // the host started again: its generations begin anew
    assert(f.OnHost(0x9999, 1, 2, 0, 0, own, 40000, hold) == Follower::Take::Apply && f.value == 2 && f.generation == 1 && f.hostToken == 0x9999);
    // the tick: a pick in the pause menu is asked for, anything else goes back to the host's value
    LocalPick pick;
    assert(!pick.Matches(2, 0, 60000));
    pick.Arm(2, 5000);
    assert(pick.Matches(2, 6000, 60000) && !pick.Matches(3, 6000, 60000) && !pick.Matches(2, 70000, 60000));
    pick.Clear();
    assert(!pick.Matches(2, 6000, 60000));
    assert(DecideClient(2, 1, true, false, 1) == ClientStep::Ask);
    assert(DecideClient(2, 1, false, false, 1) == ClientStep::Restore);   // a save's restore
    assert(DecideClient(2, 2, false, true, 1) == ClientStep::None);       // our request still waits
    assert(DecideClient(1, 1, false, false, 1) == ClientStep::None);
    assert(DecideClient(2, 2, false, false, -1) == ClientStep::None);     // no host value yet
    assert(DecideClient(-1, 1, true, false, 1) == ClientStep::Restore);   // unreadable never asks
    std::printf("client ok\n");
}

static void Hud() {
    char line[96] = "WAVE 4  -  11 LEFT";
    assert(AppendSuffix(line, sizeof(line), 3) && std::strcmp(line, "WAVE 4  -  11 LEFT  -  ZEALOT") == 0);
    char mode[96] = "OBJECTIVE 2/5  |  3:10  |  KILLS 12";
    assert(AppendSuffix(mode, sizeof(mode), 4) && std::strcmp(mode, "OBJECTIVE 2/5  |  3:10  |  KILLS 12  |  HARD CORE") == 0);
    char empty[96] = "";
    assert(!AppendSuffix(empty, sizeof(empty), 1) && !empty[0]);
    char unknown[96] = "WAVE 1  -  3 LEFT";
    assert(!AppendSuffix(unknown, sizeof(unknown), -1) && std::strcmp(unknown, "WAVE 1  -  3 LEFT") == 0);
    // never cut: a line too long keeps its own end
    char tight[24] = "WAVE 12  -  110 LEFT";
    assert(!AppendSuffix(tight, sizeof(tight), 1) && std::strcmp(tight, "WAVE 12  -  110 LEFT") == 0);
    char exact[30] = "WAVE 4  -  11 LEFT";   // 18 + 5 + 6 = 29 characters and the terminator
    assert(AppendSuffix(exact, sizeof(exact), 3) && std::strlen(exact) == 29);
    std::printf("hud ok\n");
}

int main() {
    Names();
    Picker();
    Fields();
    Host();
    Client();
    Hud();
    std::printf("Difficulty passed: picker rows, routes, packet fields, generations, held requests, HUD suffix.\n");
    return 0;
}
