/* ==================================================================
 * hook.c - 入力を拾って相手へ送る(マスター側)
 *
 *  専用のスレッドで WH_MOUSE_LL / WH_KEYBOARD_LL を掛ける。
 *  フックの中では送る内容を net の待ち行列に積むだけにして、すぐ戻る
 *  (時間がかかると Windows にフックを外される)。
 *
 *  このPC を操作しているとき
 *      画面の端を越える動きを見張る。越えた先に相手がいれば切り替える。
 *      低レベル フックには、端で止められる前の座標が届く(2026-10-03 実測:
 *      右端 3839 で右へ 10 押すと pt.x = 3849)。
 *  相手を操作しているとき
 *      カーソルをメイン画面の中央に留め、透明な小窓でカーソルを隠す。
 *      入力はすべて握りつぶして送る。移動量は「フックに届いた座標 −
 *      今のカーソル位置」。握りつぶすとカーソルは動かないので、毎回
 *      中央からの差になる(加速は適用済み。実測: +7 の移動が +6/+7 で届く)。
 *      SetCursorPos はフックに流れない(実測)。
 *
 *  キーとボタンは「押したときの行き先」を覚えておき、離したときも
 *  同じ相手へ送る。切り替えをまたいでも押しっぱなしにならない。
 *
 *  自分が SendInput した入力(dwExtraInfo = INJECT_MAGIC)は素通しする。
 *  この PC がスレーブとして受けた入力を、マスターとして拾い直さないため。
 * ================================================================== */

#include "mouser.h"
#include <wtsapi32.h>

volatile LONG g_target = -1;
volatile LONG g_locked;
volatile LONG g_hotkeyCapture;

static volatile LONG g_paused;
static DWORD   g_tid;
static HANDLE  g_thread;
static HHOOK   g_mh, g_kh;
static HWND    g_hide;
static POINT   g_park;
static LONG    g_events, g_eventsSeen;
static UINT_PTR g_timer;            /* 相手を操作中だけ動かす見回り */
static Config  g_hc;                /* このスレッドが使う設定の写し */

static BYTE    g_down[256];         /* 物理的に押されているキー */
static BYTE    g_keyOwner[256];     /* 0 = このPC、1.. = 相手+1、OWNER_EATEN = ホットキー */
static BYTE    g_btnDown[5], g_btnOwner[5];
static int     g_cellX, g_cellY;
static int     g_edgeSrc = -2, g_edgeSide = SIDE_NONE;
static DWORD   g_edgeSince;

#define OWNER_EATEN 0xFF

#define HM_CONFIG  (WM_APP + 100)   /* lp = Config*(HeapAlloc) */
#define HM_SWITCH  (WM_APP + 101)   /* wp = 相手(-1 = このPC) */
#define HM_EDGE    (WM_APP + 102)   /* wp = 相手 | 辺 << 8, lp = 位置 | 角まで << 16 */
#define HM_DOWN    (WM_APP + 103)   /* wp = 相手 */
#define HM_PAUSE   (WM_APP + 104)   /* wp = 止める */
#define HM_ACTION  (WM_APP + 105)   /* wp = ホットキーの働き */
#define HM_QUIT    (WM_APP + 106)
#define HM_FORCEHOME (WM_APP + 107) /* ロック・安全なデスクトップ。押したままのキーも離させる */

enum { ACT_NONE, ACT_HOME, ACT_LOCK, ACT_PEER0 };

#define HIDE_CLASS L"InputMouser.Hide"

/* ------------------------------------------------------------------ */
/*  送る                                                                */
/* ------------------------------------------------------------------ */

static void send_move(int peer, int dx, int dy)
{
    BYTE b[8];
    b[0] = (BYTE)dx; b[1] = (BYTE)(dx >> 8); b[2] = (BYTE)(dx >> 16); b[3] = (BYTE)(dx >> 24);
    b[4] = (BYTE)dy; b[5] = (BYTE)(dy >> 8); b[6] = (BYTE)(dy >> 16); b[7] = (BYTE)(dy >> 24);
    net_send(peer, M_MOVE, b, 8);
}

static void send_key(int peer, int vk, int scan, int flags)
{
    BYTE b[5];
    b[0] = (BYTE)vk; b[1] = (BYTE)(vk >> 8);
    b[2] = (BYTE)scan; b[3] = (BYTE)(scan >> 8);
    b[4] = (BYTE)flags;
    net_send(peer, M_KEY, b, 5);
}

/* ------------------------------------------------------------------ */
/*  カーソルを留めて隠す                                                */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK hide_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATEANDEAT;
    case WM_WTSSESSION_CHANGE:
        if ((wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_CONNECT) &&
            g_target >= 0)
            PostThreadMessageW(g_tid, HM_FORCEHOME, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void hide_create(void)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = hide_proc;
    wc.hInstance     = g_inst;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = HIDE_CLASS;
    RegisterClassW(&wc);
    g_hide = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                             HIDE_CLASS, L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, g_inst, NULL);
    if (g_hide) {
        SetLayeredWindowAttributes(g_hide, 0, 1, LWA_ALPHA);    /* 見えないが当たり判定はある */
        WTSRegisterSessionNotification(g_hide, NOTIFY_FOR_THIS_SESSION);
    }
}

static void park_point(POINT *p)
{
    POINT       z = { 0, 0 };
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(z, MONITOR_DEFAULTTOPRIMARY), &mi);
    p->x = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
    p->y = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
}

static void park_begin(void)
{
    int s = 24;
    park_point(&g_park);
    if (g_hide)
        SetWindowPos(g_hide, HWND_TOPMOST, g_park.x - s / 2, g_park.y - s / 2, s, s,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetCursorPos(g_park.x, g_park.y);
    g_eventsSeen = g_events;
    if (!g_timer) g_timer = SetTimer(NULL, 0, 500, NULL);
}

static void park_end(int side, int pos)
{
    POINT p = screen_entry(side, pos);
    if (g_timer) { KillTimer(NULL, g_timer); g_timer = 0; }
    if (g_hide) ShowWindow(g_hide, SW_HIDE);
    SetCursorPos(p.x, p.y);
}

/* ------------------------------------------------------------------ */
/*  切り替え                                                            */
/* ------------------------------------------------------------------ */

static void edge_clear(void)
{
    g_edgeSrc  = -2;
    g_edgeSide = SIDE_NONE;
}

static void switch_to(int dest, int entrySide, int pos)
{
    int prev = g_target;

    if (dest == prev) return;
    edge_clear();
    if (prev >= 0) net_send(prev, M_LEAVE, NULL, 0);
    if (dest >= 0) {
        BYTE b[3];
        if (prev < 0) park_begin();
        InterlockedExchange(&g_target, dest);
        g_cellX = g_hc.peers[dest].gx;
        g_cellY = g_hc.peers[dest].gy;
        b[0] = (BYTE)entrySide;
        b[1] = (BYTE)pos; b[2] = (BYTE)(pos >> 8);
        net_send(dest, M_ENTER, b, 3);
    } else {
        InterlockedExchange(&g_target, -1);
        g_cellX = g_cellY = 0;
        park_end(entrySide, pos);
    }
    if (g_trayWnd) PostMessageW(g_trayWnd, WM_APP_SWITCHED, (WPARAM)dest, (LPARAM)prev);
}

/* 押したままのキー・ボタンの行方が分からなくなったとき(Ctrl+Alt+Del、ロック)。
   相手には全部離させ、こちらの記録も消して手元に戻る。 */
static void force_home(void)
{
    int i;
    for (i = 0; i < g_hc.npeers; i++)
        if (g_peerStatus[i] == PS_READY) net_send(i, M_RELEASE, NULL, 0);
    ZeroMemory(g_down, sizeof(g_down));
    ZeroMemory(g_keyOwner, sizeof(g_keyOwner));
    ZeroMemory(g_btnDown, sizeof(g_btnDown));
    ZeroMemory(g_btnOwner, sizeof(g_btnOwner));
    if (g_target >= 0) {
        log_printf(L"ロックか安全なデスクトップに移ったので、このPC に戻ります");
        switch_to(-1, SIDE_NONE, 0);
    }
}

static BOOL any_button(void)
{
    int i;
    for (i = 0; i < 5; i++) if (g_btnDown[i]) return TRUE;
    return FALSE;
}

/* 端を押した。切り替えたら TRUE */
static BOOL edge_push(int src, int side, int pos, int corner)
{
    static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
    static const int opposite[4] = { SIDE_RIGHT, SIDE_LEFT, SIDE_BOTTOM, SIDE_TOP };
    int nx, ny, dest;

    if (side > SIDE_BOTTOM || g_locked) return FALSE;
    if (g_hc.noDragSwitch && any_button()) return FALSE;
    if (g_hc.corner > 0 && corner < g_hc.corner) { edge_clear(); return FALSE; }

    nx = g_cellX + dx[side];
    ny = g_cellY + dy[side];
    if (nx == 0 && ny == 0) {
        dest = -1;
    } else {
        dest = peer_at(&g_hc, nx, ny);
        if (dest < 0 || g_peerStatus[dest] != PS_READY) return FALSE;
    }
    if (g_hc.edgeDelay > 0) {
        DWORD now = GetTickCount();
        if (g_edgeSrc != src || g_edgeSide != side) {
            g_edgeSrc   = src;
            g_edgeSide  = side;
            g_edgeSince = now;
            return FALSE;
        }
        if (now - g_edgeSince < (DWORD)g_hc.edgeDelay) return FALSE;
    }
    switch_to(dest, opposite[side], pos);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  フック                                                              */
/* ------------------------------------------------------------------ */

static LRESULT button(int b, BOOL down, int code, WPARAM wp, LPARAM lp)
{
    int  owner;
    BYTE m[2];

    if (down) {
        g_btnDown[b]  = 1;
        owner         = g_target < 0 ? 0 : g_target + 1;
        g_btnOwner[b] = (BYTE)owner;
    } else {
        g_btnDown[b]  = 0;
        owner         = g_btnOwner[b];
        g_btnOwner[b] = 0;
    }
    if (!owner) return CallNextHookEx(NULL, code, wp, lp);
    m[0] = (BYTE)b;
    m[1] = (BYTE)down;
    net_send(owner - 1, M_BUTTON, m, 2);
    return 1;
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp)
{
    MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
    int t;

    if (code != HC_ACTION || m->dwExtraInfo == INJECT_MAGIC) return CallNextHookEx(NULL, code, wp, lp);
    g_events++;
    t = g_target;

    switch (wp) {
    case WM_MOUSEMOVE: {
        POINT cur;
        GetCursorPos(&cur);
        if (t < 0) {
            POINT np;
            int   pos = 0, corner = 0, side;
            if (g_locked) break;
            side = screen_step(cur, m->pt, &np, &pos, &corner);
            if (side == SIDE_NONE) { if (g_edgeSrc == -1) edge_clear(); break; }
            if (edge_push(-1, side, pos, corner)) return 1;
            break;
        }
        if (m->pt.x != cur.x || m->pt.y != cur.y) send_move(t, m->pt.x - cur.x, m->pt.y - cur.y);
        return 1;
    }
    case WM_LBUTTONDOWN: return button(0, TRUE,  code, wp, lp);
    case WM_LBUTTONUP:   return button(0, FALSE, code, wp, lp);
    case WM_RBUTTONDOWN: return button(1, TRUE,  code, wp, lp);
    case WM_RBUTTONUP:   return button(1, FALSE, code, wp, lp);
    case WM_MBUTTONDOWN: return button(2, TRUE,  code, wp, lp);
    case WM_MBUTTONUP:   return button(2, FALSE, code, wp, lp);
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return button(HIWORD(m->mouseData) == XBUTTON1 ? 3 : 4, wp == WM_XBUTTONDOWN, code, wp, lp);
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (t >= 0) {
            BYTE  b[3];
            SHORT d = (SHORT)HIWORD(m->mouseData);
            b[0] = wp == WM_MOUSEHWHEEL;
            b[1] = (BYTE)d; b[2] = (BYTE)((WORD)d >> 8);
            net_send(t, M_WHEEL, b, 3);
            return 1;
        }
        break;
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static int mods_now(void)
{
    int m = 0;
    if (g_down[VK_LCONTROL] || g_down[VK_RCONTROL]) m |= HK_CTRL;
    if (g_down[VK_LMENU]    || g_down[VK_RMENU])    m |= HK_ALT;
    if (g_down[VK_LSHIFT]   || g_down[VK_RSHIFT])   m |= HK_SHIFT;
    if (g_down[VK_LWIN]     || g_down[VK_RWIN])     m |= HK_WIN;
    return m;
}

static int hotkey_action(int vk)
{
    int m = mods_now(), i;
    if (g_hc.hkHome.vk == vk && g_hc.hkHome.mods == m) return ACT_HOME;
    if (g_hc.hkLock.vk == vk && g_hc.hkLock.mods == m) return ACT_LOCK;
    for (i = 0; i < g_hc.npeers; i++)
        if (g_hc.peers[i].hk.vk == vk && g_hc.peers[i].hk.mods == m) return ACT_PEER0 + i;
    return ACT_NONE;
}

/* Alt や Win を押したままホットキーを食べると、離したときに
   メニューやスタートが開く。間に意味のないキーを挟んで防ぐ。 */
static void mask_modifiers(void)
{
    static const BYTE mods[] = { VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    BOOL done[PEER_MAX + 1];
    int  i;

    ZeroMemory(done, sizeof(done));
    for (i = 0; i < (int)ARRAYSIZE(mods); i++) {
        int o = g_keyOwner[mods[i]];
        if (!g_down[mods[i]] || o == OWNER_EATEN || o > PEER_MAX || done[o]) continue;
        done[o] = TRUE;
        if (o == 0) {
            INPUT in[2];
            ZeroMemory(in, sizeof(in));
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = 0xE8;             /* 割り当てのない仮想キー */
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            in[0].ki.dwExtraInfo = in[1].ki.dwExtraInfo = INJECT_MAGIC;
            SendInput(2, in, sizeof(INPUT));
        } else {
            send_key(o - 1, 0xE8, 0, 0);
            send_key(o - 1, 0xE8, 0, IKF_UP);
        }
    }
}

static LRESULT CALLBACK key_proc(int code, WPARAM wp, LPARAM lp)
{
    KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
    int  vk, owner, flags;
    BOOL up;

    if (code != HC_ACTION || k->dwExtraInfo == INJECT_MAGIC) return CallNextHookEx(NULL, code, wp, lp);
    g_events++;
    vk = (int)(k->vkCode & 0xFF);
    up = (k->flags & LLKHF_UP) != 0;

    if (!up) {
        BOOL repeat = g_down[vk];
        g_down[vk] = 1;
        if (!repeat && !g_hotkeyCapture) {
            int act = hotkey_action(vk);
            if (act != ACT_NONE) {
                g_keyOwner[vk] = OWNER_EATEN;
                mask_modifiers();
                PostThreadMessageW(g_tid, HM_ACTION, (WPARAM)act, 0);
                return 1;
            }
        }
        if (!repeat || !g_keyOwner[vk]) g_keyOwner[vk] = g_target < 0 ? 0 : (BYTE)(g_target + 1);
        owner = g_keyOwner[vk];
    } else {
        g_down[vk]     = 0;
        owner          = g_keyOwner[vk];
        g_keyOwner[vk] = 0;
    }
    if (owner == OWNER_EATEN) return 1;
    if (owner == 0) return CallNextHookEx(NULL, code, wp, lp);

    flags = (up ? IKF_UP : 0) | ((k->flags & LLKHF_EXTENDED) ? IKF_EXT : 0);
    if (vk == VK_PACKET) {
        flags |= IKF_UNICODE;
        flags &= ~IKF_EXT;
    }
    send_key(owner - 1, vk, (int)k->scanCode, flags);
    return 1;
}

static void hooks_set(BOOL on)
{
    if (on) {
        if (!g_mh) g_mh = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, g_inst, 0);
        if (!g_kh) g_kh = SetWindowsHookExW(WH_KEYBOARD_LL, key_proc, g_inst, 0);
        if (!g_mh || !g_kh) log_printf(L"フックを掛けられませんでした (%lu)", GetLastError());
    } else {
        if (g_target >= 0) switch_to(-1, SIDE_NONE, 0);
        if (g_mh) { UnhookWindowsHookEx(g_mh); g_mh = NULL; }
        if (g_kh) { UnhookWindowsHookEx(g_kh); g_kh = NULL; }
        ZeroMemory(g_down, sizeof(g_down));
        ZeroMemory(g_keyOwner, sizeof(g_keyOwner));
        ZeroMemory(g_btnDown, sizeof(g_btnDown));
        ZeroMemory(g_btnOwner, sizeof(g_btnOwner));
    }
}

static void hooks_update(void)
{
    hooks_set(!g_paused && g_hc.npeers > 0);
}

/* 相手を操作中の見回り。フックが外されていないか、カーソルが留まっているか */
static void watchdog(void)
{
    POINT c;
    HDESK d;
    if (g_target < 0) return;
    /* Ctrl+Alt+Del や UAC の画面(安全なデスクトップ)は開けない */
    d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) { force_home(); return; }
    CloseDesktop(d);
    if (GetCursorPos(&c) && (abs(c.x - g_park.x) > 2 || abs(c.y - g_park.y) > 2)) {
        if (g_events == g_eventsSeen) {
            /* 入力が来ないのにカーソルが動いた = フックが効いていない */
            log_printf(L"フックが働いていないようなので掛け直します");
            if (g_mh) { UnhookWindowsHookEx(g_mh); g_mh = NULL; }
            if (g_kh) { UnhookWindowsHookEx(g_kh); g_kh = NULL; }
            hooks_set(TRUE);
        }
        park_begin();
    } else if (g_hide) {
        SetWindowPos(g_hide, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    g_eventsSeen = g_events;
}

static BOOL peers_moved(const Config *a, const Config *b)
{
    int i;
    if (a->npeers != b->npeers) return TRUE;
    for (i = 0; i < a->npeers; i++)
        if (lstrcmpiW(a->peers[i].host, b->peers[i].host) || a->peers[i].port != b->peers[i].port)
            return TRUE;
    return FALSE;
}

static DWORD WINAPI hook_thread(LPVOID arg)
{
    MSG msg;

    g_hc = *(Config *)arg;
    HeapFree(GetProcessHeap(), 0, arg);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);    /* 行列を作る */
    hide_create();
    hooks_update();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd) { DispatchMessageW(&msg); continue; }
        switch (msg.message) {
        case WM_TIMER:
            watchdog();
            break;
        case HM_CONFIG: {
            Config *c = (Config *)msg.lParam;
            if (g_target >= 0 && peers_moved(c, &g_hc)) switch_to(-1, SIDE_NONE, 0);
            g_hc = *c;
            HeapFree(GetProcessHeap(), 0, c);
            if (g_target >= 0) { g_cellX = g_hc.peers[g_target].gx; g_cellY = g_hc.peers[g_target].gy; }
            hooks_update();
            break;
        }
        case HM_SWITCH: {
            int p = (int)(INT_PTR)msg.wParam;
            if (p >= 0 && (p >= g_hc.npeers || g_peerStatus[p] != PS_READY)) break;
            if (p >= 0 && (!g_mh || !g_kh)) break;      /* 一時停止中 */
            switch_to(p, SIDE_NONE, 0);
            break;
        }
        case HM_ACTION: {
            int a = (int)msg.wParam;
            if (a == ACT_HOME) switch_to(-1, SIDE_NONE, 0);
            else if (a == ACT_LOCK) hook_toggle_lock();
            else if (a >= ACT_PEER0) {
                int p = a - ACT_PEER0;
                if (p < g_hc.npeers && g_peerStatus[p] == PS_READY) switch_to(p, SIDE_NONE, 0);
            }
            break;
        }
        case HM_EDGE: {
            int peer = (int)(msg.wParam & 0xFF), side = (int)((msg.wParam >> 8) & 0xFF);
            if (peer != g_target) break;            /* 切り替える前の相手からの遅れた知らせ */
            if (side == SIDE_NONE) { if (g_edgeSrc == peer) edge_clear(); break; }
            edge_push(peer, side, (int)(msg.lParam & 0xFFFF), (int)((msg.lParam >> 16) & 0xFFFF));
            break;
        }
        case HM_DOWN:
            if ((int)msg.wParam == g_target) switch_to(-1, SIDE_NONE, 0);
            break;
        case HM_PAUSE:
            InterlockedExchange(&g_paused, (LONG)msg.wParam);
            hooks_update();
            if (g_trayWnd) PostMessageW(g_trayWnd, WM_APP_STATUS, 0, 0);
            break;
        case HM_FORCEHOME:
            force_home();
            break;
        case HM_QUIT:
            hooks_set(FALSE);
            PostQuitMessage(0);
            break;
        }
    }
    if (g_hide) {
        WTSUnRegisterSessionNotification(g_hide);
        DestroyWindow(g_hide);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  外から                                                              */
/* ------------------------------------------------------------------ */

static void post_config(UINT msg)
{
    Config *c = (Config *)HeapAlloc(GetProcessHeap(), 0, sizeof(Config));
    if (!c) return;
    *c = g_cfg;
    if (msg == 0) {
        g_thread = CreateThread(NULL, 0, hook_thread, c, 0, &g_tid);
        if (!g_thread) HeapFree(GetProcessHeap(), 0, c);
    } else if (!g_tid || !PostThreadMessageW(g_tid, msg, 0, (LPARAM)c)) {
        HeapFree(GetProcessHeap(), 0, c);
    }
}

void hook_start(void)          { post_config(0); }
void hook_config_changed(void) { post_config(HM_CONFIG); }

void hook_stop(void)
{
    if (!g_thread) return;
    PostThreadMessageW(g_tid, HM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_tid = 0;
}

void hook_switch(int peer)
{
    if (g_tid) PostThreadMessageW(g_tid, HM_SWITCH, (WPARAM)(INT_PTR)peer, 0);
}

void hook_set_paused(BOOL on)
{
    if (g_tid) PostThreadMessageW(g_tid, HM_PAUSE, (WPARAM)on, 0);
}

BOOL hook_paused(void) { return g_paused != 0; }

void hook_toggle_lock(void)
{
    LONG now = !g_locked;
    InterlockedExchange(&g_locked, now);
    log_printf(now ? L"切り替えを固定しました" : L"切り替えの固定を解きました");
    if (g_trayWnd) PostMessageW(g_trayWnd, WM_APP_STATUS, 2, now);
}

void hook_peer_edge(int peer, int side, int pos, int cornerDist)
{
    if (g_tid)
        PostThreadMessageW(g_tid, HM_EDGE, (WPARAM)((peer & 0xFF) | ((side & 0xFF) << 8)),
                           (LPARAM)((pos & 0xFFFF) | ((LPARAM)(cornerDist & 0xFFFF) << 16)));
}

void hook_peer_down(int peer)
{
    if (g_tid) PostThreadMessageW(g_tid, HM_DOWN, (WPARAM)peer, 0);
}
