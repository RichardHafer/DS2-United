// Dead Space 2 United by Hafer.
// Our own item calls without the multiplayer bits.
//
// While a Versus round runs, the level manager carries the multiplayer (0x2) and
// host (0x4) bits (runtime_coop_versus.inc). The loadout, the bots' ammo and a
// restored inventory spawn and pick up items through raw calls; with the bits
// the pickup takes the multiplayer path, which faults without a Blaze player,
// and the SEH guard around it then leaves the game's stack SafePtrs linked into
// the item. The map's unload walked one of them (host dump 11.10. 21:40,
// Base::~Base of a SpareAmmo). Inside a Scope the bits are off and come back
// after it; everything here runs on the game thread.
namespace mpquiet {
constexpr uint32_t kLevelManager = 0x1C1C0A4, kFlags = 0x1C;
int depth = 0;
uint32_t lm = 0;
uint16_t taken = 0;
volatile LONG scopes = 0, faults = 0;
void Enter() {
    if (depth++) return;
    taken = 0;
    lm = 0;
    uint32_t m = 0;
    uint16_t flags = 0;
    if (!Read(base + kLevelManager, &m, 4) || !m || !Read(m + kFlags, &flags, 2) || !(flags & 0x6)) return;
    const uint16_t off = static_cast<uint16_t>(flags & ~0x6);
    if (!CopyIn(m + kFlags, &off, 2)) return;
    lm = m;
    taken = static_cast<uint16_t>(flags & 0x6);
    InterlockedIncrement(&scopes);
}
void Leave() {
    if (--depth > 0 || !taken) return;
    uint32_t m = 0;
    uint16_t flags = 0;
    if (Read(base + kLevelManager, &m, 4) && m == lm && Read(m + kFlags, &flags, 2)) {
        const uint16_t on = static_cast<uint16_t>(flags | taken);
        CopyIn(m + kFlags, &on, 2);
    }
    taken = 0;
}
struct Scope {
    Scope() { Enter(); }
    ~Scope() { Leave(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}  // namespace mpquiet
