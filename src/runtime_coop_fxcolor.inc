// Dead Space 2 United by Richard Hafer.
// Effect colours ([Visuals] StasisColor, KinesisColor, LaserColor, WeaponColor and one key per
// gun, BloodColor; analysis/DS2_FX_COLORS_2026-10-15.md). Every effect buffer passes
// TFXSequencer::PatchInit 0x006DA2C0 (cdecl(data), AL, only caller VFXManager::LoadInit
// 0x006A0B66) once when its stream loads, and the engine keeps using that buffer in place.
// Before the original runs the records still carry their tags and relative offsets: the
// first 16 bytes are the packed effect name (generated_fx_colors.h, tools/build_fx_colors.py),
// the record count is the byte at +0x1C, the record table starts at +0x40 (0x24 per entry,
// entry+0 = offset from the buffer), a record is tag(4) version(4) and its params at +0x0C.
// Each chromatic colour of a listed effect is turned about the grey axis onto the chosen hue,
// so a particle's start, minimum, maximum and delta stay consistent. Blood records (red band)
// take BloodColor, the blood textures go through rigcolor's texture hook.
namespace fxcolor {
constexpr uint32_t kPatchInitRva = 0x2DA2C0;
// sub esp,0x10 / push ebx / push ebp / xor ebx,ebx / push esi / push edi / mov edi,[esp+0x24] / and [edi+0x38],...
const unsigned char kPatchInitBytes[] = {0x83, 0xEC, 0x10, 0x53, 0x55, 0x33, 0xDB, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x24, 0x81, 0x67, 0x38};
using ds2fx::kGroupCount;

struct Choice { const char* name; float hue, saturation, value; };   // hue < 0: grey with value as gain
const Choice kChoices[] = {
    {"red", 2.0f, 1.0f, 1.0f},     {"orange", 26.0f, 1.0f, 1.0f}, {"yellow", 50.0f, 1.0f, 0.85f}, {"lime", 95.0f, 1.0f, 0.9f},
    {"green", 130.0f, 1.0f, 1.0f}, {"blue", 215.0f, 1.0f, 1.0f},  {"purple", 275.0f, 1.0f, 1.0f}, {"pink", 320.0f, 1.0f, 1.0f},
    {"white", -1.0f, 0.0f, 1.4f},  {"gray", -1.0f, 0.0f, 0.75f},
};
struct Xform { const Choice* choice = nullptr; float angle = 0.0f; };
Xform xforms[kGroupCount];
bool installed = false, throwTrail = false;
safetyhook::InlineHook patchHook;

volatile LONG loads = 0, matched[kGroupCount]{}, records[kGroupCount]{}, keys[kGroupCount]{}, bloodRecords = 0;
volatile LONG skippedGrey = 0, skippedBand = 0, versionMismatch = 0, badTables = 0, faults = 0;
constexpr unsigned kNamesPerGroup = 4;
volatile LONG namedSlots[kGroupCount]{};
const char* firstNames[kGroupCount][kNamesPerGroup]{};

// Hue as an angle in the chroma plane, 0 red, 120 green, 240 blue.
float HueAngle(float r, float g, float b) {
    float a = std::atan2(1.7320508f * (g - b), 2.0f * r - g - b) * 57.29578f;
    return a < 0.0f ? a + 360.0f : a;
}
bool Grey(float r, float g, float b) {
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    return mx <= 0.0f || mx - mn < 0.05f * mx;
}
bool RedBand(float angle) { return angle >= 330.0f || angle <= 35.0f; }

// The matrix that turns `from` degrees onto the choice's hue, with its saturation and value.
struct Matrix { float m[9]; float saturation, value; bool grey; };
Matrix Build(const Xform& x, float from) {
    Matrix out{};
    out.value = x.choice->value;
    out.saturation = x.choice->saturation;
    out.grey = x.choice->hue < 0.0f;
    if (out.grey) return out;
    const float t = (x.angle - from) / 57.29578f, c = std::cos(t), s = std::sin(t), k = (1.0f - c) / 3.0f, q = s / 1.7320508f;
    const float m[9] = {c + k, k - q, k + q, k + q, c + k, k - q, k - q, k + q, c + k};
    std::memcpy(out.m, m, sizeof(m));
    return out;
}
void Apply(const Matrix& m, float* v, float lo, float hi) {
    const float mean = (v[0] + v[1] + v[2]) / 3.0f;
    float n[3];
    if (m.grey) {
        n[0] = n[1] = n[2] = mean;
    } else {
        for (int r = 0; r < 3; ++r) n[r] = m.m[r * 3] * v[0] + m.m[r * 3 + 1] * v[1] + m.m[r * 3 + 2] * v[2];
        for (int r = 0; r < 3; ++r) n[r] = mean + (n[r] - mean) * m.saturation;
    }
    for (int r = 0; r < 3; ++r) v[r] = std::min(hi, std::max(lo, n[r] * m.value));
}

const ds2fx::Effect* Find(const uint8_t* key) {
    unsigned lo = 0, hi = ds2fx::kEffectCount;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        const int c = std::memcmp(ds2fx::kEffects[mid].key, key, 16);
        if (c == 0) return &ds2fx::kEffects[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

// One record type: its tag, version, the colour fields in its params and whether they are 0..255.
struct Field { uint32_t offset; bool signedDelta; };
struct Kind { uint32_t tag, version; Field fields[2]; unsigned count; };
constexpr uint32_t Tag(char a, char b, char c) { return static_cast<uint32_t>(a) | static_cast<uint32_t>(b) << 8 | static_cast<uint32_t>(c) << 16 | 0x2E000000u; }
const Kind kKinds[] = {
    {Tag('m', 'e', 'b'), 9, {{0x50, false}}, 1},                  // beam color (4th float: intensity)
    {Tag('g', 't', 'l'), 1, {{0x78, false}}, 1},                  // lightning
    {Tag('t', 'i', 'l'), 10, {{0x44, false}, {0x84, false}}, 2},  // light, shadow colour
    {Tag('c', 'l', 'f'), 10, {{0x20, false}}, 1},                 // corona
    {Tag('l', 'r', 't'), 11, {{0x48, false}}, 1},                 // trail
    {Tag('a', 'r', 't'), 0, {{0x14, false}}, 1},                  // tracer
    {Tag('p', 'm', 'i'), 14, {{0x98, false}}, 1},                 // impact, decal
    {Tag('r', 'c', 's'), 6, {{0x20, false}, {0x44, true}}, 2},    // screen colour and its delta
};
constexpr uint32_t kParticleTag = Tag('t', 'r', 'p'), kParticleVersion = 0x24;

// The transform for one record of an effect, or null when it stays.
const Xform* XformFor(const ds2fx::Effect& e, float angle) {
    const Xform& blood = xforms[ds2fx::kBlood];
    if (e.blood && blood.choice && RedBand(angle)) return &blood;
    if (e.group == ds2fx::kBlood) {
        InterlockedIncrement(&skippedBand);
        return nullptr;
    }
    if (e.group == ds2fx::kKinesisThrow && !throwTrail) return nullptr;
    return xforms[e.group].choice ? &xforms[e.group] : nullptr;
}
void Count(const Xform* x, const ds2fx::Effect& e, LONG changed) {
    const unsigned g = x == &xforms[ds2fx::kBlood] ? ds2fx::kBlood : e.group;
    if (g == ds2fx::kBlood) InterlockedIncrement(&bloodRecords);
    InterlockedIncrement(&records[g]);
    InterlockedExchangeAdd(&keys[g], changed);
}

void Particle(const ds2fx::Effect& e, uint8_t* rec, uint32_t length) {
    uint32_t version = 0, paramsAt = 0, colourAt = 0;
    std::memcpy(&version, rec + 4, 4);
    std::memcpy(&paramsAt, rec + 0x10, 4);
    std::memcpy(&colourAt, rec + 0x14, 4);
    if (version != kParticleVersion || (paramsAt != 0x10 && paramsAt != 0x18) || colourAt - paramsAt != 0x1C0) {
        InterlockedIncrement(&versionMismatch);
        return;
    }
    if (length && 0x0C + paramsAt + 0x210 > length) { InterlockedIncrement(&badTables); return; }
    float* f = reinterpret_cast<float*>(rec + 0x0C + paramsAt + 0x1D0);   // delta, min, max, start: 4 floats each
    const float* ref = !Grey(f[12], f[13], f[14]) ? f + 12 : !Grey(f[8], f[9], f[10]) ? f + 8 : !Grey(f[4], f[5], f[6]) ? f + 4 : nullptr;
    if (!ref) { InterlockedIncrement(&skippedGrey); return; }
    const float angle = HueAngle(ref[0], ref[1], ref[2]);
    const Xform* x = XformFor(e, angle);
    if (!x) return;
    const Matrix m = Build(*x, angle);
    Apply(m, f, -255.0f, 255.0f);
    for (int k = 1; k < 4; ++k) Apply(m, f + 4 * k, 0.0f, 255.0f);
    Count(x, e, 4);
}
void Other(const ds2fx::Effect& e, uint8_t* rec, const Kind& kind, uint32_t length) {
    uint32_t version = 0;
    std::memcpy(&version, rec + 4, 4);
    if (version != kind.version) { InterlockedIncrement(&versionMismatch); return; }
    float* main = reinterpret_cast<float*>(rec + 0x0C + kind.fields[0].offset);
    if (length && 0x0C + kind.fields[kind.count - 1].offset + 12 > length) { InterlockedIncrement(&badTables); return; }
    if (Grey(main[0], main[1], main[2])) { InterlockedIncrement(&skippedGrey); return; }
    const float angle = HueAngle(main[0], main[1], main[2]);
    const Xform* x = XformFor(e, angle);
    if (!x) return;
    const Matrix m = Build(*x, angle);
    for (unsigned i = 0; i < kind.count; ++i) {
        float* v = reinterpret_cast<float*>(rec + 0x0C + kind.fields[i].offset);
        Apply(m, v, kind.fields[i].signedDelta ? -64.0f : 0.0f, 64.0f);
    }
    Count(x, e, static_cast<LONG>(kind.count));
}

__declspec(noinline) void Recolour(uint8_t* data) {
    __try {
        const ds2fx::Effect* e = Find(data);
        if (!e) return;
        InterlockedIncrement(&matched[e->group]);
        const LONG slot = InterlockedIncrement(&namedSlots[e->group]) - 1;
        if (slot >= 0 && slot < static_cast<LONG>(kNamesPerGroup)) firstNames[e->group][slot] = e->name;
        const unsigned n = data[0x1C];
        if (!n || n > 63) { InterlockedIncrement(&badTables); return; }
        uint32_t offsets[64];
        for (unsigned i = 0; i < n; ++i) {
            std::memcpy(&offsets[i], data + 0x40 + 0x24 * i, 4);
            if (offsets[i] < 0x40 + 0x24 * n || offsets[i] >= 0x10000 || (i && offsets[i] <= offsets[i - 1])) { InterlockedIncrement(&badTables); return; }
        }
        for (unsigned i = 0; i < n; ++i) {
            uint8_t* rec = data + offsets[i];
            const uint32_t length = i + 1 < n ? offsets[i + 1] - offsets[i] : 0;   // the last record's end is unknown
            uint32_t tag = 0;
            std::memcpy(&tag, rec, 4);
            if (tag == kParticleTag) {
                Particle(*e, rec, length);
                continue;
            }
            for (const Kind& k : kKinds)
                if (k.tag == tag) { Other(*e, rec, k, length); break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
    }
}

bool __cdecl OnPatchInit(uint8_t* data) {
    InterlockedIncrement(&loads);
    if (data) Recolour(data);
    return patchHook.call<bool>(data);
}

const Choice* Pick(const char* text) {
    for (const Choice& c : kChoices)
        if (_stricmp(text, c.name) == 0) return &c;
    return nullptr;
}
float AngleOf(const Choice& c) {
    if (c.hue < 0.0f) return 0.0f;
    // the hue's full colour, measured the way records are
    const float h = c.hue / 60.0f, f = h - std::floor(h);
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(std::floor(h)) % 6) {
        case 0: r = 1; g = f; break;
        case 1: r = 1 - f; g = 1; break;
        case 2: g = 1; b = f; break;
        case 3: g = 1 - f; b = 1; break;
        case 4: r = f; b = 1; break;
        default: r = 1; b = 1 - f; break;
    }
    return HueAngle(r, g, b);
}
void EarlyInstall(FILE* log, const std::string& ini) {
    struct Key { const char* key; unsigned group; };
    const Key kKeys[] = {{"StasisColor", ds2fx::kStasis},       {"KinesisColor", ds2fx::kKinesis},      {"LaserColor", ds2fx::kLaser},
                         {"BloodColor", ds2fx::kBlood},         {"PlasmaCutterColor", ds2fx::kPlasmaCutter}, {"PulseRifleColor", ds2fx::kPulseRifle},
                         {"LineGunColor", ds2fx::kLineGun},     {"ContactBeamColor", ds2fx::kContactBeam}, {"RipperColor", ds2fx::kRipper},
                         {"FlamethrowerColor", ds2fx::kFlamethrower}, {"ForceGunColor", ds2fx::kForceGun}, {"JavelinColor", ds2fx::kJavelin},
                         {"DetonatorColor", ds2fx::kDetonator}, {"SeekerRifleColor", ds2fx::kSeekerRifle}, {"RivetGunColor", ds2fx::kRivetGun}};
    char weapon[32]{};
    GetPrivateProfileStringA("Visuals", "WeaponColor", "stock", weapon, sizeof(weapon), ini.c_str());
    const Choice* weaponChoice = Pick(weapon);
    if (weapon[0] && _stricmp(weapon, "stock") && !weaponChoice) std::fprintf(log, "fx colours: WeaponColor=%s is no colour; stock\n", weapon);
    throwTrail = GetPrivateProfileIntA("Visuals", "KinesisThrowTrail", 0, ini.c_str()) != 0;
    std::string summary;
    for (const Key& k : kKeys) {
        char text[32]{};
        GetPrivateProfileStringA("Visuals", k.key, "", text, sizeof(text), ini.c_str());
        const Choice* c = text[0] ? Pick(text) : nullptr;
        if (text[0] && _stricmp(text, "stock") && !c) std::fprintf(log, "fx colours: %s=%s is no colour; stock\n", k.key, text);
        // a gun without its own key follows WeaponColor; an own "stock" keeps it stock
        if (!text[0] && k.group >= ds2fx::kPlasmaCutter) c = weaponChoice;
        xforms[k.group].choice = c;
        xforms[k.group].angle = c ? AngleOf(*c) : 0.0f;
        if (c) summary += std::string(" ") + ds2fx::kGroupNames[k.group] + "=" + c->name;
    }
    xforms[ds2fx::kKinesisThrow] = xforms[ds2fx::kKinesis];
    if (summary.empty()) {
        std::fprintf(log, "fx colours: all stock ([Visuals] StasisColor, KinesisColor, LaserColor, WeaponColor, BloodColor)\n");
        return;
    }
    installed = hook_site::Install(log, "fx colours TFXSequencer::PatchInit", base + kPatchInitRva, kPatchInitBytes, sizeof(kPatchInitBytes),
                                   reinterpret_cast<void*>(OnPatchInit), patchHook);
    std::fprintf(log, "fx colours %s:%s; kinesis throw trail %s; %u effects listed\n", installed ? "on" : "NOT installed", summary.c_str(),
                 throwTrail ? "too" : "stock ([Visuals] KinesisThrowTrail=0)", ds2fx::kEffectCount);
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    LONG sum = InterlockedCompareExchange(&faults, 0, 0) * 7 + InterlockedCompareExchange(&badTables, 0, 0) * 5 +
               InterlockedCompareExchange(&versionMismatch, 0, 0) * 3 + InterlockedCompareExchange(&bloodRecords, 0, 0);
    for (unsigned g = 0; g < kGroupCount; ++g) sum += InterlockedCompareExchange(&records[g], 0, 0) * static_cast<LONG>(g + 11);
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "fx colours loads=%ld version_mismatch=%ld bad_tables=%ld skipped_grey=%ld skipped_band=%ld faults=%ld:", loads, versionMismatch, badTables,
                 skippedGrey, skippedBand, faults);
    for (unsigned g = 0; g < kGroupCount; ++g) {
        if (!matched[g] && !records[g]) continue;
        std::fprintf(log, " %s effects=%ld records=%ld keys=%ld (", ds2fx::kGroupNames[g], matched[g], records[g], keys[g]);
        for (unsigned i = 0; i < kNamesPerGroup && firstNames[g][i]; ++i) std::fprintf(log, "%s%s", i ? " " : "", firstNames[g][i]);
        std::fprintf(log, ")");
    }
    std::fprintf(log, "\n");
}
}  // namespace fxcolor
