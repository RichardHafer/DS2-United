// Dead Space 2 United by Richard Hafer.
// Keyboard and mouse for the overlay's panels, the supply terminal and the debug
// menu (10.10.: not everybody has a numpad). W/S the row, A/D the tab, Q/E a
// page (the debug menu: a chapter), Space uses the row. The mouse: moving it
// selects the row it points at, a click on a tab opens it, a click on the
// selected row uses it, a click on the arrows at the right scrolls. Read only
// while the game window has the focus; the game sees none of it while a panel
// is open (focusrate::PanelHolds).
namespace panelinput {
struct Frame {
    bool up = false, down = false, left = false, right = false, pageUp = false, pageDown = false, use = false;
    int tab = -1;        // a tab clicked
    int hoverRow = -1;   // the row of the view the pointer moved onto
    int clickRow = -1;   // the row of the view clicked
    int scroll = 0;      // an arrow clicked: -1 up, +1 down
};
volatile LONG clicks = 0, hovers = 0, pointerMisses = 0;

// The pointer in render target pixels: the cursor in the game window's client
// area, scaled to what the overlay draws on.
bool MousePos(int& x, int& y) {
    uint32_t window = 0;
    if (!Read(base + focusrate::kWindowRva, &window, 4) || !window) return false;
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(window));
    POINT p{};
    RECT rc{};
    const LONG tw = InterlockedCompareExchange(&overlay::targetW, 0, 0), th = InterlockedCompareExchange(&overlay::targetH, 0, 0);
    if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0 || tw <= 0 || th <= 0) {
        InterlockedIncrement(&pointerMisses);
        return false;
    }
    x = static_cast<int>(static_cast<int64_t>(p.x) * tw / rc.right);
    y = static_cast<int>(static_cast<int64_t>(p.y) * th / rc.bottom);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= tw) x = tw - 1;
    if (y >= th) y = th - 1;
    return true;
}

struct Reader {
    coop_shop::Repeat rep[6];   // W S A D Q E
    coop_shop::Edge use, click;
    int lastX = -1, lastY = -1;
    void Reset() { *this = Reader{}; }
    Frame Read(DWORD now, bool focused) {
        Frame f;
        auto down = [&](int vk) { return focused && (GetAsyncKeyState(vk) & 0x8000) != 0; };
        static const int kKeys[6] = {'W', 'S', 'A', 'D', 'Q', 'E'};
        bool step[6];
        for (int i = 0; i < 6; ++i) step[i] = rep[i].Step(down(kKeys[i]), now);
        f.up = step[0];
        f.down = step[1];
        f.left = step[2];
        f.right = step[3];
        f.pageUp = step[4];
        f.pageDown = step[5];
        f.use = use.Pressed(down(VK_SPACE));
        const bool pressed = click.Pressed(down(VK_LBUTTON));
        int x = 0, y = 0;
        if (!focused || !MousePos(x, y)) { overlay::SetPointer(false, 0, 0); return f; }
        overlay::SetPointer(true, x, y);
        const bool moved = lastX >= 0 && (x != lastX || y != lastY);
        lastX = x;
        lastY = y;
        const overlay::HitMap m = overlay::Hits();
        if (!m.valid) return f;
        const bool inRows = x >= m.rowsX0 && x < m.rowsX1 && y >= m.rowsY0 && y < m.rowsY0 + m.rowsShown * m.rowLine;
        const int k = inRows ? (y - m.rowsY0) / m.rowLine : -1;
        if (pressed) {
            InterlockedIncrement(&clicks);
            for (int i = 0; i < m.tabCount; ++i)
                if (x >= m.tabX0[i] && x < m.tabX1[i] && y >= m.tabY0 && y < m.tabY1) f.tab = i;
            if (inRows && x >= m.arrowX0 && (k == 0 || k == m.rowsShown - 1)) f.scroll = k == 0 ? -1 : 1;
            else if (inRows) f.clickRow = m.rowFirst + k;
        } else if (moved && inRows && x < m.arrowX0) {
            f.hoverRow = m.rowFirst + k;
            InterlockedIncrement(&hovers);
        }
        return f;
    }
};
}  // namespace panelinput
