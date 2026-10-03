/* ==================================================================
 * ui_layout.c - 画面の配置図
 *
 *  Windows 11 の「ディスプレイの配置」に寄せた図。このPC を真ん中に、
 *  相手の PC をます目に並べる。タイルをドラッグして置き場所を変える
 *  (相手のいるますに落とすと入れ替わる)。キーボードでは矢印で動かす。
 *
 *  図形は 2 倍の大きさで描いてから縮め(HALFTONE)、角の丸みを
 *  なめらかにする。文字は縮めるとにじむので、縮めた後に描く。
 *
 *  g_cfg を直接読み、並びを変えたときは g_cfg を書き換えてから
 *  親に LN_MOVED を知らせる(保存は親が行う)。
 * ================================================================== */

#include "mouser.h"

typedef struct {
    int   sel;              /* 選んでいる相手。-1 = なし(このPC) */
    int   hot;              /* マウスが乗っている相手 */
    BOOL  pressed, dragging;
    int   dragPeer;
    POINT down, grab, mouse;
    int   dropX, dropY;     /* 落とす先のます */
    /* 図の大きさ(ドラッグ中は変えない) */
    int   minX, minY, cols, rows, cw, ch, ox, oy;
    UINT  dpi;
    HFONT fName, fNameS, fSub;
} Layout;

static int S(const Layout *l, int v) { return MulDiv(v, l->dpi, 96); }

static COLORREF accent(void)
{
    /* Windows 11 の既定のアクセント(ライト #005FB8 / ダーク #60CDFF) */
    return theme_is_dark() ? RGB(96, 205, 255) : RGB(0, 95, 184);
}
static COLORREF canvas_back(void) { return theme_is_dark() ? RGB(39, 39, 39)  : RGB(243, 243, 243); }
static COLORREF tile_back(void)   { return theme_is_dark() ? RGB(55, 55, 55)  : RGB(255, 255, 255); }
static COLORREF tile_line(void)   { return theme_is_dark() ? RGB(80, 80, 80)  : RGB(204, 204, 204); }
static COLORREF tile_hot(void)    { return theme_is_dark() ? RGB(64, 64, 64)  : RGB(249, 249, 249); }

static COLORREF status_color(int st)
{
    BOOL d = theme_is_dark();
    switch (st) {
    case PS_READY:      return d ? RGB(108, 203, 95) : RGB(15, 123, 15);
    case PS_BADPASS:
    case PS_REFUSED:
    case PS_UNREACHABLE: return d ? RGB(255, 153, 164) : RGB(196, 43, 28);
    }
    return d ? RGB(157, 157, 157) : RGB(138, 138, 138);
}

/* ------------------------------------------------------------------ */
/*  形                                                                  */
/* ------------------------------------------------------------------ */

static void make_fonts(Layout *l)
{
    if (l->fName)  DeleteObject(l->fName);
    if (l->fNameS) DeleteObject(l->fNameS);
    if (l->fSub)   DeleteObject(l->fSub);
    l->fName  = CreateFontW(-S(l, 13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    l->fNameS = CreateFontW(-S(l, 11), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    l->fSub  = CreateFontW(-S(l, 11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
}

static void geometry(HWND h, Layout *l)
{
    RECT c;
    int  i, maxX = 0, maxY = 0, w, hgt, cw;

    l->minX = l->minY = 0;
    for (i = 0; i < g_cfg.npeers; i++) {
        const Peer *p = &g_cfg.peers[i];
        if (p->gx < l->minX) l->minX = p->gx;
        if (p->gy < l->minY) l->minY = p->gy;
        if (p->gx > maxX) maxX = p->gx;
        if (p->gy > maxY) maxY = p->gy;
    }
    l->cols = maxX - l->minX + 1;
    l->rows = maxY - l->minY + 1;

    /* まわりに半ますずつ余白を残す(ドラッグで外側へ置けるように) */
    GetClientRect(h, &c);
    w   = c.right - S(l, 8);
    hgt = c.bottom - S(l, 8);
    cw  = min(w * 2 / (2 * l->cols + 1), hgt * 16 * 2 / 10 / (2 * l->rows + 1));
    cw  = min(cw, S(l, 176));
    l->cw = cw;
    l->ch = cw * 10 / 16;
    l->ox = (c.right - cw * l->cols) / 2;
    l->oy = (c.bottom - l->ch * l->rows) / 2;
}

static RECT cell_rect(const Layout *l, int gx, int gy)
{
    RECT r;
    int  g = max(S(l, 5), l->cw / 22);
    r.left   = l->ox + (gx - l->minX) * l->cw + g;
    r.top    = l->oy + (gy - l->minY) * l->ch + g;
    r.right  = r.left + l->cw - 2 * g;
    r.bottom = r.top + l->ch - 2 * g;
    return r;
}

static void cell_at(const Layout *l, POINT p, int *gx, int *gy)
{
    int x = p.x - l->ox, y = p.y - l->oy;
    *gx = (x >= 0 ? x / l->cw : (x - l->cw + 1) / l->cw) + l->minX;
    *gy = (y >= 0 ? y / l->ch : (y - l->ch + 1) / l->ch) + l->minY;
}

/* -1 = このPC、0.. = 相手、-2 = なにもない */
static int hit(const Layout *l, POINT p)
{
    int  i;
    RECT r = cell_rect(l, 0, 0);
    for (i = 0; i < g_cfg.npeers; i++) {
        RECT t = cell_rect(l, g_cfg.peers[i].gx, g_cfg.peers[i].gy);
        if (PtInRect(&t, p)) return i;
    }
    return PtInRect(&r, p) ? -1 : -2;
}

/* ------------------------------------------------------------------ */
/*  描画                                                                */
/* ------------------------------------------------------------------ */

static void rrect(HDC dc, RECT r, int k, int radius, COLORREF fill, COLORREF line, int width, BOOL dash)
{
    HBRUSH br  = fill == CLR_NONE ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN   pen;
    HGDIOBJ ob, op;

    if (line == CLR_NONE) pen = (HPEN)GetStockObject(NULL_PEN);
    else if (dash) {
        LOGBRUSH lb = { BS_SOLID, line, 0 };
        DWORD    style[2] = { (DWORD)(4 * k), (DWORD)(3 * k) };
        pen = ExtCreatePen(PS_GEOMETRIC | PS_USERSTYLE | PS_ENDCAP_FLAT, width * k, &lb, 2, style);
    } else pen = CreatePen(PS_INSIDEFRAME, width * k, line);
    ob = SelectObject(dc, br);
    op = SelectObject(dc, pen);
    RoundRect(dc, r.left * k, r.top * k, r.right * k, r.bottom * k, radius * 2 * k, radius * 2 * k);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (fill != CLR_NONE) DeleteObject(br);
    if (line != CLR_NONE) DeleteObject(pen);
}

static RECT drag_rect(const Layout *l)
{
    RECT r = cell_rect(l, g_cfg.peers[l->dragPeer].gx, g_cfg.peers[l->dragPeer].gy);
    OffsetRect(&r, l->mouse.x - l->grab.x - r.left, l->mouse.y - l->grab.y - r.top);
    return r;
}

/* タイルの図形(2 倍の DC に描く) */
static void tile_shape(HWND h, const Layout *l, HDC dc, int k, int who, RECT r)
{
    int  rad = S(l, 6);
    BOOL focus = GetFocus() == h;

    if (who < 0) {
        rrect(dc, r, k, rad, accent(), CLR_NONE, 1, FALSE);
    } else {
        BOOL hot = l->hot == who && !l->dragging;
        rrect(dc, r, k, rad, hot ? tile_hot() : tile_back(), tile_line(), 1, FALSE);
        if (g_target == who) {          /* 操作中 */
            RECT b = r;
            InflateRect(&b, -S(l, 1), -S(l, 1));
            rrect(dc, b, k, rad, CLR_NONE, accent(), max(2, S(l, 2)), FALSE);
        }
        {   /* 状態の点 */
            int  d = S(l, 8);
            RECT dot;
            HBRUSH br = CreateSolidBrush(status_color(g_peerStatus[who]));
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
            dot.left   = r.left + S(l, 10);
            dot.top    = r.bottom - S(l, 10) - d;
            Ellipse(dc, dot.left * k, dot.top * k, (dot.left + d) * k, (dot.top + d) * k);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(br);
        }
    }
    if (l->sel == who && who >= 0) {
        RECT f = r;
        InflateRect(&f, S(l, 3), S(l, 3));
        rrect(dc, f, k, rad + S(l, 3), CLR_NONE, focus ? theme_text() : tile_line(), max(2, S(l, 2)), FALSE);
    }
}

static void tile_text(const Layout *l, HDC dc, int who, RECT r)
{
    RECT  t = r;
    WCHAR sub[64];
    const WCHAR *name;
    int   pad = S(l, 10);

    SetBkMode(dc, TRANSPARENT);
    InflateRect(&t, -pad, 0);
    if (who < 0) {
        name = g_hostName[0] ? g_hostName : L"このPC";
        lstrcpyW(sub, L"このPC");
        SetTextColor(dc, theme_is_dark() ? RGB(0, 0, 0) : RGB(255, 255, 255));
    } else {
        name = g_cfg.peers[who].host;
        lstrcpynW(sub, g_target == who ? L"操作中" : peer_status_text(g_peerStatus[who]), ARRAYSIZE(sub));
        SetTextColor(dc, theme_text());
    }
    t.top = r.top + S(l, 9);
    {   /* 長い名前は一段小さい字で収める */
        SIZE sz;
        SelectObject(dc, l->fName);
        GetTextExtentPoint32W(dc, name, lstrlenW(name), &sz);
        if (sz.cx > t.right - t.left) SelectObject(dc, l->fNameS);
    }
    DrawTextW(dc, name, -1, &t, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SelectObject(dc, l->fSub);
    t = r;
    t.left  += who < 0 ? pad : pad + S(l, 13);
    t.right -= pad;
    t.bottom -= S(l, 6);
    if (who >= 0) SetTextColor(dc, theme_dim_text());
    DrawTextW(dc, sub, -1, &t, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void paint(HWND h, Layout *l, HDC out)
{
    RECT    c;
    int     W, H, i, k = 2;
    HDC     d1, d2;
    HBITMAP b1, b2;
    HGDIOBJ o1, o2;

    GetClientRect(h, &c);
    W = c.right; H = c.bottom;
    if (W <= 0 || H <= 0) return;
    if (!l->dragging) geometry(h, l);

    d1 = CreateCompatibleDC(out);
    d2 = CreateCompatibleDC(out);
    b1 = CreateCompatibleBitmap(out, W, H);
    b2 = CreateCompatibleBitmap(out, W * k, H * k);
    o1 = SelectObject(d1, b1);
    o2 = SelectObject(d2, b2);

    /* --- 図形(2 倍) --- */
    {
        HBRUSH bg = CreateSolidBrush(theme_back());
        RECT   all = { 0, 0, W * k, H * k };
        RECT   cv = c;
        FillRect(d2, &all, bg);
        DeleteObject(bg);
        rrect(d2, cv, k, S(l, 8), canvas_back(), theme_line(), 1, FALSE);
    }
    if (l->dragging) {                      /* 置ける場所 */
        int gx, gy;
        for (gy = l->minY - 1; gy <= l->minY + l->rows; gy++)
            for (gx = l->minX - 1; gx <= l->minX + l->cols; gx++) {
                RECT r = cell_rect(l, gx, gy);
                BOOL target = gx == l->dropX && gy == l->dropY && !(gx == 0 && gy == 0);
                if (gx == 0 && gy == 0) continue;
                rrect(d2, r, k, S(l, 6), CLR_NONE, target ? accent() : tile_line(),
                      target ? max(2, S(l, 2)) : 1, !target);
            }
    }
    tile_shape(h, l, d2, k, -1, cell_rect(l, 0, 0));
    for (i = 0; i < g_cfg.npeers; i++) {
        if (l->dragging && i == l->dragPeer) continue;
        tile_shape(h, l, d2, k, i, cell_rect(l, g_cfg.peers[i].gx, g_cfg.peers[i].gy));
    }
    if (l->dragging) tile_shape(h, l, d2, k, l->dragPeer, drag_rect(l));

    SetStretchBltMode(d1, HALFTONE);
    SetBrushOrgEx(d1, 0, 0, NULL);
    StretchBlt(d1, 0, 0, W, H, d2, 0, 0, W * k, H * k, SRCCOPY);

    /* --- 文字(等倍) --- */
    {
        HGDIOBJ of = SelectObject(d1, l->fName);
        tile_text(l, d1, -1, cell_rect(l, 0, 0));
        for (i = 0; i < g_cfg.npeers; i++) {
            if (l->dragging && i == l->dragPeer) continue;
            tile_text(l, d1, i, cell_rect(l, g_cfg.peers[i].gx, g_cfg.peers[i].gy));
        }
        if (l->dragging) tile_text(l, d1, l->dragPeer, drag_rect(l));
        if (!g_cfg.npeers) {
            RECT t = c;
            t.bottom -= S(l, 10);
            SelectObject(d1, l->fSub);
            SetTextColor(d1, theme_dim_text());
            DrawTextW(d1, L"「PC を追加」で、操作したい PC を登録します", -1, &t,
                      DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(d1, of);
    }

    BitBlt(out, 0, 0, W, H, d1, 0, 0, SRCCOPY);
    SelectObject(d1, o1);
    SelectObject(d2, o2);
    DeleteObject(b1);
    DeleteObject(b2);
    DeleteDC(d1);
    DeleteDC(d2);
}

/* ------------------------------------------------------------------ */
/*  操作                                                                */
/* ------------------------------------------------------------------ */

static void notify(HWND h, int code)
{
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), code), (LPARAM)h);
}

/* 相手 p を (gx, gy) へ。ほかの相手がいれば入れ替える */
static BOOL move_peer(int p, int gx, int gy)
{
    int other;
    if (p < 0 || p >= g_cfg.npeers || (gx == 0 && gy == 0)) return FALSE;
    if (gx < -15 || gx > 15 || gy < -15 || gy > 15) return FALSE;
    if (g_cfg.peers[p].gx == gx && g_cfg.peers[p].gy == gy) return FALSE;
    other = peer_at(&g_cfg, gx, gy);
    if (other >= 0) {
        g_cfg.peers[other].gx = g_cfg.peers[p].gx;
        g_cfg.peers[other].gy = g_cfg.peers[p].gy;
    }
    g_cfg.peers[p].gx = gx;
    g_cfg.peers[p].gy = gy;
    return TRUE;
}

static LRESULT CALLBACK layout_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Layout *l = (Layout *)GetWindowLongPtrW(h, 0);

    switch (msg) {
    case WM_NCCREATE:
        l = (Layout *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Layout));
        if (!l) return FALSE;
        l->sel = -1;
        l->hot = -2;
        l->dpi = GetDpiForWindow(h);
        if (!l->dpi) l->dpi = 96;
        make_fonts(l);
        SetWindowLongPtrW(h, 0, (LONG_PTR)l);
        break;

    case WM_NCDESTROY:
        if (l) {
            if (l->fName)  DeleteObject(l->fName);
            if (l->fNameS) DeleteObject(l->fNameS);
            if (l->fSub)   DeleteObject(l->fSub);
            HeapFree(GetProcessHeap(), 0, l);
        }
        break;

    case WM_DPICHANGED_AFTERPARENT:
        l->dpi = GetDpiForWindow(h);
        make_fonts(l);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, l, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int   w = hit(l, p);
        SetFocus(h);
        if (l->sel != (w >= 0 ? w : -1)) {
            l->sel = w >= 0 ? w : -1;
            notify(h, LN_SELCHANGE);
        }
        InvalidateRect(h, NULL, FALSE);
        if (msg == WM_LBUTTONDBLCLK) {
            if (w >= 0) notify(h, LN_ACTIVATE);
            return 0;
        }
        if (w >= 0) {
            RECT r = cell_rect(l, g_cfg.peers[w].gx, g_cfg.peers[w].gy);
            l->pressed  = TRUE;
            l->dragPeer = w;
            l->down     = p;
            l->grab.x   = p.x - r.left;
            l->grab.y   = p.y - r.top;
            SetCapture(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (l->pressed) {
            if (!l->dragging && (abs(p.x - l->down.x) > GetSystemMetrics(SM_CXDRAG) ||
                                 abs(p.y - l->down.y) > GetSystemMetrics(SM_CYDRAG)))
                l->dragging = TRUE;
            if (l->dragging) {
                RECT  r;
                POINT mid;
                l->mouse = p;
                r = drag_rect(l);
                mid.x = (r.left + r.right) / 2;
                mid.y = (r.top + r.bottom) / 2;
                cell_at(l, mid, &l->dropX, &l->dropY);
                InvalidateRect(h, NULL, FALSE);
            }
        } else {
            int w = hit(l, p);
            if (w != l->hot) {
                TRACKMOUSEEVENT tm = { sizeof(tm), TME_LEAVE, h, 0 };
                l->hot = w;
                TrackMouseEvent(&tm);
                InvalidateRect(h, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (l->hot != -2) { l->hot = -2; InvalidateRect(h, NULL, FALSE); }
        return 0;

    case WM_LBUTTONUP:
        if (l->pressed) {
            BOOL drop = l->dragging;
            l->pressed = l->dragging = FALSE;
            ReleaseCapture();
            if (drop && move_peer(l->dragPeer, l->dropX, l->dropY)) notify(h, LN_MOVED);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (l->pressed && (HWND)lp != h) {
            l->pressed = l->dragging = FALSE;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            if (l->sel < 0) {
                if (g_cfg.npeers) { l->sel = 0; notify(h, LN_SELCHANGE); InvalidateRect(h, NULL, FALSE); }
            } else {
                const Peer *p = &g_cfg.peers[l->sel];
                int gx = p->gx + (wp == VK_LEFT ? -1 : wp == VK_RIGHT ? 1 : 0);
                int gy = p->gy + (wp == VK_UP ? -1 : wp == VK_DOWN ? 1 : 0);
                if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_SHIFT) < 0) {
                    /* Ctrl/Shift + 矢印は選び替え */
                    l->sel = (l->sel + ((wp == VK_RIGHT || wp == VK_DOWN) ? 1 : g_cfg.npeers - 1)) % g_cfg.npeers;
                    notify(h, LN_SELCHANGE);
                } else if (gx == 0 && gy == 0) {
                    gx += gx - p->gx; gy += gy - p->gy;     /* このPC を飛び越える */
                    if (move_peer(l->sel, gx, gy)) notify(h, LN_MOVED);
                } else if (move_peer(l->sel, gx, gy)) {
                    notify(h, LN_MOVED);
                }
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        case VK_DELETE:
            if (l->sel >= 0) notify(h, LN_DELETE);
            return 0;
        case VK_ESCAPE:
            if (l->pressed) { l->pressed = l->dragging = FALSE; ReleaseCapture(); InvalidateRect(h, NULL, FALSE); return 0; }
            break;
        }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void layout_register(void)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = layout_proc;
    wc.cbWndExtra    = sizeof(void *);
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = LAYOUT_CLASS;
    RegisterClassW(&wc);
}

int layout_selected(HWND h)
{
    Layout *l = (Layout *)GetWindowLongPtrW(h, 0);
    return l && l->sel < g_cfg.npeers ? l->sel : -1;
}

void layout_select(HWND h, int peer)
{
    Layout *l = (Layout *)GetWindowLongPtrW(h, 0);
    if (!l) return;
    l->sel = peer < g_cfg.npeers ? peer : -1;
    InvalidateRect(h, NULL, FALSE);
}
