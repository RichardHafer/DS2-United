// Dead Space 2 United by Richard Hafer.
// The supply terminal's panel, drawn with Direct3D 9 inside the game's own
// frame (runtime_coop_shop.inc fills the View, coop_shop.h has the layout).
//
// Where: the renderer's EndFrame (PC 0x013CAF60, cdecl, plain ret, called only
// by the render command 0x00770A80) returns at once unless the in-scene flag
// [0x0211CDC3] is up, which its BeginScene wrapper (0x013C9C00) sets after
// IDirect3DDevice9::BeginScene. It then unbinds both shaders, 16 textures, the
// indices and 16 streams, puts the default vertex declaration back and calls
// EndScene through [[esi]+0xA8], esi = [0x0211CDCC], the device that
// IDirect3D9::CreateDevice (0x013CBE89, ppDevice = 0x0211CDCC) returned. The
// mid hook sits on that call (0x013CB0F1): the scene is open, the game is done
// with it and nothing of its own is bound. The two Present calls (0x013C9F55,
// 0x013CAD60) are spliced by the exe's packer; Discord, RivaTuner and Steam hook
// Present and Reset inside d3d9.dll, so neither they nor the device vtable are
// touched here.
//
// The device is a pure device (BehaviorFlags [0x01F639F0] = 0x54): no Get* for
// state. A D3DSBT_ALL state block is created right before drawing, applied and
// released right after; the game's own render state cache (applied values at
// 0x0211E1A0) therefore stays true, and nothing of ours outlives the frame,
// which a Reset would refuse. The font is a managed texture, which a Reset
// keeps. Nothing is drawn while TestCooperativeLevel fails or the renderer's
// lost flag [0x0211CDEA] is set. Drawing may happen on the render thread: the
// View is handed over under a lock.
#pragma comment(lib, "gdi32.lib")
namespace overlay {
constexpr uint32_t kEndFrameRva = 0xFCAF60, kEndSceneCallRva = 0xFCB0F1, kDeviceLoad = 0x1E, kEndFrameCallerRva = 0x370A80;
constexpr uint32_t kDeviceGlobalRva = 0x1D1CDCC, kInSceneRva = 0x1D1CDC3, kLostFlagRva = 0x1D1CDEA, kPresentParamsRva = 0x1D1C754;

// COM slots (IDirect3DDevice9 and friends), each checked against the game's own calls.
enum : unsigned {
    kRelease = 2,
    kDevTestCooperativeLevel = 3, kDevGetBackBuffer = 18, kDevCreateTexture = 23, kDevSetRenderTarget = 37, kDevGetRenderTarget = 38,
    kDevSetDepthStencil = 39, kDevGetDepthStencil = 40, kDevSetViewport = 47, kDevSetRenderState = 57,
    kDevCreateStateBlock = 59, kDevSetTexture = 65, kDevSetTextureStageState = 67, kDevSetSamplerState = 69, kDevDrawPrimitiveUP = 83,
    kDevSetFVF = 89, kDevSetVertexShader = 92, kDevSetPixelShader = 107,
    kSurfaceGetDesc = 12, kTextureLockRect = 19, kTextureUnlockRect = 20, kBlockApply = 5,
};
template <typename R, typename... A>
R VCall(void* object, unsigned slot, A... args) {
    using Fn = R(__stdcall*)(void*, A...);
    return (*reinterpret_cast<Fn**>(object))[slot](object, args...);
}
struct SurfaceDesc { uint32_t format, type, usage, pool, multiSample, multiSampleQuality, width, height; };
struct LockedRect { int pitch; void* bits; };
struct Viewport { uint32_t x, y, width, height; float minZ, maxZ; };
struct Vertex { float x, y, z, rhw; uint32_t color; float u, v; };
constexpr uint32_t kFvf = 0x004 | 0x040 | 0x100;   // XYZRHW | DIFFUSE | TEX1

// --- configuration, state, counters ---------------------------------------------
bool wanted = false, ready = false;
int scalePercent = 100;
char fontFaces[4][48] = {"Bahnschrift SemiBold", "Bahnschrift", "Segoe UI Semibold", "Arial"};
safetyhook::MidHook endSceneHook;
volatile LONG visible = 0, disabled = 0;
// Players saw the overlay vanish for the rest of a session (reported 02.11.): three draw
// faults or three failed font builds anywhere in a session switched it off for good.
// Now only three within 10 s pause it, and it tries again 30 s later.
constexpr DWORD kTroubleWindowMs = 10000, kRetryAfterMs = 30000;
DWORD disabledAt = 0, faultWindowStart = 0, fontWindowStart = 0;
LONG faultsInWindow = 0, fontFailuresInWindow = 0;
volatile LONG pauses = 0, retries = 0;
const char* disabledWhy = "";
void PauseDrawing(const char* why) {
    disabledAt = GetTickCount();
    if (!disabledAt) disabledAt = 1;
    disabledWhy = why;
    InterlockedIncrement(&pauses);
    InterlockedExchange(&disabled, 1);
}
volatile LONG cameraHidden = 0;   // the trailer camera hides the status line and the marks (runtime_coop_camtools.inc)
SRWLOCK viewLock = SRWLOCK_INIT;
coop_shop::View shared{};
volatile LONG sharedSerial = 0;
volatile LONG hookCalls = 0, framesDrawn = 0, hiddenFrames = 0, lostSkips = 0, resets = 0, deviceMismatch = 0, noTarget = 0, smallTarget = 0, blockFailures = 0,
              fontBuilds = 0, fontFailures = 0, drawFaults = 0, deviceChanges = 0, drawCalls = 0, drawMicrosTotal = 0, drawMicrosMax = 0, eventLines = 0;
volatile LONG targetW = 0, targetH = 0, targetFormat = 0;
// Frames the panel itself was drawn, frames that ended on a target other than the back buffer,
// and how many of those were drawn into the back buffer instead ([Shop] OverlayBackBuffer).
volatile LONG panelFrames = 0, offscreenFrames = 0, backBufferDraws = 0, backBufferFailures = 0, offscreenNotes = 0;
bool backBufferFix = false;
void Line(const char* format, ...) {
    if (InterlockedIncrement(&eventLines) > 40) return;
    char buffer[300];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    gate::Event("shop overlay: %s", buffer);
}

// The game thread hands a View over; the drawing side copies it when the serial moves.
void Publish(const coop_shop::View& view) {
    AcquireSRWLockExclusive(&viewLock);
    shared = view;
    ReleaseSRWLockExclusive(&viewLock);
    InterlockedIncrement(&sharedSerial);
    InterlockedExchange(&visible, 1);
}
void Hide() { InterlockedExchange(&visible, 0); }

// --- render thread ----------------------------------------------------------------
coop_shop::View view{};
LONG viewSerial = -1;
bool wasLost = false, loggedTarget = false;
void* lastDevice = nullptr;

// The font: printable ASCII from a GDI font into one managed A8R8G8B8 texture,
// one cell per glyph, and a white cell for the panel's solid parts.
struct Font {
    void* texture = nullptr;
    void* device = nullptr;
    int px = 0, texW = 0, texH = 0, cellW = 0, cellH = 0, cols = 0, pad = 0;
    float whiteU = 0.0f, whiteV = 0.0f;
    coop_shop::Metrics metrics;
    char face[48] = "";
};
Font font;
void ReleaseFont() {
    if (font.texture) VCall<ULONG>(font.texture, kRelease);
    font = Font{};
}
// The first face of the list that GDI really has (it substitutes silently otherwise).
bool PickFace(HDC dc, int px, HFONT& out, HGDIOBJ& previous, char* face, size_t faceSize) {
    for (const char* want : fontFaces) {
        if (!want[0]) continue;
        HFONT f = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, want);
        if (!f) continue;
        HGDIOBJ old = SelectObject(dc, f);
        char got[64]{};
        GetTextFaceA(dc, sizeof(got), got);
        if (_stricmp(got, want) == 0) {
            out = f;
            previous = old;
            std::snprintf(face, faceSize, "%s", got);
            return true;
        }
        SelectObject(dc, old);
        DeleteObject(f);
    }
    return false;
}
bool BuildFont(void* device, int px) {
    ReleaseFont();
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT gdiFont = nullptr;
    HGDIOBJ oldFont = nullptr;
    char face[48] = "";
    if (!PickFace(dc, px, gdiFont, oldFont, face, sizeof(face))) {
        DeleteDC(dc);
        Line("no font face of the list is installed");
        return false;
    }
    TEXTMETRICA tm{};
    GetTextMetricsA(dc, &tm);
    Font f;
    f.px = px;
    f.metrics.lineHeight = tm.tmHeight;
    int widest = 1;
    for (int i = 0; i < 95; ++i) {
        const char c = static_cast<char>(32 + i);
        SIZE s{};
        GetTextExtentPoint32A(dc, &c, 1, &s);
        f.metrics.advance[i] = s.cx;
        if (s.cx > widest) widest = s.cx;
    }
    f.pad = px / 5 < 2 ? 2 : px / 5;
    f.cellW = widest + 2 * f.pad;
    f.cellH = tm.tmHeight + 2;
    // 95 glyphs and the white cell in the smallest power-of-two square-ish atlas.
    for (f.texW = 256; f.texW <= 4096; f.texW *= 2) {
        f.cols = f.texW / f.cellW;
        if (f.cols < 1) continue;
        const int rows = (96 + f.cols - 1) / f.cols;
        f.texH = 64;
        while (f.texH < rows * f.cellH) f.texH *= 2;
        if (f.texH <= f.texW) break;
    }
    if (f.texW > 4096 || f.cols < 1) {
        SelectObject(dc, oldFont);
        DeleteObject(gdiFont);
        DeleteDC(dc);
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = f.texW;
    bi.bmiHeader.biHeight = -f.texH;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = bitmap && bits;
    if (ok) {
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        std::memset(bits, 0, static_cast<size_t>(f.texW) * f.texH * 4);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, TA_TOP | TA_LEFT);
        for (int i = 0; i < 95; ++i) {
            const char c = static_cast<char>(32 + i);
            TextOutA(dc, (i % f.cols) * f.cellW + f.pad, (i / f.cols) * f.cellH + 1, &c, 1);
        }
        GdiFlush();
        uint32_t* pixels = static_cast<uint32_t*>(bits);
        const int wx = (95 % f.cols) * f.cellW + 2, wy = (95 / f.cols) * f.cellH + 2;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) pixels[(wy + y) * f.texW + wx + x] = 0xFFFFFFFFu;
        f.whiteU = (wx + 2.0f) / f.texW;
        f.whiteV = (wy + 2.0f) / f.texH;
        void* texture = nullptr;
        // CreateTexture(w, h, levels 1, usage 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)
        const HRESULT created = VCall<HRESULT>(device, kDevCreateTexture, static_cast<UINT>(f.texW), static_cast<UINT>(f.texH), 1u, 0ul, 21u, 1u, &texture,
                                               static_cast<HANDLE*>(nullptr));
        LockedRect locked{};
        ok = created == 0 && texture && VCall<HRESULT>(texture, kTextureLockRect, 0u, &locked, static_cast<const RECT*>(nullptr), 0ul) == 0 && locked.bits;
        if (ok) {
            for (int y = 0; y < f.texH; ++y) {
                const uint32_t* src = pixels + static_cast<size_t>(y) * f.texW;
                uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<unsigned char*>(locked.bits) + static_cast<size_t>(y) * locked.pitch);
                for (int x = 0; x < f.texW; ++x) {
                    const uint32_t p = src[x];
                    uint32_t a = p & 0xFF;
                    if (((p >> 8) & 0xFF) > a) a = (p >> 8) & 0xFF;
                    if (((p >> 16) & 0xFF) > a) a = (p >> 16) & 0xFF;
                    dst[x] = (a << 24) | 0x00FFFFFFu;
                }
            }
            VCall<HRESULT>(texture, kTextureUnlockRect, 0u);
            f.texture = texture;
            f.device = device;
        } else {
            Line("font texture %dx%d failed: CreateTexture %08X", f.texW, f.texH, static_cast<unsigned>(created));
            if (texture) VCall<ULONG>(texture, kRelease);
        }
        SelectObject(dc, oldBitmap);
    }
    SelectObject(dc, oldFont);
    if (bitmap) DeleteObject(bitmap);
    DeleteObject(gdiFont);
    DeleteDC(dc);
    if (!ok) return false;
    std::snprintf(f.face, sizeof(f.face), "%s", face);
    font = f;
    InterlockedIncrement(&fontBuilds);
    Line("font '%s' %d px: atlas %dx%d, cell %dx%d, line %d (managed texture, kept across a Reset)", font.face, px, font.texW, font.texH, font.cellW, font.cellH,
         font.metrics.lineHeight);
    return true;
}

// --- quads ---------------------------------------------------------------------------
constexpr int kMaxVertices = 6 * 600;
Vertex vertices[kMaxVertices];
int vertexCount = 0;
void* drawDevice = nullptr;
void Flush() {
    if (!vertexCount || !drawDevice) return;
    // DrawPrimitiveUP(D3DPT_TRIANGLELIST, primitives, data, stride)
    VCall<HRESULT>(drawDevice, kDevDrawPrimitiveUP, 4u, static_cast<UINT>(vertexCount / 3), static_cast<const void*>(vertices), static_cast<UINT>(sizeof(Vertex)));
    InterlockedIncrement(&drawCalls);
    vertexCount = 0;
}
void Quad(float x0, float y0, float x1, float y1, uint32_t color, float u0, float v0, float u1, float v1) {
    if (vertexCount + 6 > kMaxVertices) Flush();
    // Half a pixel back: texel centres land on pixel centres.
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
    const Vertex a{x0, y0, 0.0f, 1.0f, color, u0, v0}, b{x1, y0, 0.0f, 1.0f, color, u1, v0}, c{x0, y1, 0.0f, 1.0f, color, u0, v1}, d{x1, y1, 0.0f, 1.0f, color, u1, v1};
    Vertex* v = vertices + vertexCount;
    v[0] = a; v[1] = b; v[2] = c; v[3] = c; v[4] = b; v[5] = d;
    vertexCount += 6;
}
void Tri(float ax, float ay, float bx, float by, float cx, float cy, uint32_t color) {
    if (vertexCount + 3 > kMaxVertices) Flush();
    const float u = font.whiteU, v = font.whiteV;
    Vertex* p = vertices + vertexCount;
    p[0] = Vertex{ax - 0.5f, ay - 0.5f, 0.0f, 1.0f, color, u, v};
    p[1] = Vertex{bx - 0.5f, by - 0.5f, 0.0f, 1.0f, color, u, v};
    p[2] = Vertex{cx - 0.5f, cy - 0.5f, 0.0f, 1.0f, color, u, v};
    vertexCount += 3;
}
void Rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    Quad(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + w), static_cast<float>(y + h), color, font.whiteU, font.whiteV, font.whiteU, font.whiteV);
}
int TextWidth(const char* text) { return coop_shop::Measure(font.metrics, text); }
void Text(int x, int y, const char* text, uint32_t color) {
    const float tw = static_cast<float>(font.texW), th = static_cast<float>(font.texH);
    for (; *text; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        if (c < 32 || c > 126) c = '?';
        const int i = c - 32;
        if (c != ' ') {
            const int cx = (i % font.cols) * font.cellW, cy = (i / font.cols) * font.cellH;
            const float x0 = static_cast<float>(x - font.pad), y0 = static_cast<float>(y - 1);
            Quad(x0, y0, x0 + font.cellW, y0 + font.cellH, color, cx / tw, cy / th, (cx + font.cellW) / tw, (cy + font.cellH) / th);
        }
        x += font.metrics.advance[i];
    }
}
// The same glyphs, scale times as large (the banner's title).
void TextScaled(int x, int y, const char* text, uint32_t color, int scale) {
    const float tw = static_cast<float>(font.texW), th = static_cast<float>(font.texH);
    for (; *text; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        if (c < 32 || c > 126) c = '?';
        const int i = c - 32;
        if (c != ' ') {
            const int cx = (i % font.cols) * font.cellW, cy = (i / font.cols) * font.cellH;
            const float x0 = static_cast<float>(x - font.pad * scale), y0 = static_cast<float>(y - scale);
            Quad(x0, y0, x0 + font.cellW * scale, y0 + font.cellH * scale, color, cx / tw, cy / th, (cx + font.cellW) / tw, (cy + font.cellH) / th);
        }
        x += font.metrics.advance[i] * scale;
    }
}
void TextFit(int x, int y, const char* text, int maxWidth, uint32_t color) {
    char cut[128];
    coop_shop::FitText(font.metrics, text, maxWidth, cut, sizeof(cut));
    Text(x, y, cut, color);
}

// RIG colours: teal glass, cyan frame and text.
constexpr uint32_t kPanel = 0xC8040D11, kHeaderBand = 0x2A1FA8C8, kScan = 0x0D8FE8FF, kFrame = 0xFF39C8E0, kFrameDim = 0x7039C8E0;
constexpr uint32_t kTitle = 0xFFA8F4FF, kText = 0xFFD6F7FF, kTextDim = 0xFF86BCCB, kPrice = 0xFF8FEAFF;
constexpr uint32_t kPoorLabel = 0x9AA9C7CF, kPoorPrice = 0xD8FF7466, kUnavailable = 0x787F98A0;
constexpr uint32_t kSelectBar = 0x4438C8E0, kSelectMark = 0xFF6FF2FF, kTabActive = 0x4C38C8E0, kTabText = 0xB88FD8E8;
constexpr uint32_t kGood = 0xFF8CFFB4, kBad = 0xFFFF7A68, kHint = 0xA87FD0E0;

// Where the last drawn panel put its tabs, rows and scroll arrows, in render
// target pixels, for the mouse (the game thread maps the pointer through it).
struct HitMap {
    int valid = 0, w = 0, h = 0;
    int panelX0 = 0, panelY0 = 0, panelX1 = 0, panelY1 = 0;
    int tabCount = 0, tabX0[8]{}, tabX1[8]{}, tabY0 = 0, tabY1 = 0;
    int rowsX0 = 0, rowsX1 = 0, rowsY0 = 0, rowLine = 1, rowsShown = 0, rowFirst = 0, arrowX0 = 0;
};
SRWLOCK hitLock = SRWLOCK_INIT;
HitMap hitShared{};
void PublishHits(const HitMap& m) {
    AcquireSRWLockExclusive(&hitLock);
    hitShared = m;
    ReleaseSRWLockExclusive(&hitLock);
}
HitMap Hits() {
    AcquireSRWLockShared(&hitLock);
    const HitMap m = hitShared;
    ReleaseSRWLockShared(&hitLock);
    return m;
}
// The panel's mouse pointer, set by the game thread in render target pixels.
volatile LONG pointerOn = 0, pointerX = 0, pointerY = 0;
void SetPointer(bool on, int x, int y) {
    InterlockedExchange(&pointerX, x);
    InterlockedExchange(&pointerY, y);
    InterlockedExchange(&pointerOn, on ? 1 : 0);
}

void DrawView(int W, int H) {
    const int px = font.px;
    const coop_shop::Layout l = coop_shop::PanelLayout(W, H, px, font.metrics.lineHeight, view.rowCapacity);
    HitMap hits;
    hits.valid = 1;
    hits.w = W;
    hits.h = H;
    hits.panelX0 = l.x;
    hits.panelY0 = l.y;
    hits.panelX1 = l.x + l.w;
    hits.panelY1 = l.y + l.h;
    const int t = px / 16 < 1 ? 1 : px / 16;
    const int left = l.x + l.pad, right = l.x + l.w - l.pad;
    // glass, scan lines, header band
    Rect(l.x, l.y, l.w, l.h, kPanel);
    const int step = px / 8 < 3 ? 3 : px / 8;
    for (int y = l.y + step; y < l.y + l.h; y += step) Rect(l.x, y, l.w, 1, kScan);
    Rect(l.x, l.y, l.w, l.tabsY - l.y - l.pad / 4, kHeaderBand);
    // frame and corner brackets
    Rect(l.x, l.y, l.w, t, kFrameDim);
    Rect(l.x, l.y + l.h - t, l.w, t, kFrameDim);
    Rect(l.x, l.y, t, l.h, kFrameDim);
    Rect(l.x + l.w - t, l.y, t, l.h, kFrameDim);
    const int arm = px, thick = 2 * t;
    Rect(l.x - t, l.y - t, arm, thick, kFrame); Rect(l.x - t, l.y - t, thick, arm, kFrame);
    Rect(l.x + l.w - arm + t, l.y - t, arm, thick, kFrame); Rect(l.x + l.w - thick + t, l.y - t, thick, arm, kFrame);
    Rect(l.x - t, l.y + l.h - thick + t, arm, thick, kFrame); Rect(l.x - t, l.y + l.h - arm + t, thick, arm, kFrame);
    Rect(l.x + l.w - arm + t, l.y + l.h - thick + t, arm, thick, kFrame); Rect(l.x + l.w - thick + t, l.y + l.h - arm + t, thick, arm, kFrame);
    // title and credits
    const int creditsW = TextWidth(view.credits);
    Text(right - creditsW, l.titleY, view.credits, kPrice);
    TextFit(left, l.titleY, view.title, right - creditsW - px - left, kTitle);
    TextFit(left, l.waveY, view.wave, right - left, kTextDim);
    Rect(left, l.tabsY - l.pad / 4 - t, right - left, t, kFrameDim);
    // tabs, spread over the width
    const bool ownTabs = view.tabCount > 0 && view.tabCount <= 8;
    const int tabs = ownTabs ? view.tabCount : coop_shop::kTabs;
    auto tabName = [&](int i) { return ownTabs ? static_cast<const char*>(view.tabNames[i]) : coop_shop::TabName(i); };
    int names = 0;
    for (int i = 0; i < tabs; ++i) names += TextWidth(tabName(i));
    int gap = (right - left - names) / tabs;
    if (gap > px) gap = px;
    if (gap < px / 4) gap = px / 4;
    int x = left + gap / 2;
    hits.tabCount = tabs < 8 ? tabs : 8;
    hits.tabY0 = l.tabsY - px / 8;
    hits.tabY1 = l.tabsY - px / 8 + l.line;
    for (int i = 0; i < tabs; ++i) {
        const char* name = tabName(i);
        const int w = TextWidth(name);
        if (i < 8) { hits.tabX0[i] = x - gap / 2; hits.tabX1[i] = x + w + gap / 2; }
        if (i == view.tab) {
            Rect(x - gap / 2, l.tabsY - px / 8, w + gap, l.line, kTabActive);
            Rect(x - gap / 2, l.tabsY - px / 8 + l.line - thick, w + gap, thick, kFrame);
        }
        Text(x, l.tabsY, name, i == view.tab ? kTitle : kTabText);
        x += w + gap;
    }
    Rect(left, l.rowsY - l.pad / 4 - t, right - left, t, kFrameDim);
    // rows: the overlay may hold fewer than the shop sent; the selection stays in view
    int first = 0;
    const int shown = view.rowCount < l.rows ? view.rowCount : l.rows;
    for (int i = 0; i < view.rowCount; ++i)
        if ((view.rows[i].style & coop_shop::kRowSelected) && i >= shown) first = i - shown + 1;
    for (int k = 0; k < shown; ++k) {
        const coop_shop::Row& r = view.rows[first + k];
        const int y = l.rowsY + k * l.line;
        const bool selected = (r.style & coop_shop::kRowSelected) != 0;
        const int style = r.style & 0x7F;
        if (selected) {
            Rect(left - l.pad / 2, y - px / 8, right - left + l.pad, l.line, kSelectBar);
            Rect(left - l.pad / 2, y - px / 8, thick, l.line, kSelectMark);
        }
        const uint32_t labelColor = style == coop_shop::kRowUnavailable ? kUnavailable : style == coop_shop::kRowTooPoor ? kPoorLabel : selected ? kTitle : kText;
        const uint32_t priceColor = style == coop_shop::kRowUnavailable ? kUnavailable : style == coop_shop::kRowTooPoor ? kPoorPrice : kPrice;
        const int priceW = TextWidth(r.right);
        Text(right - priceW, y, r.right, priceColor);
        TextFit(left + px / 2, y, r.label, right - priceW - px - (left + px / 2), labelColor);
    }
    if (view.rowCount == 0) Text(left + px / 2, l.rowsY, "Nothing on offer here", kUnavailable);
    if (view.above || first > 0) Text(l.x + l.w - l.pad / 2 - TextWidth("^") / 2, l.rowsY, "^", kTextDim);
    if (view.below || first + shown < view.rowCount) Text(l.x + l.w - l.pad / 2 - TextWidth("v") / 2, l.rowsY + (shown - 1) * l.line, "v", kTextDim);
    hits.rowsX0 = left - l.pad / 2;
    hits.rowsX1 = right + l.pad / 2;
    hits.rowsY0 = l.rowsY - px / 8;
    hits.rowLine = l.line > 0 ? l.line : 1;
    hits.rowsShown = shown;
    hits.rowFirst = first;
    hits.arrowX0 = l.x + l.w - l.pad;
    PublishHits(hits);
    Rect(left, l.detailY - l.pad / 4 - t, right - left, t, kFrameDim);
    TextFit(left, l.detailY, view.detail, right - left, kTextDim);
    const uint32_t statusColor = view.statusKind == coop_shop::kStatusGood ? kGood : view.statusKind == coop_shop::kStatusBad ? kBad : kText;
    TextFit(left, l.statusY, view.status, right - left, statusColor);
    TextFit(left, l.hintY, view.hint, right - left, kHint);
}

// A band across the middle of the screen (the respawn countdown). It is drawn
// over everything the game draws, death camera and blood screen included, where
// the RIG notices do not show.
struct Banner { char title[48]; char line[96]; DWORD until; };
SRWLOCK bannerLock = SRWLOCK_INIT;
Banner bannerShared{}, bannerView{};
volatile LONG bannerFrames = 0, bannerShows = 0;
void ShowBanner(const char* title, const char* line, DWORD ms) {
    AcquireSRWLockExclusive(&bannerLock);
    std::snprintf(bannerShared.title, sizeof(bannerShared.title), "%s", title ? title : "");
    std::snprintf(bannerShared.line, sizeof(bannerShared.line), "%s", line ? line : "");
    bannerShared.until = GetTickCount() + ms;
    if (!bannerShared.until) bannerShared.until = 1;
    ReleaseSRWLockExclusive(&bannerLock);
    InterlockedIncrement(&bannerShows);
}
void HideBanner() {
    AcquireSRWLockExclusive(&bannerLock);
    bannerShared.until = 0;
    ReleaseSRWLockExclusive(&bannerLock);
}
// Render thread: takes the current banner; true while it is up.
bool TakeBanner() {
    AcquireSRWLockShared(&bannerLock);
    bannerView = bannerShared;
    ReleaseSRWLockShared(&bannerLock);
    return bannerView.until && static_cast<LONG>(bannerView.until - GetTickCount()) > 0;
}
constexpr uint32_t kBannerBand = 0xB4000000, kBannerTitle = 0xFFFF5A48;
void DrawBanner(int W, int H) {
    const int px = font.px, lh = font.metrics.lineHeight, t = px / 16 < 1 ? 1 : px / 16;
    const int titleW = TextWidth(bannerView.title) * 2, lineW = TextWidth(bannerView.line);
    const int bandH = lh * 2 + lh + px, y = H * 3 / 8;
    Rect(0, y, W, bandH, kBannerBand);
    Rect(0, y, W, t, kFrameDim);
    Rect(0, y + bandH - t, W, t, kFrameDim);
    TextScaled((W - titleW) / 2, y + px / 3, bannerView.title, kBannerTitle, 2);
    Text((W - lineW) / 2, y + px / 3 + lh * 2, bannerView.line, kText);
    InterlockedIncrement(&bannerFrames);
}

// A hint card low in the middle of the screen, menus included: what happened (the
// button's or the notice's own words), why, and the fix. A refused join or launch says
// more than its button label can hold ([Notice] OverlayHints).
struct HintCard { char what[96]; char why[240]; char fix[200]; DWORD until; bool info; };
SRWLOCK hintLock = SRWLOCK_INIT;
HintCard hintShared{}, hintView{};
bool hintsOn = true;
DWORD hintMaxMs = 5000;   // [Notice] OverlayHintSeconds: a hint card stays at most this long (02.11.: 15 s was too long)
volatile LONG hintShows = 0, hintFrames = 0, hintLines = 0;
void ShowHint(const char* what, const char* why, const char* fix, DWORD ms) {
    if (InterlockedIncrement(&hintLines) <= 120) Line("hint: %s | why: %s | fix: %s", what ? what : "", why ? why : "", fix ? fix : "");
    if (!hintsOn) return;
    AcquireSRWLockExclusive(&hintLock);
    std::snprintf(hintShared.what, sizeof(hintShared.what), "%s", what ? what : "");
    std::snprintf(hintShared.why, sizeof(hintShared.why), "%s", why ? why : "");
    std::snprintf(hintShared.fix, sizeof(hintShared.fix), "%s", fix ? fix : "");
    hintShared.until = GetTickCount() + (hintMaxMs && ms > hintMaxMs ? hintMaxMs : ms);
    if (!hintShared.until) hintShared.until = 1;
    hintShared.info = false;
    ReleaseSRWLockExclusive(&hintLock);
    InterlockedIncrement(&hintShows);
}
// The same card for a plain tip: a title and two lines, no why and fix.
void ShowInfo(const char* title, const char* line1, const char* line2, DWORD ms) {
    ShowHint(title, line1, line2, ms);
    AcquireSRWLockExclusive(&hintLock);
    hintShared.info = true;
    ReleaseSRWLockExclusive(&hintLock);
}
// Render thread: takes the current card; true while it is up.
bool TakeHint() {
    AcquireSRWLockShared(&hintLock);
    hintView = hintShared;
    ReleaseSRWLockShared(&hintLock);
    return hintView.until && static_cast<LONG>(hintView.until - GetTickCount()) > 0;
}
// Words onto at most `cap` lines of maxWidth; the last one is cut to fit.
int WrapLines(const char* prefix, const char* text, int maxWidth, char (*out)[128], int cap) {
    if (!text || !text[0] || cap <= 0) return 0;
    char all[360];
    std::snprintf(all, sizeof(all), "%s%s", prefix, text);
    int n = 0;
    const char* p = all;
    while (*p && n < cap) {
        const int length = static_cast<int>(std::strlen(p));
        if (n == cap - 1 || TextWidth(p) <= maxWidth) {
            char cut[128];
            coop_shop::FitText(font.metrics, p, maxWidth, cut, sizeof(cut));
            std::snprintf(out[n++], 128, "%s", cut);
            break;
        }
        int best = 0;
        for (int i = 1; i <= length && i < 127; ++i) {
            if (p[i] != ' ' && p[i] != 0) continue;
            char line[128];
            std::memcpy(line, p, static_cast<size_t>(i));
            line[i] = 0;
            if (TextWidth(line) > maxWidth) break;
            best = i;
        }
        if (!best) best = length < 120 ? length : 120;
        std::memcpy(out[n], p, static_cast<size_t>(best));
        out[n++][best] = 0;
        p += best;
        while (*p == ' ') ++p;
    }
    return n;
}
constexpr uint32_t kHintTitle = 0xFFFFC266;
void DrawHint(int W, int H) {
    const int px = font.px, lh = font.metrics.lineHeight, t = px / 16 < 1 ? 1 : px / 16, pad = px / 2;
    const int width = W * 3 / 5, x = (W - width) / 2, inner = width - pad * 2;
    char lines[7][128];
    uint32_t colors[7];
    int n = 0;
    n += WrapLines("", hintView.what, inner, lines + n, 1);
    colors[0] = kHintTitle;
    const int whyLines = WrapLines(hintView.info ? "" : "Why: ", hintView.why, inner, lines + n, 3);
    for (int i = 0; i < whyLines; ++i) colors[n + i] = kText;
    n += whyLines;
    const int fixLines = WrapLines(hintView.info ? "" : "Fix: ", hintView.fix, inner, lines + n, 3);
    for (int i = 0; i < fixLines; ++i) colors[n + i] = hintView.info ? kText : kGood;
    n += fixLines;
    if (!n) return;
    const int h = pad * 2 + lh * n;
    const int y = H - h - H / 9;
    Rect(x, y, width, h, kPanel);
    Rect(x, y, width, t, kFrame);
    Rect(x, y + h - t, width, t, kFrameDim);
    for (int i = 0; i < n; ++i) Text(x + pad, y + pad + i * lh, lines[i], colors[i]);
    InterlockedIncrement(&hintFrames);
}

// A page card in the middle of the screen: a title and up to 16 lines, each wrapped to
// two at most (the Multiplayer screen's records, how to play and about). The same page
// again closes it, so does its time running out.
struct PageCard { char title[96]; char lines[16][200]; int count; DWORD until; };
SRWLOCK pageLock = SRWLOCK_INIT;
PageCard pageShared{}, pageView{};
volatile LONG pageShows = 0, pageFrames = 0;
// True when it opened, false when the same page was up and is closed now.
bool ShowPage(const char* title, const char* const* lines, int count, DWORD ms) {
    AcquireSRWLockExclusive(&pageLock);
    const bool same = pageShared.until && static_cast<LONG>(pageShared.until - GetTickCount()) > 0 && std::strcmp(pageShared.title, title) == 0;
    if (same) {
        pageShared.until = 0;
    } else {
        std::snprintf(pageShared.title, sizeof(pageShared.title), "%s", title);
        pageShared.count = count < 16 ? count : 16;
        for (int i = 0; i < pageShared.count; ++i) std::snprintf(pageShared.lines[i], sizeof(pageShared.lines[i]), "%s", lines[i] ? lines[i] : "");
        pageShared.until = GetTickCount() + ms;
        if (!pageShared.until) pageShared.until = 1;
    }
    ReleaseSRWLockExclusive(&pageLock);
    if (!same) InterlockedIncrement(&pageShows);
    return !same;
}
bool TakePage() {
    AcquireSRWLockShared(&pageLock);
    pageView = pageShared;
    ReleaseSRWLockShared(&pageLock);
    return pageView.until && static_cast<LONG>(pageView.until - GetTickCount()) > 0;
}
void DrawPage(int W, int H) {
    const int lh = font.metrics.lineHeight, t = font.px / 16 < 1 ? 1 : font.px / 16, pad = font.px / 2;
    const int width = W * 2 / 3, x = (W - width) / 2, inner = width - pad * 2;
    char lines[34][128];
    uint32_t colors[34];
    int n = WrapLines("", pageView.title, inner, lines, 1);
    colors[0] = kHintTitle;
    for (int i = 0; i < pageView.count && n < 32; ++i) {
        const int got = WrapLines("", pageView.lines[i], inner, lines + n, 2);
        for (int k = 0; k < got; ++k) colors[n + k] = kText;
        n += got;
    }
    if (!n) return;
    const int h = pad * 2 + lh * n;
    const int y = (H - h) / 2;
    Rect(x, y, width, h, kPanel);
    Rect(x, y, width, t, kFrame);
    Rect(x, y + h - t, width, t, kFrameDim);
    for (int i = 0; i < n; ++i) Text(x + pad, y + pad + i * lh, lines[i], colors[i]);
    InterlockedIncrement(&pageFrames);
}

// A black screen over an MP map's first moments ([MpMap] ArrivalCurtain): the
// loading screen is gone a moment before the player's body and camera stand, and
// the camera looks into the void meanwhile. runtime_coop_mpmap.inc raises it at
// the arrival and lifts it once the body (or the necro ghost) stands; it falls by
// itself at the deadline.
volatile LONG curtainUntil = 0, curtainShows = 0, curtainFrames = 0;
void Curtain(DWORD ms) {
    DWORD until = GetTickCount() + ms;
    if (!until) until = 1;
    InterlockedExchange(&curtainUntil, static_cast<LONG>(until));
    InterlockedIncrement(&curtainShows);
}
void LiftCurtain() { InterlockedExchange(&curtainUntil, 0); }
bool CurtainUp() {
    const DWORD until = static_cast<DWORD>(InterlockedCompareExchange(&curtainUntil, 0, 0));
    return until && static_cast<LONG>(until - GetTickCount()) > 0;
}
constexpr uint32_t kCurtain = 0xFF000000, kCurtainText = 0xB0D6F7FF;
void DrawCurtain(int W, int H) {
    Rect(0, 0, W, H, kCurtain);
    const char* text = "LOADING";
    Text((W - TextWidth(text)) / 2, H / 2 - font.metrics.lineHeight / 2, text, kCurtainText);
    InterlockedIncrement(&curtainFrames);
}

// The Dementia mutator's veil (runtime_coop_dementia.inc), this machine only: a lasting
// vignette and tint for the stage, and bursts (flicker, glitch bands, flashes) that are
// worked out per frame from their start, length and seed. Drawn under everything else.
enum VeilBurst : int { kVeilNone = 0, kVeilFlickerMild, kVeilFlicker, kVeilFlickerHeavy, kVeilFreakout, kVeilGlitch, kVeilFlash };
struct Veil {
    DWORD start = 0, until = 0;
    int kind = kVeilNone;
    float strength = 0.0f;
    uint32_t seed = 0;
    float ambient = 0.0f;        // 0..1: vignette and tint of the stage
    uint32_t tint = 0x0020A0A0;  // RGB of the tint
    bool gentle = false;         // [Dementia] Flashes=0: no full blackouts, no white flashes
};
SRWLOCK veilLock = SRWLOCK_INIT;
Veil veilShared, veilView;
bool veilNow = false;   // render thread: this frame draws it
volatile LONG veilBursts = 0, veilFrames = 0;
void VeilBurstStart(int kind, float strength, DWORD ms, uint32_t seed) {
    AcquireSRWLockExclusive(&veilLock);
    veilShared.kind = kind;
    veilShared.strength = strength;
    veilShared.seed = seed;
    veilShared.start = GetTickCount();
    veilShared.until = veilShared.start + ms;
    ReleaseSRWLockExclusive(&veilLock);
    InterlockedIncrement(&veilBursts);
}
void VeilAmbient(float level, uint32_t tint, bool gentle) {
    AcquireSRWLockExclusive(&veilLock);
    veilShared.ambient = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
    veilShared.tint = tint & 0xFFFFFF;
    veilShared.gentle = gentle;
    ReleaseSRWLockExclusive(&veilLock);
}
bool TakeVeil() {
    AcquireSRWLockShared(&veilLock);
    veilView = veilShared;
    ReleaseSRWLockShared(&veilLock);
    return veilView.ambient > 0.01f || (veilView.until && static_cast<LONG>(veilView.until - GetTickCount()) > 0);
}
uint32_t VeilHash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}
float VeilUnit(uint32_t a, uint32_t b) { return static_cast<float>(VeilHash(a, b) >> 8) / 16777216.0f; }
uint32_t VeilColor(uint32_t rgb, float alpha) {
    const int a = alpha <= 0.0f ? 0 : alpha >= 1.0f ? 255 : static_cast<int>(alpha * 255.0f);
    return static_cast<uint32_t>(a) << 24 | (rgb & 0xFFFFFF);
}
// How dark a flicker is at t ms: dips of 50..150 ms, `count` of them spread over `length`.
float VeilDips(uint32_t seed, unsigned count, DWORD length, DWORD t) {
    float dark = 0.0f;
    for (unsigned k = 0; k < count; ++k) {
        const float center = VeilUnit(seed, k * 3) * static_cast<float>(length);
        const float half = 25.0f + VeilUnit(seed, k * 3 + 1) * 50.0f;
        const float depth = 0.6f + VeilUnit(seed, k * 3 + 2) * 0.4f;
        const float d = std::fabs(static_cast<float>(t) - center);
        if (d < half) dark = std::max(dark, depth * (1.0f - d / half * 0.3f));
    }
    return dark;
}
void DrawVeil(int W, int H) {
    const Veil& v = veilView;
    const DWORD now = GetTickCount();
    float ambient = v.ambient;
    // the heartbeat of the late stages: two quick swells of the vignette, about 70 a minute
    if (ambient >= 0.6f) {
        const DWORD beat = now % 860;
        const float swell = beat < 120 ? beat / 120.0f : beat < 240 ? 1.0f - (beat - 120) / 120.0f : beat < 300 ? 0.0f : beat < 390 ? (beat - 300) / 90.0f * 0.6f
                          : beat < 480 ? 0.6f - (beat - 390) / 90.0f * 0.6f : 0.0f;
        ambient = std::min(1.0f, ambient + swell * 0.25f);
    }
    float dark = 0.0f, white = 0.0f, red = 0.0f, glitch = 0.0f;
    if (v.until && static_cast<LONG>(v.until - now) > 0) {
        const DWORD length = v.until - v.start, t = now - v.start;
        const float fade = t > length - 200 ? (length - t) / 200.0f : 1.0f;
        switch (v.kind) {
        case kVeilFlickerMild: dark = VeilDips(v.seed, 4, length, t) * 0.45f; break;
        case kVeilFlicker: dark = VeilDips(v.seed, 8, length, t) * 0.8f; break;
        case kVeilFlickerHeavy:
            dark = VeilDips(v.seed, 14, length, t);
            white = VeilDips(v.seed ^ 0x5A5A, 3, length, t) * 0.7f;
            break;
        case kVeilFreakout:
            dark = VeilDips(v.seed, 18, length, t);
            white = VeilDips(v.seed ^ 0x5A5A, 4, length, t) * 0.6f;
            red = (0.5f + 0.5f * std::sin(static_cast<float>(t) * 0.012f)) * 0.35f * fade;
            glitch = 0.6f * fade;
            break;
        case kVeilGlitch: glitch = fade; break;
        case kVeilFlash: white = t < 120 ? 1.0f : std::max(0.0f, 1.0f - (t - 120) / 500.0f); break;
        default: break;
        }
        dark *= v.strength;
        white *= v.strength;
        glitch *= v.strength;
    }
    if (v.gentle) { dark = std::min(dark, 0.5f); white = std::min(white, 0.25f); }
    // the stage's tint, then the vignette in rings, darker towards the edge
    if (ambient > 0.0f) Rect(0, 0, W, H, VeilColor(v.tint, ambient * 0.10f));
    const int rings = 10, depthPx = static_cast<int>(std::min(W, H) * (0.10f + 0.22f * ambient));
    for (int i = 0; i < rings && ambient > 0.0f; ++i) {
        const int inset = depthPx * i / rings, band = depthPx / rings + 1;
        const float o = static_cast<float>(rings - i) / rings;
        const uint32_t c = VeilColor(0x000000, ambient * 0.55f * o * o);
        Rect(inset, inset, W - 2 * inset, band, c);
        Rect(inset, H - inset - band, W - 2 * inset, band, c);
        Rect(inset, inset + band, band, H - 2 * inset - 2 * band, c);
        Rect(W - inset - band, inset + band, band, H - 2 * inset - 2 * band, c);
    }
    if (glitch > 0.0f) {
        const uint32_t slot = now / 50;
        for (uint32_t k = 0; k < 7; ++k) {
            if (VeilUnit(v.seed ^ slot, k * 5) > glitch) continue;
            const int y = static_cast<int>(VeilUnit(v.seed ^ slot, k * 5 + 1) * H), h = 2 + static_cast<int>(VeilUnit(v.seed ^ slot, k * 5 + 2) * 24);
            const int x = static_cast<int>((VeilUnit(v.seed ^ slot, k * 5 + 3) - 0.5f) * 40.0f);
            Rect(x, y, W, h, VeilColor(k % 2 ? 0x30E0E0 : 0xE8F0F0, 0.12f + 0.25f * VeilUnit(v.seed ^ slot, k * 5 + 4)));
        }
    }
    if (red > 0.0f) Rect(0, 0, W, H, VeilColor(0x900000, red));
    if (dark > 0.0f) Rect(0, 0, W, H, VeilColor(0x000000, dark));
    if (white > 0.0f) Rect(0, 0, W, H, VeilColor(0xE8F4FF, white));
    InterlockedIncrement(&veilFrames);
}

// An arrow, tip at (x, y), one rect per pixel row, outlined so it shows on any background.
constexpr uint32_t kPointerFill = 0xFFE8FDFF, kPointerEdge = 0xE0000000;
void DrawPointer(int x, int y) {
    const int h = font.px > 12 ? font.px : 12, u = h / 12 < 1 ? 1 : h / 12;
    for (int pass = 0; pass < 2; ++pass) {
        const int grow = pass == 0 ? u : 0;
        const uint32_t color = pass == 0 ? kPointerEdge : kPointerFill;
        for (int r = 0; r < h; ++r) {
            const int w = r < h * 3 / 4 ? r * 2 / 3 + 1 : (h - r) * 2;
            Rect(x - grow, y + r - (r == 0 ? grow : 0), w + 2 * grow, 1 + (r == h - 1 ? grow : 0), color);
        }
    }
}

// The survival status line (runtime_coop_hud.inc): small at the top centre, a
// thin shadow instead of a panel, an event line under it. The glyphs are the
// panel's, drawn smaller with linear filtering.
struct Hud { char line[96]; char flash[96]; int scalePercent; int topPermille; uint32_t textColor, flashColor; };
SRWLOCK hudLock = SRWLOCK_INIT;
Hud hudShared{}, hudView{};
volatile LONG hudVisible = 0, hudFrames = 0, hudShows = 0;
void SetHud(const Hud& h) {
    AcquireSRWLockExclusive(&hudLock);
    hudShared = h;
    ReleaseSRWLockExclusive(&hudLock);
    if (!InterlockedExchange(&hudVisible, h.line[0] ? 1 : 0) && h.line[0]) InterlockedIncrement(&hudShows);
}
void HideHud() { InterlockedExchange(&hudVisible, 0); }
bool TakeHud() {
    if (!InterlockedCompareExchange(&hudVisible, 0, 0)) return false;
    AcquireSRWLockShared(&hudLock);
    hudView = hudShared;
    ReleaseSRWLockShared(&hudLock);
    return hudView.line[0] != 0;
}
void TextAt(float x, float y, const char* text, uint32_t color, float s) {
    const float tw = static_cast<float>(font.texW), th = static_cast<float>(font.texH);
    for (; *text; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        if (c < 32 || c > 126) c = '?';
        const int i = c - 32;
        if (c != ' ') {
            const int cx = (i % font.cols) * font.cellW, cy = (i / font.cols) * font.cellH;
            const float x0 = x - font.pad * s, y0 = y - s;
            Quad(x0, y0, x0 + font.cellW * s, y0 + font.cellH * s, color, cx / tw, cy / th, (cx + font.cellW) / tw, (cy + font.cellH) / th);
        }
        x += font.metrics.advance[i] * s;
    }
}
constexpr uint32_t kHudShadow = 0x99000000, kHudText = 0xF0D6F7FF, kHudFlash = 0xE8A8F4FF;
void DrawHud(int W, int H) {
    const float s = hudView.scalePercent / 100.0f, sf = s * 0.85f;
    const float o = s < 1.0f ? 1.0f : s;
    const float y = static_cast<float>(H) * static_cast<float>(hudView.topPermille) / 1000.0f;
    const float w = static_cast<float>(TextWidth(hudView.line)) * s;
    const float x = (static_cast<float>(W) - w) / 2.0f;
    TextAt(x + o, y + o, hudView.line, kHudShadow, s);
    TextAt(x, y, hudView.line, hudView.textColor ? hudView.textColor : kHudText, s);
    if (hudView.flash[0]) {
        const float fw = static_cast<float>(TextWidth(hudView.flash)) * sf, fx = (static_cast<float>(W) - fw) / 2.0f;
        const float fy = y + static_cast<float>(font.metrics.lineHeight) * s * 1.05f;
        TextAt(fx + o, fy + o, hudView.flash, kHudShadow, sf);
        TextAt(fx, fy, hudView.flash, hudView.flashColor ? hudView.flashColor : kHudFlash, sf);
    }
    InterlockedIncrement(&hudFrames);
}

// The voice packs' subtitles (runtime_coop_voices.inc): up to three rows at the
// bottom centre, the speaker's pack name in its colour, the line in white.
struct SubtitleRow { char who[24]; char text[100]; uint32_t color; DWORD until; };
struct Subtitles { int count; SubtitleRow rows[3]; };
SRWLOCK subtitleLock = SRWLOCK_INIT;
Subtitles subtitleShared{}, subtitleView{};
volatile LONG subtitlesVisible = 0, subtitleFrames = 0;
void SetSubtitles(const Subtitles& s) {
    AcquireSRWLockExclusive(&subtitleLock);
    subtitleShared = s;
    ReleaseSRWLockExclusive(&subtitleLock);
    InterlockedExchange(&subtitlesVisible, s.count > 0 ? 1 : 0);
}
void HideSubtitles() { InterlockedExchange(&subtitlesVisible, 0); }
bool TakeSubtitles() {
    if (!InterlockedCompareExchange(&subtitlesVisible, 0, 0)) return false;
    AcquireSRWLockShared(&subtitleLock);
    subtitleView = subtitleShared;
    ReleaseSRWLockShared(&subtitleLock);
    const DWORD now = GetTickCount();
    int live = 0;
    for (int i = 0; i < subtitleView.count; ++i)
        if (static_cast<LONG>(subtitleView.rows[i].until - now) > 0) subtitleView.rows[live++] = subtitleView.rows[i];
    subtitleView.count = live;
    return live > 0;
}
void DrawSubtitles(int W, int H) {
    const float s = static_cast<float>(H) / 1080.0f * 0.75f < 0.45f ? 0.45f : static_cast<float>(H) / 1080.0f * 0.75f;
    const float lh = static_cast<float>(font.metrics.lineHeight) * s * 1.15f, o = s < 1.0f ? 1.0f : s;
    float y = static_cast<float>(H) * 0.86f - lh * static_cast<float>(subtitleView.count);
    for (int i = 0; i < subtitleView.count; ++i, y += lh) {
        const SubtitleRow& r = subtitleView.rows[i];
        char who[32];
        std::snprintf(who, sizeof(who), "%s: ", r.who);
        const float ww = static_cast<float>(TextWidth(who)) * s, tw = static_cast<float>(TextWidth(r.text)) * s;
        const float x = (static_cast<float>(W) - ww - tw) / 2.0f;
        TextAt(x + o, y + o, who, kHudShadow, s);
        TextAt(x, y, who, r.color, s);
        TextAt(x + ww + o, y + o, r.text, kHudShadow, s);
        TextAt(x + ww, y, r.text, 0xF0FFFFFFu, s);
    }
    InterlockedIncrement(&subtitleFrames);
}

// Waypoints on the screen for the zone and the Mercenaries capsules
// (runtime_coop_marks.inc hands them over). The camera is the one
// UIMPObjectiveIndicator::CheckObjOnScreen (0x01095A90) projects with: the
// camera-to-world matrix CameraManager::GetActiveCameraToWorldMatrix (0x00792970)
// returns for player 0 (CameraManager [0x0202131C]: blenders +0x10, 0x2A0 each,
// matrix +0x50; viewports +0x164, their player ids +0x168), and the field of view
// (horizontal, radians) and aspect of that viewport's RenderContext (+0x14 per
// viewport; RC_SetPerspective 0x00792110 writes +0x14C and +0x150).
struct Mark { float pos[3]; uint32_t color; uint8_t kind; char text[24]; };
// kMarkName: a partner's or a bot's name over its head where the game's own plate is
// not available (the campaign, the arenas): text only, only while on the screen.
// kMarkDebugEnemy: the debug menu's "show all enemies", only while on the screen.
// kMarkHeart: a human a necro player hears (the heartbeat radar), through walls.
enum : uint8_t { kMarkZone = 0, kMarkCapsule = 1, kMarkGolden = 2, kMarkName = 3, kMarkEnemy = 4, kMarkDebugEnemy = 5, kMarkHeart = 6 };
struct Marks { int count; int scalePercent; bool selfValid; float self[3]; DWORD enemyScanAt; Mark m[80]; };
SRWLOCK marksLock = SRWLOCK_INIT;
Marks marksShared{}, marksView{};
volatile LONG marksVisible = 0, marksShows = 0, marksFrames = 0, marksNoCamera = 0, marksOnScreen = 0, marksAtEdge = 0, marksDrawThread = 0;
volatile LONG marksFovMilli = 0, marksAspectMilli = 0, marksHearts = 0;
float heartRange = 40.0f;   // [Necro] RadarRange, set by marks::Install
void SetMarks(const Marks& m) {
    AcquireSRWLockExclusive(&marksLock);
    marksShared = m;
    ReleaseSRWLockExclusive(&marksLock);
    if (!InterlockedExchange(&marksVisible, m.count > 0 ? 1 : 0) && m.count > 0) InterlockedIncrement(&marksShows);
}
void HideMarks() { InterlockedExchange(&marksVisible, 0); }
bool TakeMarks() {
    if (!InterlockedCompareExchange(&marksVisible, 0, 0)) return false;
    AcquireSRWLockShared(&marksLock);
    marksView = marksShared;
    ReleaseSRWLockShared(&marksLock);
    return marksView.count > 0;
}
constexpr uint32_t kCameraManagerRva = 0x1C2131C, kCmBlenders = 0x10, kCmContexts = 0x14, kCmViewports = 0x164, kCmPlayerIds = 0x168, kBlenderSize = 0x2A0,
                   kBlenderCamera = 0x50, kContextFov = 0x14C;
bool cameraLogged = false;
bool CameraNow(coop_marks::Camera& c, int W, int H) {
    uint32_t cm = 0, count = 0, blenders = 0, context = 0;
    if (!Read(base + kCameraManagerRva, &cm, 4) || !cm || !Read(cm + kCmViewports, &count, 4) || !count || count > 4) return false;
    uint32_t index = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t player = 1;
        if (Read(cm + kCmPlayerIds + i * 4, &player, 4) && player == 0) { index = i; break; }
    }
    float m[16]{}, fa[2]{};
    if (!Read(cm + kCmBlenders, &blenders, 4) || !blenders || !Read(blenders + index * kBlenderSize + kBlenderCamera, m, sizeof(m))) return false;
    if (!Read(cm + kCmContexts + index * 4, &context, 4) || !context || !Read(context + kContextFov, fa, sizeof(fa))) return false;
    if (!(fa[0] > 0.2f && fa[0] < 3.0f) || !(fa[1] > 0.3f && fa[1] < 5.0f)) return false;
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) c.axis[r][k] = m[r * 4 + k];
        c.pos[r] = m[12 + r];
    }
    for (float v : m) if (!std::isfinite(v)) return false;
    c.tanX = std::tan(fa[0] * 0.5f);
    c.tanY = c.tanX / fa[1];
    InterlockedExchange(&marksFovMilli, static_cast<LONG>(fa[0] * 1000.0f));
    InterlockedExchange(&marksAspectMilli, static_cast<LONG>(fa[1] * 1000.0f));
    if (!cameraLogged) {
        cameraLogged = true;
        Line("marks camera: viewport %u of %u, RenderContext %08X, fov %.3f rad (%.1f deg horizontal), aspect %.3f, target %dx%d (%.3f), camera at (%.1f %.1f %.1f)",
             index, count, context, fa[0], fa[0] * 57.29578f, fa[1], W, H, H ? static_cast<float>(W) / static_cast<float>(H) : 0.0f, m[12], m[13], m[14]);
    }
    return true;
}
uint32_t Alpha(uint32_t c, float f) { return (static_cast<uint32_t>(static_cast<float>(c >> 24) * f) << 24) | (c & 0x00FFFFFFu); }
void Diamond(float x, float y, float r, uint32_t c) {
    Tri(x, y - r, x + r, y, x, y + r, c);
    Tri(x, y - r, x, y + r, x - r, y, c);
}
void Hourglass(float x, float y, float r, uint32_t c) {
    const float a = r * 0.72f;
    Tri(x - a, y - r, x + a, y - r, x, y, c);
    Tri(x, y, x + a, y + r, x - a, y + r, c);
}
void Arrow(float x, float y, float dx, float dy, float r, float grow, uint32_t c) {
    const float tip = r * 2.35f + grow, back = r * 1.45f - grow * 0.5f, half = r * 0.7f + grow;
    const float bx = x + dx * back, by = y + dy * back;
    Tri(x + dx * tip, y + dy * tip, bx - dy * half, by + dx * half, bx + dy * half, by - dx * half, c);
}
void Heart(float x, float y, float s, uint32_t c) {
    Diamond(x - s * 0.5f, y - s * 0.3f, s * 0.62f, c);
    Diamond(x + s * 0.5f, y - s * 0.3f, s * 0.62f, c);
    Tri(x - s * 1.08f, y - s * 0.12f, x + s * 1.08f, y - s * 0.12f, x, y + s * 1.15f, c);
}
// A double beat (lub dub) once a period; the period shrinks as the human comes closer.
float HeartBeat(float distance) {
    const DWORD period = static_cast<DWORD>(500.0f + 25.0f * std::min(distance, 60.0f));
    const float phase = static_cast<float>(GetTickCount() % period) / static_cast<float>(period);
    const auto pulse = [](float t, float at, float width) { const float d = (t - at) / width; return d < 0.0f || d > 1.0f ? 0.0f : 1.0f - d; };
    return std::max(pulse(phase, 0.0f, 0.14f), 0.7f * pulse(phase, 0.2f, 0.14f));
}
constexpr uint32_t kMarkEdge = 0xC8000000, kMarkShadow = 0xB0000000;
void DrawMarks(int W, int H) {
    coop_marks::Camera cam{};
    if (!CameraNow(cam, W, H)) { InterlockedIncrement(&marksNoCamera); return; }
    InterlockedExchange(&marksDrawThread, static_cast<LONG>(GetCurrentThreadId()));
    const float scale = (marksView.scalePercent < 25 ? 25 : marksView.scalePercent > 400 ? 400 : marksView.scalePercent) / 100.0f;
    const float r = std::max(7.0f, static_cast<float>(H) * 0.011f * scale), ts = 0.6f * scale, lh = static_cast<float>(font.metrics.lineHeight) * ts;
    const float margin = r * 2.6f + lh * 2.0f;
    const bool staleEnemies = GetTickCount() - marksView.enemyScanAt >= 1000;
    for (int i = 0; i < marksView.count && i < static_cast<int>(std::size(marksView.m)); ++i) {
        const Mark& m = marksView.m[i];
        if (m.kind == kMarkDebugEnemy) {
            if (staleEnemies) continue;
            const float p[3] = {m.pos[0], m.pos[1] + 1.0f, m.pos[2]};
            const coop_marks::Placed at = coop_marks::Place(cam, p, static_cast<float>(W), static_cast<float>(H), lh);
            if (!at.onScreen) continue;
            const float small = std::max(3.0f, r * 0.45f), o = std::max(1.0f, small * 0.25f);
            Diamond(at.x, at.y, small + o, kMarkEdge);
            Diamond(at.x, at.y, small, m.color);
            char distance[16] = "";
            if (marksView.selfValid) {
                const float dx = m.pos[0] - marksView.self[0], dy = m.pos[1] - marksView.self[1], dz = m.pos[2] - marksView.self[2];
                std::snprintf(distance, sizeof(distance), "%.0f M", std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            const float ds = ts * 0.85f;
            float ty = at.y + small * 1.4f;
            for (const char* line : {static_cast<const char*>(m.text), static_cast<const char*>(distance)}) {
                if (!line[0]) continue;
                const float tw = static_cast<float>(TextWidth(line)) * ds, tx = at.x - tw * 0.5f;
                TextAt(tx + 1.0f, ty + 1.0f, line, kMarkShadow, ds);
                TextAt(tx, ty, line, line == m.text ? m.color : kHudText, ds);
                ty += lh * 0.85f;
            }
            continue;
        }
        if (m.kind == kMarkName) {
            const coop_marks::Placed at = coop_marks::Place(cam, m.pos, static_cast<float>(W), static_cast<float>(H), lh);
            if (!at.onScreen || !m.text[0]) continue;
            // far away it fades, never below a quarter
            float fade = 1.0f;
            if (marksView.selfValid) {
                const float dx = m.pos[0] - marksView.self[0], dy = m.pos[1] - marksView.self[1], dz = m.pos[2] - marksView.self[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > 40.0f) fade = std::max(0.25f, 1.0f - (d - 40.0f) / 60.0f);
            }
            const float ns = ts * 1.15f, tw = static_cast<float>(TextWidth(m.text)) * ns, tx = at.x - tw * 0.5f, ty = at.y - lh * 1.15f;
            TextAt(tx + 1.0f, ty + 1.0f, m.text, Alpha(kMarkShadow, fade), ns);
            TextAt(tx, ty, m.text, Alpha(m.color, fade), ns);
            continue;
        }
        if (m.kind == kMarkHeart) {
            if (!marksView.selfValid) continue;
            const float hp[3] = {m.pos[0], m.pos[1] + 1.3f, m.pos[2]};
            const float dx = hp[0] - marksView.self[0], dy = hp[1] - marksView.self[1], dz = hp[2] - marksView.self[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz), closeness = std::max(0.0f, 1.0f - d / std::max(5.0f, heartRange));
            const float beat = HeartBeat(d), alpha = (0.3f + 0.7f * closeness) * (0.35f + 0.65f * beat);
            const coop_marks::Placed at = coop_marks::Place(cam, hp, static_cast<float>(W), static_cast<float>(H), margin);
            const float size = r * (0.75f + 0.25f * closeness) * (1.0f + 0.35f * beat), o = std::max(1.5f, r * 0.18f);
            if (at.onScreen) {
                Heart(at.x, at.y, size + o, Alpha(kMarkEdge, alpha));
                Heart(at.x, at.y, size, Alpha(m.color, alpha));
            } else {
                Arrow(at.x, at.y, at.dirX, at.dirY, size, o, Alpha(kMarkEdge, alpha));
                Arrow(at.x, at.y, at.dirX, at.dirY, size, 0.0f, Alpha(m.color, alpha));
            }
            InterlockedIncrement(&marksHearts);
            continue;
        }
        const float lift = m.kind == kMarkZone ? 1.2f : 0.6f;
        const float p[3] = {m.pos[0], m.pos[1] + lift, m.pos[2]};
        const coop_marks::Placed at = coop_marks::Place(cam, p, static_cast<float>(W), static_cast<float>(H), margin);
        float fade = 1.0f;
        if (at.onScreen) {
            InterlockedIncrement(&marksOnScreen);
            const float dx = at.x - W * 0.5f, dy = at.y - H * 0.5f, centre = H * 0.07f;
            if (dx * dx + dy * dy < centre * centre) fade = 0.4f;   // out of the crosshair's way
        } else {
            InterlockedIncrement(&marksAtEdge);
        }
        const float big = m.kind == kMarkGolden ? r * 1.2f : r, o = std::max(1.5f, r * 0.18f);
        if (!at.onScreen) {
            Arrow(at.x, at.y, at.dirX, at.dirY, big, o, Alpha(kMarkEdge, fade));
            Arrow(at.x, at.y, at.dirX, at.dirY, big, 0.0f, Alpha(m.color, fade));
        }
        if (m.kind == kMarkZone) {
            Diamond(at.x, at.y, big + o, Alpha(kMarkEdge, fade));
            Diamond(at.x, at.y, big, Alpha(m.color, fade));
            Diamond(at.x, at.y, big * 0.45f, Alpha(kMarkEdge, fade * 0.8f));
        } else if (m.kind == kMarkEnemy) {
            Diamond(at.x, at.y, big * 0.8f + o, Alpha(kMarkEdge, fade));
            Diamond(at.x, at.y, big * 0.8f, Alpha(m.color, fade));
        } else {
            Hourglass(at.x, at.y, big + o, Alpha(kMarkEdge, fade));
            Hourglass(at.x, at.y, big, Alpha(m.color, fade));
        }
        // the distance (and the zone's state) on the side away from the arrow
        char distance[16] = "";
        if (marksView.selfValid) {
            const float dx = m.pos[0] - marksView.self[0], dy = m.pos[1] - marksView.self[1], dz = m.pos[2] - marksView.self[2];
            std::snprintf(distance, sizeof(distance), "%.0f M", std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        const char* lines[2] = {distance, m.text};
        const bool above = !at.onScreen && at.dirY > 0.35f;
        float ty = above ? at.y - big * 1.35f - lh * ((distance[0] ? 1.0f : 0.0f) + (m.text[0] ? 1.0f : 0.0f)) : at.y + big * 1.35f;
        for (const char* line : lines) {
            if (!line[0]) continue;
            const float tw = static_cast<float>(TextWidth(line)) * ts, tx = at.x - tw * 0.5f;
            TextAt(tx + 1.0f, ty + 1.0f, line, Alpha(kMarkShadow, fade), ts);
            TextAt(tx, ty, line, Alpha(line == m.text ? m.color : kHudText, fade), ts);
            ty += lh;
        }
    }
    InterlockedIncrement(&marksFrames);
}

void SetStates(void* d, uint32_t w, uint32_t h) {
    static const uint32_t kStates[][2] = {
        {7, 0}, {14, 0}, {15, 0}, {22, 1}, {8, 3}, {9, 2},               // no z, no alpha test, no culling, solid, gouraud
        {27, 1}, {19, 5}, {20, 6}, {171, 1}, {206, 0},                   // src alpha over inv src alpha
        {28, 0}, {29, 0}, {52, 0}, {137, 0}, {136, 1}, {174, 0},         // no fog, specular, stencil, lighting, scissor
        {168, 0xF}, {194, 0}, {195, 0}, {175, 0}, {185, 0}, {152, 0},    // colour writes, linear writes, no depth bias, no clip planes
        {151, 0}, {167, 0}, {156, 0}, {176, 0}, {141, 1}, {180, 0},      // ... and no NVDB depth bounds (adaptive tess X)
    };
    for (const auto& s : kStates) VCall<HRESULT>(d, kDevSetRenderState, s[0], s[1]);
    static const uint32_t kStage0[][2] = {{1, 4}, {2, 2}, {3, 0}, {4, 4}, {5, 2}, {6, 0}, {11, 0}, {24, 0}};   // modulate texture and diffuse
    for (const auto& s : kStage0) VCall<HRESULT>(d, kDevSetTextureStageState, 0ul, s[0], s[1]);
    VCall<HRESULT>(d, kDevSetTextureStageState, 1ul, 1ul, 1ul);   // stage 1 colour off
    VCall<HRESULT>(d, kDevSetTextureStageState, 1ul, 4ul, 1ul);   // stage 1 alpha off
    static const uint32_t kSampler0[][2] = {{1, 3}, {2, 3}, {5, 1}, {6, 1}, {7, 0}, {11, 0}, {8, 0}, {9, 0}};   // clamp, point, no mips, no sRGB
    for (const auto& s : kSampler0) VCall<HRESULT>(d, kDevSetSamplerState, 0ul, s[0], s[1]);
    VCall<HRESULT>(d, kDevSetVertexShader, static_cast<void*>(nullptr));
    VCall<HRESULT>(d, kDevSetPixelShader, static_cast<void*>(nullptr));
    VCall<HRESULT>(d, kDevSetFVF, kFvf);
    VCall<HRESULT>(d, kDevSetTexture, 0ul, font.texture);
    VCall<HRESULT>(d, kDevSetTexture, 1ul, static_cast<void*>(nullptr));
    const Viewport vp{0, 0, w, h, 0.0f, 1.0f};
    VCall<HRESULT>(d, kDevSetViewport, &vp);
}

void* activeBlock = nullptr;
// 17.10.: the debug menu was drawn every frame and never seen; the guess was that the frame
// ended on an offscreen target. It always does (18.10.: every frame, 1920x1080 render target):
// the game copies that target to the screen after EndScene, so drawing into the back buffer
// hid the whole overlay. Off by default; [Shop] OverlayBackBuffer=1 only for experiments.
// Render thread.
void* savedTarget = nullptr;
void* savedDepth = nullptr;
void* lastOffscreen = nullptr;
void* OffscreenBackBuffer(void* d, void* target, SurfaceDesc& desc, HRESULT& described) {
    if (!backBufferFix) return nullptr;
    void* backBuffer = nullptr;
    if (VCall<HRESULT>(d, kDevGetBackBuffer, 0u, 0u, 0u, &backBuffer) != 0 || !backBuffer) return nullptr;
    if (backBuffer == target) { VCall<ULONG>(backBuffer, kRelease); return nullptr; }
    InterlockedIncrement(&offscreenFrames);
    SurfaceDesc bb{};
    const HRESULT bbDescribed = VCall<HRESULT>(backBuffer, kSurfaceGetDesc, &bb);
    if (target != lastOffscreen && InterlockedIncrement(&offscreenNotes) <= 8)
        Line("the frame ends on %p (%ux%u format %u usage %08X) instead of the back buffer %p (%ux%u format %u): the panel goes to the back buffer", target,
             desc.width, desc.height, desc.format, desc.usage, backBuffer, bb.width, bb.height, bb.format);
    lastOffscreen = target;
    if (bbDescribed != 0 || bb.width < 320 || bb.height < 200) { VCall<ULONG>(backBuffer, kRelease); return nullptr; }
    desc = bb;
    described = 0;
    return backBuffer;
}
// Takes the back buffer's reference. No depth surface: ours may be smaller than the back buffer.
void BindBackBuffer(void* d, void* backBuffer) {
    void* target = nullptr;
    void* depth = nullptr;
    if (VCall<HRESULT>(d, kDevGetRenderTarget, 0ul, &target) != 0 || !target) {
        VCall<ULONG>(backBuffer, kRelease);
        InterlockedIncrement(&backBufferFailures);
        return;
    }
    if (VCall<HRESULT>(d, kDevGetDepthStencil, &depth) != 0) depth = nullptr;
    if (VCall<HRESULT>(d, kDevSetRenderTarget, 0ul, backBuffer) != 0) {
        VCall<ULONG>(backBuffer, kRelease);
        VCall<ULONG>(target, kRelease);
        if (depth) VCall<ULONG>(depth, kRelease);
        if (InterlockedIncrement(&backBufferFailures) <= 3) Line("SetRenderTarget(back buffer) failed: drawn where the frame ends");
        return;
    }
    VCall<HRESULT>(d, kDevSetDepthStencil, static_cast<void*>(nullptr));
    VCall<ULONG>(backBuffer, kRelease);   // bound, the device holds it
    savedTarget = target;
    savedDepth = depth;
    InterlockedIncrement(&backBufferDraws);
}
// Before the state block goes back: SetRenderTarget resets the viewport, the block restores it.
void UnbindBackBuffer(void* d) {
    if (!savedTarget) return;
    void* target = savedTarget;
    void* depth = savedDepth;
    savedTarget = nullptr;
    savedDepth = nullptr;
    VCall<HRESULT>(d, kDevSetRenderTarget, 0ul, target);
    VCall<HRESULT>(d, kDevSetDepthStencil, depth);
    VCall<ULONG>(target, kRelease);
    if (depth) VCall<ULONG>(depth, kRelease);
}
void DrawFrame(uint32_t device) {
    void* d = reinterpret_cast<void*>(static_cast<uintptr_t>(device));
    uint32_t global = 0;
    unsigned char lost = 0;
    if (!device || !Read(base + kDeviceGlobalRva, &global, 4) || global != device) { InterlockedIncrement(&deviceMismatch); return; }
    Read(base + kLostFlagRva, &lost, 1);
    const HRESULT cooperative = VCall<HRESULT>(d, kDevTestCooperativeLevel);
    if (lost || cooperative != 0) {
        InterlockedIncrement(&lostSkips);
        if (!wasLost) Line("device lost (TestCooperativeLevel %08X, renderer flag %u): not drawn until it is back", static_cast<unsigned>(cooperative), lost);
        wasLost = true;
        return;
    }
    if (wasLost) {
        wasLost = false;
        InterlockedIncrement(&resets);
        Line("device back after a loss: drawing again (font texture %s)", font.texture ? "kept, managed" : "not built yet");
    }
    if (lastDevice && d != lastDevice) {
        InterlockedIncrement(&deviceChanges);
        Line("the device changed %p -> %p: the font is built again", lastDevice, d);
        ReleaseFont();
    }
    lastDevice = d;
    void* target = nullptr;
    if (VCall<HRESULT>(d, kDevGetRenderTarget, 0ul, &target) != 0 || !target) { InterlockedIncrement(&noTarget); return; }
    SurfaceDesc desc{};
    HRESULT described = VCall<HRESULT>(target, kSurfaceGetDesc, &desc);
    void* backBuffer = OffscreenBackBuffer(d, target, desc, described);   // referenced, or null
    VCall<ULONG>(target, kRelease);
    auto dropBackBuffer = [&] { if (backBuffer) VCall<ULONG>(backBuffer, kRelease); backBuffer = nullptr; };
    if (described != 0 || desc.width < 320 || desc.height < 200 || desc.width > 16384 || desc.height > 16384) {
        dropBackBuffer();
        InterlockedIncrement(&smallTarget);
        return;
    }
    if (!loggedTarget || static_cast<LONG>(desc.width) != targetW || static_cast<LONG>(desc.height) != targetH) {
        uint32_t presentSize[2]{};
        Read(base + kPresentParamsRva, presentSize, 8);
        Line("render target %ux%u format %u, back buffer %ux%u", desc.width, desc.height, desc.format, presentSize[0], presentSize[1]);
        loggedTarget = true;
    }
    InterlockedExchange(&targetW, static_cast<LONG>(desc.width));
    InterlockedExchange(&targetH, static_cast<LONG>(desc.height));
    InterlockedExchange(&targetFormat, static_cast<LONG>(desc.format));
    const LONG serial = InterlockedCompareExchange(&sharedSerial, 0, 0);
    if (serial != viewSerial) {
        AcquireSRWLockShared(&viewLock);
        view = shared;
        ReleaseSRWLockShared(&viewLock);
        viewSerial = serial;
    }
    const int px = coop_shop::GlyphPixels(static_cast<int>(desc.height), scalePercent);
    if (!font.texture || font.px != px || font.device != d) {
        if (!BuildFont(d, px)) {
            dropBackBuffer();
            InterlockedIncrement(&fontFailures);
            const DWORD now = GetTickCount();
            if (now - fontWindowStart > kTroubleWindowMs) { fontWindowStart = now; fontFailuresInWindow = 0; }
            if (++fontFailuresInWindow >= 3) {
                PauseDrawing("three font builds failed within 10 s");
                Line("three font builds failed within 10 s: nothing drawn for 30 s, then the font is built again");
            }
            return;
        }
    }
    void* block = nullptr;
    if (VCall<HRESULT>(d, kDevCreateStateBlock, 1u, &block) != 0 || !block) {   // D3DSBT_ALL
        dropBackBuffer();
        if (InterlockedIncrement(&blockFailures) <= 3) Line("CreateStateBlock(D3DSBT_ALL) failed: nothing drawn (no way to put the game's state back)");
        return;
    }
    activeBlock = block;
    drawDevice = d;
    vertexCount = 0;
    if (backBuffer) {
        void* taken = backBuffer;
        backBuffer = nullptr;
        BindBackBuffer(d, taken);
    }
    SetStates(d, desc.width, desc.height);
    if (veilNow) DrawVeil(static_cast<int>(desc.width), static_cast<int>(desc.height));
    if (CurtainUp()) DrawCurtain(static_cast<int>(desc.width), static_cast<int>(desc.height));
    const bool panel = InterlockedCompareExchange(&visible, 0, 0) != 0;
    if (panel) {
        DrawView(static_cast<int>(desc.width), static_cast<int>(desc.height));
        InterlockedIncrement(&panelFrames);
    }
    if (panel && InterlockedCompareExchange(&pointerOn, 0, 0))
        DrawPointer(static_cast<int>(InterlockedCompareExchange(&pointerX, 0, 0)), static_cast<int>(InterlockedCompareExchange(&pointerY, 0, 0)));
    if (bannerView.until && static_cast<LONG>(bannerView.until - GetTickCount()) > 0) DrawBanner(static_cast<int>(desc.width), static_cast<int>(desc.height));
    if (hintView.until && static_cast<LONG>(hintView.until - GetTickCount()) > 0) DrawHint(static_cast<int>(desc.width), static_cast<int>(desc.height));
    if (pageView.until && static_cast<LONG>(pageView.until - GetTickCount()) > 0) DrawPage(static_cast<int>(desc.width), static_cast<int>(desc.height));
    Flush();
    // The panel shows the wave itself; the HUD keeps out of its way. The marks
    // stay under the HUD and away from the curtain, the panel and the banner.
    const bool clean = InterlockedCompareExchange(&cameraHidden, 0, 0) != 0;
    const bool hudNow = !panel && !clean && hudView.line[0] && InterlockedCompareExchange(&hudVisible, 0, 0);
    const bool subtitlesNow = !panel && !clean && !CurtainUp() && subtitleView.count > 0 && InterlockedCompareExchange(&subtitlesVisible, 0, 0);
    const bool marksNow = !panel && !clean && !CurtainUp() && !(bannerView.until && static_cast<LONG>(bannerView.until - GetTickCount()) > 0) && marksView.count > 0 &&
                          InterlockedCompareExchange(&marksVisible, 0, 0);
    if (hudNow || marksNow || subtitlesNow) {
        VCall<HRESULT>(d, kDevSetSamplerState, 0ul, 5ul, 2ul);   // mag filter linear
        VCall<HRESULT>(d, kDevSetSamplerState, 0ul, 6ul, 2ul);   // min filter linear
        if (marksNow) DrawMarks(static_cast<int>(desc.width), static_cast<int>(desc.height));
        if (hudNow) DrawHud(static_cast<int>(desc.width), static_cast<int>(desc.height));
        if (subtitlesNow) DrawSubtitles(static_cast<int>(desc.width), static_cast<int>(desc.height));
        Flush();
    }
    UnbindBackBuffer(d);
    VCall<HRESULT>(block, kBlockApply);
    VCall<ULONG>(block, kRelease);
    activeBlock = nullptr;
    InterlockedIncrement(&framesDrawn);
}
int DrawFilter(unsigned code) {
    if (InterlockedIncrement(&drawFaults) <= 5) gate::Event("shop overlay FAULT code %08X while drawing", code);
    return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) void RestoreAfterFault() {
    void* block = activeBlock;
    activeBlock = nullptr;
    vertexCount = 0;
    if (savedTarget && drawDevice) {
        __try {
            UnbindBackBuffer(drawDevice);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            savedTarget = nullptr;
            savedDepth = nullptr;
        }
    }
    if (!block) return;
    __try {
        VCall<HRESULT>(block, kBlockApply);
        VCall<ULONG>(block, kRelease);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
__declspec(noinline) void DrawGuarded(uint32_t device) {
    LARGE_INTEGER t0{}, t1{}, hz{};
    QueryPerformanceCounter(&t0);
    __try {
        DrawFrame(device);
    } __except (DrawFilter(GetExceptionCode())) {
        RestoreAfterFault();
        const DWORD now = GetTickCount();
        if (now - faultWindowStart > kTroubleWindowMs) { faultWindowStart = now; faultsInWindow = 0; }
        if (++faultsInWindow >= 3) {
            PauseDrawing("three draw faults within 10 s");
        }
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    if (hz.QuadPart > 0) {
        const LONG us = static_cast<LONG>((t1.QuadPart - t0.QuadPart) * 1000000 / hz.QuadPart);
        InterlockedExchangeAdd(&drawMicrosTotal, us);
        if (us > drawMicrosMax) InterlockedExchange(&drawMicrosMax, us);
    }
}
// The mid hook: esi holds the device EndFrame loaded; the call to EndScene follows.
void OnEndScene(safetyhook::Context& ctx) {
    InterlockedIncrement(&hookCalls);
    if (InterlockedCompareExchange(&disabled, 0, 0) && disabledAt && GetTickCount() - disabledAt >= kRetryAfterMs) {
        disabledAt = 0;
        faultsInWindow = fontFailuresInWindow = 0;
        InterlockedIncrement(&retries);
        InterlockedExchange(&disabled, 0);
        Line("drawing again after a 30 s pause (%s)", disabledWhy);
    }
    const bool banner = TakeBanner();
    const bool hintUp = TakeHint();
    const bool pageUp = TakePage();
    const bool hudUp = TakeHud();
    const bool curtain = CurtainUp();
    const bool marksUp = TakeMarks();
    const bool subtitlesUp = TakeSubtitles();
    veilNow = TakeVeil() && !InterlockedCompareExchange(&cameraHidden, 0, 0);
    if ((!InterlockedCompareExchange(&visible, 0, 0) && !banner && !hintUp && !pageUp && !hudUp && !curtain && !marksUp && !subtitlesUp && !veilNow) ||
        InterlockedCompareExchange(&disabled, 0, 0)) {
        InterlockedIncrement(&hiddenFrames);
        return;
    }
    DrawGuarded(static_cast<uint32_t>(ctx.esi));
}

// --- setup ---------------------------------------------------------------------------
bool Install(FILE* log, const std::string& ini) {
    if (ready) return true;   // the shop, the debug menu and the HUD each ask for it
    scalePercent = GetPrivateProfileIntA("Shop", "Scale", 100, ini.c_str());
    backBufferFix = GetPrivateProfileIntA("Shop", "OverlayBackBuffer", 0, ini.c_str()) != 0;
    char faceText[48]{};
    GetPrivateProfileStringA("Shop", "Font", "Bahnschrift SemiBold", faceText, sizeof(faceText), ini.c_str());
    if (faceText[0]) std::snprintf(fontFaces[0], sizeof(fontFaces[0]), "%s", faceText);
    const uint32_t inScene = static_cast<uint32_t>(base + kInSceneRva), device = static_cast<uint32_t>(base + kDeviceGlobalRva);
    // sub esp,18h / push ebx / xor ebx,ebx / cmp [in-scene flag],bl / jz
    const unsigned char kEntry[] = {0x83, 0xEC, 0x18, 0x53, 0x33, 0xDB, 0x38, 0x1D, static_cast<unsigned char>(inScene), static_cast<unsigned char>(inScene >> 8),
                                    static_cast<unsigned char>(inScene >> 16), static_cast<unsigned char>(inScene >> 24), 0x0F, 0x84};
    // mov esi,[device]
    const unsigned char kLoad[] = {0x8B, 0x35, static_cast<unsigned char>(device), static_cast<unsigned char>(device >> 8), static_cast<unsigned char>(device >> 16),
                                   static_cast<unsigned char>(device >> 24)};
    // mov eax,[esi] / mov ecx,[eax+0A8h] / push esi / call ecx / pop esi / pop ebx / add esp,18h / ret
    static const unsigned char kCall[] = {0x8B, 0x06, 0x8B, 0x88, 0xA8, 0x00, 0x00, 0x00, 0x56, 0xFF, 0xD1, 0x5E, 0x5B, 0x83, 0xC4, 0x18, 0xC3};
    unsigned char entry[sizeof(kEntry)]{}, load[sizeof(kLoad)]{}, caller[5]{};
    const bool entryOk = Read(base + kEndFrameRva, entry, sizeof(entry)) && std::memcmp(entry, kEntry, sizeof(kEntry)) == 0;
    const bool loadOk = Read(base + kEndFrameRva + kDeviceLoad, load, sizeof(load)) && std::memcmp(load, kLoad, sizeof(kLoad)) == 0;
    int32_t rel = 0;
    const bool callerOk = Read(base + kEndFrameCallerRva, caller, sizeof(caller)) && caller[0] == 0xE8 && (std::memcpy(&rel, caller + 1, 4), true) &&
                          base + kEndFrameCallerRva + 5 + rel == base + kEndFrameRva;
    uint32_t deviceNow = 0, flags = 0;
    Read(base + kDeviceGlobalRva, &deviceNow, 4);
    Read(base + 0x1B639F0, &flags, 4);   // the BehaviorFlags CreateDevice was given
    if (entryOk && loadOk)
        ready = hook_site::InstallMid(log, "shop overlay EndFrame EndScene call", base + kEndSceneCallRva, kCall, sizeof(kCall), OnEndScene, endSceneHook);
    std::fprintf(log, "shop overlay render hook %s: EndFrame %08X (entry=%d device_load=%d, caller %08X=%d) EndScene call site %08X | device source [%08X] "
                      "(IDirect3D9::CreateDevice ppDevice, compared with esi every frame; now %08X, behavior flags %08X%s) lost flag [%08X] in-scene [%08X] "
                      "| font: GDI '%s' (then %s, %s, %s) into a managed texture on the first visible frame, scale %d%%\n",
                 ready ? "installed" : "NOT installed", static_cast<unsigned>(base + kEndFrameRva), entryOk, loadOk, static_cast<unsigned>(base + kEndFrameCallerRva),
                 callerOk, static_cast<unsigned>(base + kEndSceneCallRva), device, deviceNow, flags, (flags & 0x10) ? ", pure device" : "",
                 static_cast<unsigned>(base + kLostFlagRva), inScene, fontFaces[0], fontFaces[1], fontFaces[2], fontFaces[3], scalePercent);
    return ready;
}

// Something is meant to be on screen (the panel, the status line, the marks) but no frame
// was drawn for 3 s: one line with every reason counter, and one when drawing resumes.
void WatchStall(FILE* log) {
    static LONG lastDrawn = -1, lastHooks = 0, lostAt = 0, mismatchAt = 0, noTargetAt = 0, smallAt = 0, blockAt = 0, fontAt = 0, faultAt = 0;
    static DWORD stuckSince = 0;
    static bool reported = false;
    const DWORD now = GetTickCount();
    const LONG drawn = InterlockedCompareExchange(&framesDrawn, 0, 0), hooks = InterlockedCompareExchange(&hookCalls, 0, 0);
    const bool onScreen = InterlockedCompareExchange(&visible, 0, 0) || InterlockedCompareExchange(&hudVisible, 0, 0) || InterlockedCompareExchange(&marksVisible, 0, 0);
    if (drawn != lastDrawn || !onScreen) {
        if (reported && drawn != lastDrawn)
            std::fprintf(log, "shop overlay drawing again after %lu ms without a frame\n", static_cast<unsigned long>(now - stuckSince));
        reported = false;
        stuckSince = now;
        lastDrawn = drawn;
        lastHooks = hooks;
        lostAt = lostSkips; mismatchAt = deviceMismatch; noTargetAt = noTarget; smallAt = smallTarget; blockAt = blockFailures; fontAt = fontFailures; faultAt = drawFaults;
        return;
    }
    if (reported || now - stuckSince < 3000) return;
    reported = true;
    std::fprintf(log,
                 "shop overlay STALL: nothing drawn for %lu ms while panel=%ld hud=%ld marks=%ld; end_scene_calls=+%ld disabled=%ld (%s) lost_skips=+%ld device_mismatch=+%ld "
                 "no_target=+%ld small_target=+%ld stateblock_failures=+%ld font_failures=+%ld faults=+%ld camera_hidden=%ld pauses=%ld retries=%ld\n",
                 static_cast<unsigned long>(now - stuckSince), InterlockedCompareExchange(&visible, 0, 0), InterlockedCompareExchange(&hudVisible, 0, 0),
                 InterlockedCompareExchange(&marksVisible, 0, 0), hooks - lastHooks, InterlockedCompareExchange(&disabled, 0, 0), disabled ? disabledWhy : "-",
                 lostSkips - lostAt, deviceMismatch - mismatchAt, noTarget - noTargetAt, smallTarget - smallAt, blockFailures - blockAt, fontFailures - fontAt,
                 drawFaults - faultAt, InterlockedCompareExchange(&cameraHidden, 0, 0), InterlockedCompareExchange(&pauses, 0, 0), InterlockedCompareExchange(&retries, 0, 0));
}

void WriteDiagnostics(FILE* log) {
    WatchStall(log);
    static LONG last = -1;
    const LONG sum = framesDrawn / 600 + lostSkips + resets + deviceMismatch + noTarget + smallTarget + blockFailures + fontBuilds + fontFailures + drawFaults + deviceChanges +
                     offscreenFrames / 60 + backBufferFailures * 29 + panelFrames / 60 +
                     (visible ? 1 : 0) * 7 + disabled * 13 + bannerShows + hudShows * 17 + curtainShows * 19 + marksShows * 23 + marksFrames / 600 + marksNoCamera / 60;
    if (sum == last) return;
    last = sum;
    const LONG drawn = InterlockedCompareExchange(&framesDrawn, 0, 0);
    std::fprintf(log, "shop overlay visible=%ld disabled=%ld hook_calls=%ld drawn=%ld hidden=%ld lost_skips=%ld resets=%ld device_mismatch=%ld no_target=%ld "
                      "small_target=%ld stateblock_failures=%ld font_builds=%ld font_failures=%ld faults=%ld device_changes=%ld draw_calls=%ld avg_us=%ld max_us=%ld "
                      "target=%ldx%ld fmt=%ld font='%s' %dpx banner_shows=%ld banner_frames=%ld hud_shows=%ld hud_frames=%ld curtain_shows=%ld curtain_frames=%ld\n",
                 visible, disabled, hookCalls, drawn, hiddenFrames, lostSkips, resets, deviceMismatch, noTarget, smallTarget, blockFailures, fontBuilds, fontFailures,
                 drawFaults, deviceChanges, drawCalls, drawn ? drawMicrosTotal / drawn : 0L, drawMicrosMax, targetW, targetH, targetFormat, font.face, font.px,
                 InterlockedCompareExchange(&bannerShows, 0, 0), InterlockedCompareExchange(&bannerFrames, 0, 0), InterlockedCompareExchange(&hudShows, 0, 0),
                 InterlockedCompareExchange(&hudFrames, 0, 0), InterlockedCompareExchange(&curtainShows, 0, 0), InterlockedCompareExchange(&curtainFrames, 0, 0));
    std::fprintf(log, "shop overlay panel_frames=%ld offscreen_frames=%ld back_buffer_draws=%ld back_buffer_failures=%ld back_buffer_fix=%d\n",
                 InterlockedCompareExchange(&panelFrames, 0, 0), InterlockedCompareExchange(&offscreenFrames, 0, 0),
                 InterlockedCompareExchange(&backBufferDraws, 0, 0), InterlockedCompareExchange(&backBufferFailures, 0, 0), backBufferFix ? 1 : 0);
    std::fprintf(log, "shop overlay hints on=%d shows=%ld frames=%ld\n", hintsOn ? 1 : 0, InterlockedCompareExchange(&hintShows, 0, 0),
                 InterlockedCompareExchange(&hintFrames, 0, 0));
    std::fprintf(log, "shop overlay marks shows=%ld frames=%ld no_camera=%ld on_screen=%ld at_edge=%ld fov=%.3f aspect=%.3f draw_thread=%ld\n",
                 InterlockedCompareExchange(&marksShows, 0, 0), InterlockedCompareExchange(&marksFrames, 0, 0), InterlockedCompareExchange(&marksNoCamera, 0, 0),
                 InterlockedCompareExchange(&marksOnScreen, 0, 0), InterlockedCompareExchange(&marksAtEdge, 0, 0),
                 InterlockedCompareExchange(&marksFovMilli, 0, 0) / 1000.0, InterlockedCompareExchange(&marksAspectMilli, 0, 0) / 1000.0,
                 InterlockedCompareExchange(&marksDrawThread, 0, 0));
}
}  // namespace overlay
