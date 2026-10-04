/* ==================================================================
 * inject.c - 受けた入力の再現(スレーブ側。net スレッドで動く)
 *
 *  マウスは「移動量」で受け取り、こちらで位置を足して絶対座標で
 *  動かす。相対移動で SendInput すると、こちらの加速がもう一度
 *  かかってしまうため(マスターの移動量は加速済み)。
 *
 *  絶対座標の換算は画素の中心を指す  ((2d+1)·65536) / (2w)。
 *  単純な d·65535/(w-1) は 3840 幅で 1px ずれる点が出た
 *  (2026-10-03 実測。中心の式は 1281 点すべて一致)。
 *
 *  キーはスキャン コードで再現する(物理的なキーの位置を伝える。
 *  配列の解釈はこの PC の設定に従う)。スキャン コードの無いキーや、
 *  スキャン コードでは別のキーになるもの(Pause、メディア キー)は
 *  仮想キー コードで送る。
 *
 *  -dryrun のときは再現せずログに書く(1920×1080 の仮の画面を使う)。
 * ================================================================== */

#include "mouser.h"

static int   g_active = -1;     /* 操作中のマスターの接続 ID */
static BOOL  g_edgeOut;         /* 端を越えたと知らせた */
static BYTE  g_keyDown[256];
static WORD  g_keyScan[256];
static BYTE  g_keyFlags[256];
static BYTE  g_btnDown[5];
static POINT g_dryPos = { 960, 540 };
static const RECT k_dryScreen = { 0, 0, 1920, 1080 };

int inj_active_conn(void) { return g_active; }

/* ------------------------------------------------------------------ */
/*  画面の形                                                            */
/* ------------------------------------------------------------------ */

void screen_virtual(RECT *r)
{
    if (g_dryRun) { *r = k_dryScreen; return; }
    r->left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r->top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r->right  = r->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r->bottom = r->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

static BOOL mon_rect(POINT p, DWORD flags, RECT *out)
{
    HMONITOR    hm;
    MONITORINFO mi;

    if (g_dryRun) {
        if (!PtInRect(&k_dryScreen, p) && flags == MONITOR_DEFAULTTONULL) return FALSE;
        *out = k_dryScreen;
        return TRUE;
    }
    hm = MonitorFromPoint(p, flags);
    if (!hm) return FALSE;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return FALSE;
    *out = mi.rcMonitor;
    return TRUE;
}

static POINT clamp_pt(POINT p, const RECT *r)
{
    if (p.x < r->left)       p.x = r->left;
    if (p.x > r->right - 1)  p.x = r->right - 1;
    if (p.y < r->top)        p.y = r->top;
    if (p.y > r->bottom - 1) p.y = r->bottom - 1;
    return p;
}

static int ratio(int v, int lo, int len)
{
    LONGLONG r;
    if (len <= 1) return 32768;
    r = (LONGLONG)(v - lo) * 65535 / (len - 1);
    return r < 0 ? 0 : r > 65535 ? 65535 : (int)r;
}

int screen_step(POINT cur, POINT t, POINT *out, int *pos, int *cornerDist)
{
    RECT  m, o, v;
    POINT c, probe;
    int   ox, oy, side;

    if (mon_rect(t, MONITOR_DEFAULTTONULL, &m)) { *out = t; return SIDE_NONE; }
    mon_rect(cur, MONITOR_DEFAULTTONEAREST, &m);
    c  = clamp_pt(t, &m);
    ox = t.x < m.left ? m.left - t.x : t.x >= m.right  ? t.x - (m.right - 1)  : 0;
    oy = t.y < m.top  ? m.top - t.y  : t.y >= m.bottom ? t.y - (m.bottom - 1) : 0;
    if (ox >= oy && ox > 0) side = t.x < m.left ? SIDE_LEFT : SIDE_RIGHT;
    else                    side = t.y < m.top  ? SIDE_TOP  : SIDE_BOTTOM;

    /* その先に別のディスプレイがあれば、そちらへ移る */
    probe = c;
    switch (side) {
    case SIDE_LEFT:   probe.x = m.left - 1;   break;
    case SIDE_RIGHT:  probe.x = m.right;      break;
    case SIDE_TOP:    probe.y = m.top - 1;    break;
    default:          probe.y = m.bottom;     break;
    }
    if (mon_rect(probe, MONITOR_DEFAULTTONULL, &o)) {
        *out = clamp_pt(t, &o);
        return SIDE_NONE;
    }

    *out = c;
    screen_virtual(&v);
    if (side == SIDE_LEFT || side == SIDE_RIGHT) {
        *pos        = ratio(c.y, v.top, v.bottom - v.top);
        *cornerDist = min(c.y - m.top, m.bottom - 1 - c.y);
    } else {
        *pos        = ratio(c.x, v.left, v.right - v.left);
        *cornerDist = min(c.x - m.left, m.right - 1 - c.x);
    }
    return side;
}

POINT screen_entry(int side, int pos)
{
    RECT  v, m;
    POINT p;

    screen_virtual(&v);
    switch (side) {
    case SIDE_LEFT:
    case SIDE_RIGHT:
        p.x = side == SIDE_LEFT ? v.left : v.right - 1;
        p.y = v.top + (int)((LONGLONG)pos * (v.bottom - v.top - 1) / 65535);
        break;
    case SIDE_TOP:
    case SIDE_BOTTOM:
        p.y = side == SIDE_TOP ? v.top : v.bottom - 1;
        p.x = v.left + (int)((LONGLONG)pos * (v.right - v.left - 1) / 65535);
        break;
    default: {                          /* ホットキーで来たときはメインの中央 */
        POINT z = { 0, 0 };
        mon_rect(z, MONITOR_DEFAULTTOPRIMARY, &m);
        p.x = (m.left + m.right) / 2;
        p.y = (m.top + m.bottom) / 2;
        return p;
    }
    }
    mon_rect(p, MONITOR_DEFAULTTONEAREST, &m);
    return clamp_pt(p, &m);
}

/* ------------------------------------------------------------------ */
/*  再現                                                                */
/* ------------------------------------------------------------------ */

static void move_abs(POINT p)
{
    INPUT in;
    RECT  v;
    POINT now;

    if (g_dryRun) { g_dryPos = p; return; }
    screen_virtual(&v);
    ZeroMemory(&in, sizeof(in));
    in.type           = INPUT_MOUSE;
    in.mi.dx          = (LONG)(((LONGLONG)(2 * (p.x - v.left) + 1) * 65536) / (2 * (LONGLONG)(v.right - v.left)));
    in.mi.dy          = (LONG)(((LONGLONG)(2 * (p.y - v.top) + 1) * 65536) / (2 * (LONGLONG)(v.bottom - v.top)));
    in.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo = INJECT_MAGIC;
    SendInput(1, &in, sizeof(in));
    /* 念のため。ずれていれば直接置く */
    if (GetCursorPos(&now) && (now.x != p.x || now.y != p.y)) SetCursorPos(p.x, p.y);
}

static POINT cursor_now(void)
{
    POINT p;
    if (g_dryRun) return g_dryPos;
    if (!GetCursorPos(&p)) p = g_dryPos;
    return p;
}

static void send_edge(int conn, int side, int pos, int corner)
{
    BYTE b[5];
    b[0] = (BYTE)side;
    b[1] = (BYTE)pos; b[2] = (BYTE)(pos >> 8);
    b[3] = (BYTE)corner; b[4] = (BYTE)(corner >> 8);
    net_send(conn, M_EDGE, b, sizeof(b));
}

void inj_enter(int conn, int side, int pos)
{
    POINT p;
    g_active  = conn;
    g_edgeOut = FALSE;
    p = screen_entry(side, pos);
    move_abs(p);
    cursor_follow(p);
    cursor_log_state();
    if (g_dryRun) log_printf(L"[dryrun] enter side=%d pos=%d -> (%d,%d)", side, pos, p.x, p.y);
}

void inj_leave(int conn)
{
    if (g_dryRun) log_printf(L"[dryrun] leave");
    if (conn == g_active) { g_active = -1; cursor_hide(); }
}

void inj_move(int conn, int dx, int dy)
{
    POINT cur = cursor_now(), t, np;
    int   pos = 0, corner = 0, side;

    t.x = cur.x + dx;
    t.y = cur.y + dy;
    side = screen_step(cur, t, &np, &pos, &corner);
    if (np.x != cur.x || np.y != cur.y) move_abs(np);
    if (conn == g_active) cursor_follow(np);
    if (g_dryRun) log_printf(L"[dryrun] move %d,%d -> (%d,%d)%s", dx, dy, np.x, np.y,
                             side != SIDE_NONE ? L" edge" : L"");
    if (side != SIDE_NONE) {
        send_edge(conn, side, pos, min(corner, 65535));
        g_edgeOut = TRUE;
    } else if (g_edgeOut) {
        send_edge(conn, SIDE_NONE, 0, 0);
        g_edgeOut = FALSE;
    }
}

void inj_button(int btn, BOOL down)
{
    static const DWORD k_down[5] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                     MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN };
    static const DWORD k_up[5]   = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP,
                                     MOUSEEVENTF_XUP, MOUSEEVENTF_XUP };
    INPUT in;

    if (btn < 0 || btn > 4) return;
    g_btnDown[btn] = (BYTE)down;
    if (g_active >= 0) cursor_follow(cursor_now());     /* 出てきたメニューの上へ上げ直す */
    if (g_dryRun) { log_printf(L"[dryrun] button %d %s", btn, down ? L"down" : L"up"); return; }
    ZeroMemory(&in, sizeof(in));
    in.type           = INPUT_MOUSE;
    in.mi.dwFlags     = down ? k_down[btn] : k_up[btn];
    in.mi.mouseData   = btn == 3 ? XBUTTON1 : btn == 4 ? XBUTTON2 : 0;
    in.mi.dwExtraInfo = INJECT_MAGIC;
    SendInput(1, &in, sizeof(in));
}

void inj_wheel(BOOL horizontal, int delta)
{
    INPUT in;
    if (g_dryRun) { log_printf(L"[dryrun] wheel %s %d", horizontal ? L"h" : L"v", delta); return; }
    ZeroMemory(&in, sizeof(in));
    in.type           = INPUT_MOUSE;
    in.mi.dwFlags     = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData   = (DWORD)delta;
    in.mi.dwExtraInfo = INJECT_MAGIC;
    SendInput(1, &in, sizeof(in));
}

static BOOL by_vk(int vk)
{
    return vk == VK_PAUSE || vk == VK_CANCEL ||
           (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2);     /* ブラウザ・メディア・起動キー */
}

static void key_out(int vk, int scan, int flags)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type           = INPUT_KEYBOARD;
    in.ki.dwExtraInfo = INJECT_MAGIC;
    if (flags & IKF_UNICODE) {
        in.ki.wScan   = (WORD)scan;
        in.ki.dwFlags = KEYEVENTF_UNICODE;
    } else if (scan && !by_vk(vk)) {
        in.ki.wScan   = (WORD)scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
    } else {
        in.ki.wVk     = (WORD)vk;
        in.ki.wScan   = (WORD)scan;
    }
    if (flags & IKF_EXT) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (flags & IKF_UP)       in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

void inj_key(int vk, int scan, int flags)
{
    vk &= 0xFF;
    if (!(flags & IKF_UNICODE)) {
        if (flags & IKF_UP) g_keyDown[vk] = 0;
        else { g_keyDown[vk] = 1; g_keyScan[vk] = (WORD)scan; g_keyFlags[vk] = (BYTE)flags; }
    }
    if (g_dryRun) {
        log_printf(L"[dryrun] key vk=%02X scan=%02X %s%s", vk, scan, (flags & IKF_UP) ? L"up" : L"down",
                   (flags & IKF_EXT) ? L" ext" : L"");
        return;
    }
    key_out(vk, scan, flags);
}

void inj_release_all(void)
{
    int i, n = 0;
    for (i = 0; i < 256; i++) {
        if (!g_keyDown[i]) continue;
        g_keyDown[i] = 0;
        if (!g_dryRun) key_out(i, g_keyScan[i], (g_keyFlags[i] & ~IKF_UP) | IKF_UP);
        n++;
    }
    for (i = 0; i < 5; i++) if (g_btnDown[i]) { inj_button(i, FALSE); n++; }
    if (n) log_printf(L"押されたままのキー・ボタン %d 個を離しました", n);
}

void inj_conn_closed(int conn)
{
    if (conn == g_active || g_active < 0) inj_release_all();
    if (conn == g_active) { g_active = -1; cursor_hide(); }
}
