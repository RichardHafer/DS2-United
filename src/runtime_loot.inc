// Dead Space 2 United by Richard Hafer.
// Loot amounts: more ammo and credits per pickup.
//
// Every pickup decides its quantity once, in PickupItem's constructor
// (0x00DCEE20, PDB size 760): the authored CMD_SetAmount at data+0x9C, or a
// seeded draw between CMD_SetRandomAmountMin (+0xA4) and Max (+0xAC) in
// steps of Multiple (+0xA8) when CMD_SetUseRandomAmount (+0xA0) is set. Both
// paths meet at 0x00DCF1F4 with the result in the object at +0x260, and that
// is where this hook scales it. Per instance, so the shared attribute block
// is never touched and nothing scales twice. Data pointer = [object+0xC],
// class descriptor = [data-0x10]: SpareAmmo and MultiAmmo are ammo.
//
// Credits are different (log of 15.09. 14:55: every credit chip is a PowerUp
// with amount 1). Their worth is not the amount but a span in the shared
// attribute block, CMD_SetBonusCurrency at data+0x344 (upper) and
// CMD_SetMinBonusCurrency at +0x348 (lower), drawn when the chip is picked
// up. So for a PowerUp the span itself is scaled, once per block: a small
// table remembers what was written where, and a block that still holds that
// value is left alone. A block freed and decoded again shows its authored
// values, which no longer match, and is scaled afresh. Health kits and the
// like carry no currency span and are not touched.
// The game's own difficulty scaling runs later, on top of this.
namespace loot {
safetyhook::MidHook amountHook;
bool installed = false;
unsigned ammoPercent = 100, creditsPercent = 100;
constexpr uint32_t kDescriptorSpareAmmo = 0x1C98728, kDescriptorMultiAmmo = 0x1C97F08, kDescriptorPowerUp = 0x1C97198;
volatile LONG seen = 0, scaledAmmo = 0, scaledCredits = 0;
struct Sample { LONG ready; uint32_t object, descriptor; int before, after; char kind[8]; char name[40]; };
Sample samples[200]{};
volatile LONG sampleCount = 0;

void Record(uint32_t object, uint32_t descriptor, int before, int after, const char* kind, const char* name) {
    const LONG slot = InterlockedIncrement(&sampleCount) - 1;
    if (slot < 0 || slot >= static_cast<LONG>(std::size(samples))) return;
    auto& s = samples[slot];
    s.object = object; s.descriptor = descriptor; s.before = before; s.after = after;
    std::snprintf(s.kind, sizeof(s.kind), "%s", kind);
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    InterlockedExchange(&s.ready, 1);
}

struct CurrencyBlock { uint32_t data; int upper, lower; };
CurrencyBlock currencyBlocks[64]{};
size_t currencyBlockCount = 0;
std::mutex currencyLock;

// True when the span at `data` was already scaled to exactly these values.
bool CurrencyScaled(uint32_t data, int upper, int lower) {
    for (size_t i = 0; i < currencyBlockCount; ++i)
        if (currencyBlocks[i].data == data) return currencyBlocks[i].upper == upper && currencyBlocks[i].lower == lower;
    return false;
}

// A slot for `data`: its own, a free one, or one whose block no longer holds
// the scaled values (freed or decoded again). Never a live entry: overwriting
// one let that block be scaled a second time, and two blocks taking turns in
// the last slot of a full table compounded the scale on every pickup.
// SIZE_MAX when every slot is live; the caller then leaves the block alone.
size_t CurrencySlot(uint32_t data) {
    for (size_t i = 0; i < currencyBlockCount; ++i)
        if (currencyBlocks[i].data == data) return i;
    if (currencyBlockCount < std::size(currencyBlocks)) return currencyBlockCount;
    for (size_t i = 0; i < currencyBlockCount; ++i) {
        int upper = 0, lower = 0;
        if (!Read(currencyBlocks[i].data + 0x344, &upper, 4) || !Read(currencyBlocks[i].data + 0x348, &lower, 4) ||
            upper != currencyBlocks[i].upper || lower != currencyBlocks[i].lower)
            return i;
    }
    return SIZE_MAX;
}

void RememberCurrency(size_t slot, uint32_t data, int upper, int lower) {
    if (slot >= std::size(currencyBlocks)) return;
    currencyBlocks[slot] = {data, upper, lower};
    if (slot == currencyBlockCount) ++currencyBlockCount;
}

int Scaled(int amount, unsigned percent) {
    int after = static_cast<int>((static_cast<long long>(amount) * percent + 50) / 100);
    return after < 1 ? 1 : after;
}

// 0x00DCF1F4: esi = the pickup, its quantity settled at +0x260.
void OnAmountSettled(SafetyHookContext& ctx) {
    uint32_t data = 0, descriptor = 0;
    int amount = 0;
    if (!Read(ctx.esi + 0xC, &data, 4) || data <= 0x10 || !Read(data - 0x10, &descriptor, 4) ||
        !Read(ctx.esi + 0x260, &amount, 4)) return;
    InterlockedIncrement(&seen);
    const bool ammo = descriptor == base + kDescriptorSpareAmmo || descriptor == base + kDescriptorMultiAmmo;
    if (ammo) {
        int after = amount;
        if (ammoPercent != 100 && amount > 0) {
            after = Scaled(amount, ammoPercent);
            if (WriteBytes(ctx.esi + 0x260, &after, sizeof(after))) InterlockedIncrement(&scaledAmmo);
            else after = amount;
        }
        Record(ctx.esi, descriptor, amount, after, "ammo", "");
        return;
    }
    if (descriptor == base + kDescriptorPowerUp) {
        int upper = 0, lower = 0;
        if (Read(data + 0x344, &upper, 4) && Read(data + 0x348, &lower, 4) && upper > 0) {
            std::lock_guard<std::mutex> guard(currencyLock);
            const size_t slot = creditsPercent != 100 && !CurrencyScaled(data, upper, lower) ? CurrencySlot(data) : SIZE_MAX;
            if (slot != SIZE_MAX) {
                const int newUpper = Scaled(upper, creditsPercent), newLower = lower > 0 ? Scaled(lower, creditsPercent) : lower;
                if (WriteBytes(data + 0x344, &newUpper, sizeof(newUpper)) && WriteBytes(data + 0x348, &newLower, sizeof(newLower))) {
                    RememberCurrency(slot, data, newUpper, newLower);
                    InterlockedIncrement(&scaledCredits);
                    Record(ctx.esi, data, lower, newLower, "credits", "");
                    Record(ctx.esi, data, upper, newUpper, "credits", "");
                    return;
                }
            }
            Record(ctx.esi, data, lower, lower, "credits", "");
            Record(ctx.esi, data, upper, upper, "credits", "");
            return;
        }
        // a PowerUp without a currency span: show its bonus words once, so a
        // credit chip that keeps its worth elsewhere can be told apart
        int words[7]{};
        Read(data + 0x330, words, sizeof(words));
        char text[40]{};
        std::snprintf(text, sizeof(text), "%d/%d/%d/%d/%d/%d/%d", words[0], words[1], words[2], words[3], words[4], words[5], words[6]);
        Record(ctx.esi, data, amount, amount, "powerup", text);
        return;
    }
    static LONG others = 0;
    if (InterlockedIncrement(&others) <= 12) Record(ctx.esi, descriptor, amount, amount, "other", "");
}

bool Install(FILE* log, const std::string& ini) {
    ammoPercent = static_cast<unsigned>(ConfigInt(ini, "Loot", "AmmoPercent", 100, 25, 500));
    creditsPercent = static_cast<unsigned>(ConfigInt(ini, "Loot", "CreditsPercent", 100, 25, 500));
    // push esi / lea ecx,[esi+0x230] / call 0xDCEAC0
    static const unsigned char kSite[] = {0x56,0x8D,0x8E,0x30,0x02,0x00,0x00,0xE8,0xC0,0xF8,0xFF,0xFF};
    unsigned char actual[sizeof(kSite)]{};
    if (!Read(base + 0x9CF1F4, actual, sizeof(actual)) || std::memcmp(actual, kSite, sizeof(kSite)) != 0) {
        std::fprintf(log, "loot signature_mismatch at %08X; amounts untouched\n", static_cast<unsigned>(base + 0x9CF1F4));
        return false;
    }
    amountHook = safetyhook::create_mid(reinterpret_cast<void*>(base + 0x9CF1F4), OnAmountSettled);
    if (!amountHook) {
        std::fprintf(log, "loot hook_failed at %08X\n", static_cast<unsigned>(base + 0x9CF1F4));
        return false;
    }
    installed = true;
    std::fprintf(log, "loot installed at %08X ammo_percent=%u credits_percent=%u (100 = as authored)\n",
                 static_cast<unsigned>(base + 0x9CF1F4), ammoPercent, creditsPercent);
    return true;
}

void WriteDiagnostics(FILE* log) {
    static unsigned next = 0;
    static LONG last = -1;
    while (next < std::size(samples) && InterlockedCompareExchange(&samples[next].ready, 1, 1) == 1) {
        const auto& s = samples[next++];
        std::fprintf(log, "loot pickup=%08X descriptor=%08X kind=%s name=%s amount=%d -> %d\n",
                     s.object, s.descriptor, s.kind, s.name[0] ? s.name : "?", s.before, s.after);
    }
    const LONG now = InterlockedCompareExchange(&seen, 0, 0);
    if (now != last) {
        std::fprintf(log, "loot summary pickups=%ld scaled_ammo=%ld scaled_credits=%ld\n", now,
                     InterlockedCompareExchange(&scaledAmmo, 0, 0), InterlockedCompareExchange(&scaledCredits, 0, 0));
        last = now;
    }
}
}  // namespace loot
