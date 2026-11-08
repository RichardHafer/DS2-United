// Dead Space 2 United by Hafer.
// RIG and visor colours ([Visuals] RigColor, VisorColor). The textures the TexMod RIG and
// visor colour packs replace are found the way TexMod finds them, by the CRC32 of the top
// level without the final inversion, when TX_ReplaceTextureContents (0x013C9030,
// cdecl(texture, size, data, flag), every mip level in data) uploads them, and recoloured on
// the way: every DXT1 colour endpoint takes the hue of the choice (none for white and gray),
// the lightness stays. RIG: healthbargradient_c (32x32, in every suit's stream). Visor:
// player*helmetscreen_c (16x16) and the lens flares lf01, lf_burst_01 (global_assets) and
// lf_glow_01 (level streams); the flares are shared with other lights, as in TexMod's pack.
// Installed right after the unpack: global_assets uploads its flares before InstallDelayMs.
namespace rigcolor {
constexpr uint32_t kUploadRva = 0xFC9030;
// push ebp / mov ebp,[esp+8] / mov eax,ebp / cmp ebp,-1 / jne / mov eax,0xFFF
const unsigned char kUploadBytes[] = {0x55, 0x8B, 0x6C, 0x24, 0x08, 0x8B, 0xC5, 0x83, 0xFD, 0xFF, 0x75, 0x07, 0xB8, 0xFF, 0x0F, 0x00};
struct Target { uint32_t hash, level0, total0, total1; bool rig; const char* name; };
const Target kTargets[] = {
    {0xA21816D4, 512, 696, 0, true, "healthbargradient_c"},
    {0x877ADE19, 128, 184, 192, false, "player*helmetscreen_c"},
    {0x76131AA6, 512, 672, 0, false, "lf01"},
    {0x7A4C3E1C, 32768, 43008, 0, false, "lf_burst_01"},
    {0xBC95D28B, 32768, 43008, 0, false, "lf_glow_01"},
};
constexpr size_t kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);
struct Choice { const char* name; float hue, saturation, value; };   // hue < 0: grey
const Choice kChoices[] = {
    {"red", 2.0f, 1.1f, 1.0f},    {"orange", 26.0f, 1.1f, 1.0f}, {"yellow", 50.0f, 1.0f, 1.0f}, {"lime", 95.0f, 1.0f, 1.0f},
    {"green", 130.0f, 1.0f, 1.0f}, {"blue", 215.0f, 1.0f, 1.0f},  {"purple", 275.0f, 1.0f, 1.0f}, {"pink", 320.0f, 1.0f, 1.0f},
    {"white", -1.0f, 0.0f, 1.2f},  {"gray", -1.0f, 0.0f, 0.8f},
};
safetyhook::InlineHook uploadHook;
bool installed = false;
const Choice* rigChoice = nullptr;
const Choice* visorChoice = nullptr;
const Choice* bloodChoice = nullptr;
std::vector<uint16_t> rigLut, visorLut, bloodLut;
uint32_t crcTable[256];
volatile LONG uploads = 0, candidates = 0, faults = 0, recoloured[kTargetCount]{};
// [Visuals] BloodColor: the blood textures of the effects (generated_fx_colors.h), DXT1 and DXT5
volatile LONG bloodCandidates = 0, bloodHashes = 0, bloodRecoloured[ds2fx::kBloodTextureCount]{};

void BuildCrc() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[i] = c;
    }
}
// TexMod's hash: CRC32 without the final inversion
__declspec(noinline) bool TexModHash(const uint8_t* data, uint32_t size, uint32_t& out) {
    __try {
        uint32_t c = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < size; ++i) c = crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        out = c;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}
__declspec(noinline) bool CopyIn(uint8_t* to, const uint8_t* from, uint32_t size) {
    __try {
        std::memcpy(to, from, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&faults);
        return false;
    }
}

void Rgb565(uint16_t v, float& r, float& g, float& b) {
    const uint32_t r5 = v >> 11 & 31, g6 = v >> 5 & 63, b5 = v & 31;
    r = static_cast<float>(r5 << 3 | r5 >> 2) / 255.0f;
    g = static_cast<float>(g6 << 2 | g6 >> 4) / 255.0f;
    b = static_cast<float>(b5 << 3 | b5 >> 2) / 255.0f;
}
uint16_t To565(float r, float g, float b) {
    const auto q = [](float c, int max) { return static_cast<uint32_t>(std::lround(std::min(1.0f, std::max(0.0f, c)) * max)); };
    return static_cast<uint16_t>(q(r, 31) << 11 | q(g, 63) << 5 | q(b, 31));
}
std::vector<uint16_t> BuildLut(const Choice& c) {
    std::vector<uint16_t> lut(65536);
    for (uint32_t v = 0; v < 65536; ++v) {
        float r, g, b;
        Rgb565(static_cast<uint16_t>(v), r, g, b);
        const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
        const float s = mx > 0.0f ? d / mx : 0.0f;
        if (s < 0.05f || mx <= 0.0f) { lut[v] = static_cast<uint16_t>(v); continue; }   // greys stay
        float nh = c.hue, ns = std::min(1.0f, s * c.saturation), nv = std::min(1.0f, mx * c.value);
        if (c.hue < 0.0f) { nh = 0.0f; ns = 0.0f; }
        const float hh = nh / 60.0f, f = hh - std::floor(hh);
        const float p = nv * (1 - ns), qv = nv * (1 - ns * f), t = nv * (1 - ns * (1 - f));
        float nr, ng, nb;
        switch (static_cast<int>(std::floor(hh)) % 6) {
            case 0: nr = nv; ng = t; nb = p; break;
            case 1: nr = qv; ng = nv; nb = p; break;
            case 2: nr = p; ng = nv; nb = t; break;
            case 3: nr = p; ng = qv; nb = nv; break;
            case 4: nr = t; ng = p; nb = nv; break;
            default: nr = nv; ng = p; nb = qv; break;
        }
        lut[v] = To565(nr, ng, nb);
    }
    return lut;
}
// DXT1 blocks through the endpoint table; the block keeps its mode (tools/recolor_frontend_scene.py shift_blocks)
void ShiftBlocks(uint8_t* data, uint32_t size, const std::vector<uint16_t>& lut) {
    for (uint32_t o = 0; o + 8 <= size; o += 8) {
        uint16_t c0, c1;
        uint32_t idx;
        std::memcpy(&c0, data + o, 2);
        std::memcpy(&c1, data + o + 2, 2);
        std::memcpy(&idx, data + o + 4, 4);
        uint16_t n0 = lut[c0], n1 = lut[c1];
        const bool greater = c0 > c1;
        const bool swap = (greater && n0 < n1) || (!greater && n0 > n1);
        if (swap) {
            const uint32_t hi = (idx >> 1) & 0x55555555u;
            idx = greater ? idx ^ 0x55555555u : idx ^ (~hi & 0x55555555u);
            std::swap(n0, n1);
        }
        if (greater && n0 == n1) {   // four colours collapsed to three: index 3 would turn transparent
            const uint32_t m3 = idx & (idx >> 1) & 0x55555555u;
            idx &= ~(m3 | (m3 << 1));
        }
        std::memcpy(data + o, &n0, 2);
        std::memcpy(data + o + 2, &n1, 2);
        std::memcpy(data + o + 4, &idx, 4);
    }
}
// DXT5: the colour half of each 16-byte block always decodes four colours, so only the endpoints move.
void ShiftBlocks5(uint8_t* data, uint32_t size, const std::vector<uint16_t>& lut) {
    for (uint32_t o = 0; o + 16 <= size; o += 16) {
        uint16_t c[2];
        std::memcpy(c, data + o + 8, 4);
        c[0] = lut[c[0]];
        c[1] = lut[c[1]];
        std::memcpy(data + o + 8, c, 4);
    }
}

int __cdecl OnUpload(int texture, int size, const uint8_t* data, int flag) {
    InterlockedIncrement(&uploads);
    if (data && size > 0) {
        uint32_t hashed[2] = {0, 0}, hashes[2] = {0, 0};
        for (size_t i = 0; i < kTargetCount; ++i) {
            const Target& t = kTargets[i];
            const std::vector<uint16_t>& lut = t.rig ? rigLut : visorLut;
            if (lut.empty() || (static_cast<uint32_t>(size) != t.total0 && static_cast<uint32_t>(size) != t.total1)) continue;
            const int slot = t.level0 == 32768 ? 1 : 0;
            if (hashed[slot] != t.level0) {
                InterlockedIncrement(&candidates);
                if (!TexModHash(data, t.level0, hashes[slot])) break;
                hashed[slot] = t.level0;
            }
            if (hashes[slot] != t.hash) continue;
            std::vector<uint8_t> copy(static_cast<size_t>(size));
            if (!CopyIn(copy.data(), data, static_cast<uint32_t>(size))) break;
            ShiftBlocks(copy.data(), static_cast<uint32_t>(size), lut);
            InterlockedIncrement(&recoloured[i]);
            return uploadHook.call<int>(texture, size, copy.data(), flag);
        }
        // blood: the CRC32 of the first 256 bytes first, the level 0 hash only when that fits
        uint32_t prefix = 0, level0 = 0, hash = 0;
        bool prefixDone = false;
        for (unsigned i = 0; !bloodLut.empty() && i < ds2fx::kBloodTextureCount; ++i) {
            const ds2fx::BloodTexture& t = ds2fx::kBloodTextures[i];
            if (static_cast<uint32_t>(size) != t.total) continue;
            if (!prefixDone) {
                prefixDone = true;
                InterlockedIncrement(&bloodCandidates);
                if (!TexModHash(data, 256, prefix)) break;
                prefix = ~prefix;
            }
            if (prefix != t.prefix) continue;
            if (level0 != t.level0) {
                if (!TexModHash(data, t.level0, hash)) break;
                level0 = t.level0;
                InterlockedIncrement(&bloodHashes);
            }
            if (hash != t.hash) continue;
            std::vector<uint8_t> copy(static_cast<size_t>(size));
            if (!CopyIn(copy.data(), data, static_cast<uint32_t>(size))) break;
            if (t.dxt5) ShiftBlocks5(copy.data(), static_cast<uint32_t>(size), bloodLut);
            else ShiftBlocks(copy.data(), static_cast<uint32_t>(size), bloodLut);
            InterlockedIncrement(&bloodRecoloured[i]);
            return uploadHook.call<int>(texture, size, copy.data(), flag);
        }
    }
    return uploadHook.call<int>(texture, size, data, flag);
}

const Choice* Pick(const char* text) {
    for (const Choice& c : kChoices)
        if (_stricmp(text, c.name) == 0) return &c;
    return nullptr;
}
void EarlyInstall(FILE* log, const std::string& ini) {
    char rig[32]{}, visor[32]{};
    GetPrivateProfileStringA("Visuals", "RigColor", "stock", rig, sizeof(rig), ini.c_str());
    GetPrivateProfileStringA("Visuals", "VisorColor", "stock", visor, sizeof(visor), ini.c_str());
    char blood[32]{};
    GetPrivateProfileStringA("Visuals", "BloodColor", "stock", blood, sizeof(blood), ini.c_str());
    rigChoice = Pick(rig);
    visorChoice = Pick(visor);
    bloodChoice = Pick(blood);
    if (!rigChoice && !visorChoice && !bloodChoice) {
        std::fprintf(log, "rig/visor colours: stock (RigColor=%s VisorColor=%s BloodColor=%s)\n", rig, visor, blood);
        return;
    }
    BuildCrc();
    if (rigChoice) rigLut = BuildLut(*rigChoice);
    if (visorChoice) visorLut = BuildLut(*visorChoice);
    if (bloodChoice) bloodLut = BuildLut(*bloodChoice);
    installed = hook_site::Install(log, "rig/visor colours TX_ReplaceTextureContents", base + kUploadRva, kUploadBytes, sizeof(kUploadBytes),
                                   reinterpret_cast<void*>(OnUpload), uploadHook);
    std::fprintf(log, "rig/visor colours %s: RIG %s, visor %s, blood textures %s (%u listed)\n", installed ? "on" : "NOT installed",
                 rigChoice ? rigChoice->name : "stock", visorChoice ? visorChoice->name : "stock", bloodChoice ? bloodChoice->name : "stock",
                 ds2fx::kBloodTextureCount);
}
void WriteDiagnostics(FILE* log) {
    if (!installed) return;
    static LONG last = -1;
    LONG sum = faults * 7 + bloodHashes * 3;
    for (size_t i = 0; i < kTargetCount; ++i) sum += recoloured[i] * static_cast<LONG>(i + 1);
    for (unsigned i = 0; i < ds2fx::kBloodTextureCount; ++i) sum += bloodRecoloured[i] * static_cast<LONG>(i + 11);
    if (sum == last) return;
    last = sum;
    std::fprintf(log, "rig/visor colours uploads=%ld candidates=%ld faults=%ld recoloured:", uploads, candidates, faults);
    for (size_t i = 0; i < kTargetCount; ++i) std::fprintf(log, " %s=%ld", kTargets[i].name, recoloured[i]);
    if (!bloodLut.empty()) {
        std::fprintf(log, " | blood candidates=%ld hashed=%ld recoloured:", bloodCandidates, bloodHashes);
        for (unsigned i = 0; i < ds2fx::kBloodTextureCount; ++i)
            if (bloodRecoloured[i]) std::fprintf(log, " %s=%ld", ds2fx::kBloodTextures[i].name, bloodRecoloured[i]);
    }
    std::fprintf(log, "\n");
}
}  // namespace rigcolor
