// Dead Space 2 United by Richard Hafer.
// The supply terminal's drawing code (runtime_coop_overlay.inc) against a real
// Direct3D 9 device, without the game: a pure device with the game's behaviour
// flags (0x54) draws the panel into 1080p, 1440p and 2160p frames that are saved
// as BMP files, and a non-pure device checks that every state the panel touches
// is back afterwards. usage: coop_overlay_preview.exe [output folder]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "../third_party/safetyhook/safetyhook.hpp"
#include "../src/hook_site.h"
#include "../src/coop_shop.h"

namespace {
uintptr_t base = 0x400000;
uint32_t fakeDevice = 0, fakeBackBuffer[2]{};
unsigned char fakeLost = 0;
// The three renderer globals the overlay reads; nothing else exists here.
bool Read(uintptr_t address, void* out, size_t bytes) {
    if (address == base + 0x1D1CDCC && bytes == 4) { std::memcpy(out, &fakeDevice, 4); return true; }
    if (address == base + 0x1D1CDEA && bytes == 1) { std::memcpy(out, &fakeLost, 1); return true; }
    if (address == base + 0x1D1C754 && bytes == 8) { std::memcpy(out, fakeBackBuffer, 8); return true; }
    return false;
}
namespace gate {
void Event(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}
}  // namespace gate
#include "../src/runtime_coop_overlay.inc"
}  // namespace

int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

coop_shop::View SampleView() {
    using namespace coop_shop;
    Item items[9];
    SetItem(items[0], Kind::Ammo, 0, "Plasma Cutter ammo", 300);
    std::snprintf(items[0].detail, sizeof(items[0].detail), "One ammo pack for the Plasma Cutter");
    SetItem(items[1], Kind::Ammo, 1, "Pulse Rifle ammo", 350);
    SetItem(items[2], Kind::MedSmall, -1, "Small med pack", 400);
    SetItem(items[3], Kind::MedMedium, -1, "Medium med pack", 900);
    SetItem(items[4], Kind::MedLarge, -1, "Large med pack", 1800);
    SetItem(items[5], Kind::StasisPack, -1, "Stasis pack", 500);
    SetItem(items[6], Kind::SupplyDrop, -1, "Supply drop", 1500);
    SetItem(items[7], Kind::Gun, 2, "Line Gun", 5000);
    Refuse(items[7], "NOT LOADED");
    SetItem(items[8], Kind::Gun, 3, "Javelin Gun with a very long label that will not fit the row", 6000);
    View v;
    std::memset(&v, 0, sizeof(v));
    std::snprintf(v.title, sizeof(v.title), "SUPPLY TERMINAL");
    char credits[24];
    FormatCredits(credits, sizeof(credits), 1250);
    std::snprintf(v.credits, sizeof(v.credits), "CREDITS %s", credits);
    WaveLine(v.wave, sizeof(v.wave), true, true, kSurvIntermission, 3, 14);
    v.tab = 0;
    BuildRows(items, 9, 1, 0, 10, 1250, v);
    std::snprintf(v.detail, sizeof(v.detail), "%s", "One ammo pack for the Pulse Rifle");
    std::snprintf(v.status, sizeof(v.status), "%s", "Not enough credits");
    v.statusKind = kStatusBad;
    std::snprintf(v.hint, sizeof(v.hint), "NUM 4/6 TAB   NUM 8/2 SELECT   NUM 5 BUY   NUM 0 CLOSE");
    return v;
}

bool SaveBmp(IDirect3DDevice9* dev, IDirect3DSurface9* target, UINT w, UINT h, const std::string& path) {
    IDirect3DSurface9* copy = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr))) return false;
    bool ok = SUCCEEDED(dev->GetRenderTargetData(target, copy));
    D3DLOCKED_RECT lr{};
    if (ok) ok = SUCCEEDED(copy->LockRect(&lr, nullptr, D3DLOCK_READONLY));
    if (ok) {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
            const uint32_t row = w * 3 + (4 - (w * 3) % 4) % 4, size = 54 + row * h;
            unsigned char header[54]{'B', 'M'};
            std::memcpy(header + 2, &size, 4);
            const uint32_t offset = 54, info = 40, planesBits = 1 | (24 << 16);
            const int32_t width = static_cast<int32_t>(w), height = static_cast<int32_t>(h);
            std::memcpy(header + 10, &offset, 4);
            std::memcpy(header + 14, &info, 4);
            std::memcpy(header + 18, &width, 4);
            std::memcpy(header + 22, &height, 4);
            std::memcpy(header + 26, &planesBits, 4);
            std::fwrite(header, 1, 54, f);
            unsigned char* line = new unsigned char[row]();
            for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
                const uint32_t* px = reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch);
                for (UINT x = 0; x < w; ++x) { line[x * 3] = px[x] & 0xFF; line[x * 3 + 1] = (px[x] >> 8) & 0xFF; line[x * 3 + 2] = (px[x] >> 16) & 0xFF; }
                std::fwrite(line, 1, row, f);
            }
            delete[] line;
            std::fclose(f);
        } else {
            ok = false;
        }
        copy->UnlockRect();
    }
    copy->Release();
    return ok;
}

// A frame the way the game ends one: something drawn, then the panel, then EndScene.
bool Frame(IDirect3DDevice9* dev, UINT w, UINT h, const std::string& path) {
    IDirect3DSurface9* target = nullptr;
    if (FAILED(dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr))) return false;
    dev->SetRenderTarget(0, target);
    dev->BeginScene();
    // A stand-in for the game's picture: a dark blue-grey gradient in bands.
    for (UINT band = 0; band < 16; ++band) {
        const D3DRECT r{0, static_cast<LONG>(band * h / 16), static_cast<LONG>(w), static_cast<LONG>((band + 1) * h / 16)};
        const DWORD c = 0x18 + band * 5;
        dev->Clear(1, &r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(c / 2, c * 3 / 4, c), 1.0f, 0);
    }
    const LONG drawn = overlay::framesDrawn;
    overlay::DrawGuarded(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dev)));
    dev->EndScene();
    const bool ok = overlay::framesDrawn == drawn + 1 && (path.empty() || SaveBmp(dev, target, w, h, path));
    target->Release();
    return ok;
}

IDirect3DDevice9* MakeDevice(IDirect3D9* d3d, HWND window, DWORD flags) {
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 360;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = nullptr;
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &pp, &dev);
    std::printf("CreateDevice flags %02X: %08X\n", static_cast<unsigned>(flags), static_cast<unsigned>(hr));
    return SUCCEEDED(hr) ? dev : nullptr;
}

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ShopOverlayPreview";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "preview", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) { std::printf("no window or no Direct3D 9\n"); return 2; }

    overlay::Publish(SampleView());
    // The game's device: hardware vertex processing, pure, multithreaded.
    IDirect3DDevice9* pure = MakeDevice(d3d, window, 0x54);
    Check(pure != nullptr, "a pure device");
    if (pure) {
        fakeDevice = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pure));
        const UINT sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {1280, 720}};
        for (const auto& s : sizes) {
            fakeBackBuffer[0] = s[0];
            fakeBackBuffer[1] = s[1];
            char name[64];
            std::snprintf(name, sizeof(name), "\\shop_overlay_%ux%u.bmp", s[0], s[1]);
            Check(Frame(pure, s[0], s[1], out + name), name);
        }
        Check(overlay::blockFailures == 0, "CreateStateBlock(D3DSBT_ALL) works on the pure device");
        Check(overlay::fontBuilds >= 4, "one font per height");
        // Steady state at 1440p: the font exists, only the panel is drawn.
        fakeBackBuffer[0] = 2560;
        fakeBackBuffer[1] = 1440;
        Frame(pure, 2560, 1440, "");
        const LONG builds = overlay::fontBuilds, frames0 = overlay::framesDrawn, micros0 = overlay::drawMicrosTotal;
        for (int i = 0; i < 60; ++i) Frame(pure, 2560, 1440, "");
        const LONG frames = overlay::framesDrawn - frames0;
        std::printf("steady state 1440p: %ld frames, %ld us per frame (state block, states, %ld draw calls in all)\n", frames,
                    frames ? (overlay::drawMicrosTotal - micros0) / frames : 0L, overlay::drawCalls);
        Check(overlay::fontBuilds == builds && frames == 60, "no font rebuild at the same height");
        // A lost device is not drawn on.
        fakeLost = 1;
        const LONG skips = overlay::lostSkips, drawn = overlay::framesDrawn;
        pure->BeginScene();
        overlay::DrawGuarded(fakeDevice);
        pure->EndScene();
        Check(overlay::lostSkips == skips + 1 && overlay::framesDrawn == drawn, "the lost flag skips the frame");
        fakeLost = 0;
        // Another device than the renderer's global is refused.
        const LONG mismatch = overlay::deviceMismatch;
        overlay::DrawGuarded(fakeDevice + 4);
        Check(overlay::deviceMismatch == mismatch + 1, "a foreign device is refused");
        overlay::ReleaseFont();
        pure->Release();
    }

    // State restore, checked on a device that answers Get*.
    IDirect3DDevice9* plain = MakeDevice(d3d, window, 0x44);
    Check(plain != nullptr, "a non-pure device");
    if (plain) {
        fakeDevice = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(plain));
        IDirect3DSurface9* target = nullptr;
        plain->CreateRenderTarget(1920, 1080, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr);
        plain->SetRenderTarget(0, target);
        IDirect3DTexture9* gameTexture = nullptr;
        plain->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &gameTexture, nullptr);
        // Odd values for everything the panel sets.
        const DWORD states[][2] = {{D3DRS_ZENABLE, 1}, {D3DRS_ZWRITEENABLE, 1}, {D3DRS_ALPHATESTENABLE, 1}, {D3DRS_CULLMODE, D3DCULL_CW}, {D3DRS_FILLMODE, D3DFILL_WIREFRAME},
                                   {D3DRS_ALPHABLENDENABLE, 0}, {D3DRS_SRCBLEND, D3DBLEND_ONE}, {D3DRS_DESTBLEND, D3DBLEND_ZERO}, {D3DRS_BLENDOP, D3DBLENDOP_MAX},
                                   {D3DRS_SEPARATEALPHABLENDENABLE, 1}, {D3DRS_FOGENABLE, 1}, {D3DRS_STENCILENABLE, 1}, {D3DRS_LIGHTING, 1},
                                   {D3DRS_SCISSORTESTENABLE, 1}, {D3DRS_COLORWRITEENABLE, 0x7}, {D3DRS_SRGBWRITEENABLE, 1}, {D3DRS_CLIPPLANEENABLE, 3},
                                   {D3DRS_ADAPTIVETESS_X, 0x3F800000}};
        for (const auto& s : states) plain->SetRenderState(static_cast<D3DRENDERSTATETYPE>(s[0]), s[1]);
        plain->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD);
        plain->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        plain->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        plain->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        plain->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 1);
        plain->SetTexture(0, gameTexture);
        plain->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL);
        const D3DVIEWPORT9 vp{100, 50, 800, 600, 0.25f, 0.75f};
        plain->SetViewport(&vp);
        fakeBackBuffer[0] = 1920;
        fakeBackBuffer[1] = 1080;
        plain->BeginScene();
        const LONG drawn = overlay::framesDrawn;
        overlay::DrawGuarded(fakeDevice);
        plain->EndScene();
        Check(overlay::framesDrawn == drawn + 1, "drawn on the plain device");
        for (const auto& s : states) {
            DWORD v = 0;
            plain->GetRenderState(static_cast<D3DRENDERSTATETYPE>(s[0]), &v);
            if (v != s[1]) { std::printf("render state %lu: %08lX, wanted %08lX\n", s[0], v, s[1]); Check(false, "a render state is back"); }
        }
        DWORD v = 0;
        plain->GetTextureStageState(0, D3DTSS_COLOROP, &v); Check(v == D3DTOP_ADD, "stage 0 colour op is back");
        plain->GetTextureStageState(1, D3DTSS_COLOROP, &v); Check(v == D3DTOP_MODULATE, "stage 1 colour op is back");
        plain->GetSamplerState(0, D3DSAMP_MAGFILTER, &v); Check(v == D3DTEXF_LINEAR, "sampler filter is back");
        plain->GetSamplerState(0, D3DSAMP_ADDRESSU, &v); Check(v == D3DTADDRESS_WRAP, "sampler address is back");
        plain->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &v); Check(v == 1, "sampler sRGB is back");
        IDirect3DBaseTexture9* bound = nullptr;
        plain->GetTexture(0, &bound);
        Check(bound == gameTexture, "the game's texture is bound again");
        if (bound) bound->Release();
        plain->GetFVF(&v); Check(v == (D3DFVF_XYZ | D3DFVF_NORMAL), "the FVF is back");
        D3DVIEWPORT9 now{};
        plain->GetViewport(&now);
        Check(now.X == 100 && now.Y == 50 && now.Width == 800 && now.Height == 600 && now.MinZ == 0.25f && now.MaxZ == 0.75f, "the viewport is back");
        IDirect3DSurface9* still = nullptr;
        plain->GetRenderTarget(0, &still);
        Check(still == target, "the render target is untouched");
        if (still) still->Release();
        overlay::ReleaseFont();
        gameTexture->Release();
        target->Release();
        plain->Release();
    }
    d3d->Release();
    DestroyWindow(window);
    std::printf("frames drawn %ld, draw calls %ld, font builds %ld, state block failures %ld, avg %ld us, max %ld us\n", overlay::framesDrawn, overlay::drawCalls,
                overlay::fontBuilds, overlay::blockFailures, overlay::framesDrawn ? overlay::drawMicrosTotal / overlay::framesDrawn : 0L, overlay::drawMicrosMax);
    std::printf(failures ? "%d check(s) FAILED\n" : "all overlay preview checks passed\n", failures);
    return failures ? 1 : 0;
}
