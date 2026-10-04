/* ==================================================================
 * cursor.c - マウスのない PC で、カーソルを自分で描く(スレーブ側)
 *
 *  Windows は、マウスが 1 台もつながっていない PC ではカーソルを描かない
 *  (カーソルの表示カウンタが -1 から始まる)。SendInput で位置は動くが見えない。
 *  表示カウンタはスレッドごとなので、ほかのアプリの分を外から上げる手はない。
 *  そこで、透明でクリックの素通りする小窓にカーソルの形を描き、位置を追わせる
 *  (PowerToys の Mouse Without Borders の「カーソルを描く」と同じ考え)。
 *
 *  v8 からの既定(draw_cursor=1)は「マウスキー」を使う。
 *      Windows の補助機能のマウスキーが有効だと、マウスがなくても Windows 自身が
 *      本物のカーソルを描く(Amazon DCV の公式文書の回避策。Steam Link や Synergy の
 *      利用者の報告も同じ)。描くのが Windows なので、右クリックのメニューなど
 *      後から手前に出る窓の上でも、形が変わっても本物どおりに出る。
 *      操作されている間だけ SystemParametersInfo(SPI_SETMOUSEKEYS) で有効にし、
 *      離れたら元に戻す。SPIF_UPDATEINIFILE を付けないのでレジストリには書かない
 *      (2026-10-04 実測: 有効にしている最中も HKCU の MouseKeys\Flags は 62 のまま)。
 *      通知領域の表示は出さない。
 *  v9: v8 は「Num Lock が切れているときだけ働く」にしていたが、遠隔の利用者の環境で
 *      カーソルが出なくなった(Num Lock はふつう入っているので、マウスキーが有効でも
 *      働いていなかったと考えている。他社の回避策は Windows の既定=Num Lock が入って
 *      いるときに働く、のまま有効にしている)。そこで「今の Num Lock の状態でいつも
 *      働く」ように合わせ、Num Lock が変わったら合わせ直す。代わりに、操作されている
 *      間はテンキーがマウスキーに使われる。
 *      さらに、それでも Windows が描いていないときは自分でも描く(v7 の描き方。
 *      Windows が描き始めたら引っ込める)。どちらに転んでもカーソルは見える。
 *  draw_cursor=2 は「自分で描く」だけ(マウスキーを使わない)。
 *  自分で描くのは、操作されている間で Windows がカーソルを描いていないとき
 *  (-dryrun では常に描く)。
 *
 *  形の取り出し: 黒地と白地に DrawIconEx で 1 回ずつ描き、
 *      不透明度 = 255 − (白地 − 黒地)、色(乗算済み) = 黒地の値
 *  とする。影の半透明も取れる。白黒反転で描く形(I ビームなど)は
 *  白地のほうが暗くなるので、黒の不透明として描く。
 *
 *  位置の知らせは net スレッドから来る。毎回窓を動かすと重いので、最新の
 *  位置だけを置いて UI スレッドへ 1 通だけ知らせ、UI スレッドが動かす。
 *
 *  v7: 右クリックのメニューは最前面の窓として後から出るので、描いたカーソルの
 *  上に重なる(2026-10-04 実測: 開いた直後はメニューが上、上げ直すと描いた方が上)。
 *  描いている間は 150ms ごとと、ボタンを押したときにも最前面へ上げ直す。
 *  また「描くか」は操作が来るたびではなく、操作の間は描き続ける(途中で Windows の
 *  答えが変わっても引っ込めない。描いていなかったときだけ、途中からでも描き始める)。
 * ================================================================== */

#include "mouser.h"

volatile LONG g_drawCursor;         /* 0 = しない / 1 = マウスキーで Windows に描かせる / 2 = 自分で描く */

#define CURSOR_CLASS L"InputMouser.Cursor"

static HWND          g_cw;
static HCURSOR       g_shape;
static POINT         g_hot;
static volatile LONG g_x, g_y, g_want, g_pending;
static LONG          g_session;     /* この操作の間は描く(net スレッドだけが触る) */
static LONG          g_lastFlags = -1;

#define RAISE_MS 150

/* ------------------------------------------------------------------ */
/*  net スレッドから                                                    */
/* ------------------------------------------------------------------ */

static BOOL system_draws_cursor(void)
{
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}

/* ------------------------------------------------------------------ */
/*  マウスキー(draw_cursor=1)                                          */
/* ------------------------------------------------------------------ */

static MOUSEKEYS g_mkSaved;
static BOOL      g_mkChanged;
static int       g_mkNumLock = -1;  /* 合わせた Num Lock の状態 */

static int numlock_on(void) { return GetKeyState(VK_NUMLOCK) & 1; }    /* 通信スレッドからも読める(実測) */

/* Num Lock の今の状態で働くように設定する */
static BOOL mousekeys_apply(void)
{
    MOUSEKEYS mk = g_mkSaved;
    int       nl = numlock_on();
    mk.cbSize  = sizeof(mk);
    /* ショートカット キーなど利用者の好みは残し、通知領域の表示は外す */
    mk.dwFlags = (g_mkSaved.dwFlags & (MKF_HOTKEYACTIVE | MKF_CONFIRMHOTKEY | MKF_HOTKEYSOUND | MKF_MODIFIERS)) |
                 MKF_MOUSEKEYSON | MKF_AVAILABLE | (nl ? MKF_REPLACENUMBERS : 0);
    if (!SystemParametersInfoW(SPI_SETMOUSEKEYS, sizeof(mk), &mk, 0)) return FALSE;     /* 保存しない */
    g_mkNumLock = nl;
    return TRUE;
}

/* Num Lock が変わっていたら合わせ直す(操作の途中で呼ぶ) */
static void mousekeys_sync(void)
{
    if (g_mkChanged && numlock_on() != g_mkNumLock && mousekeys_apply())
        log_printf(L"Num Lock が%sになったので、マウスキーを合わせ直しました", g_mkNumLock ? L"入" : L"切");
}

void mousekeys_begin(void)
{
    MOUSEKEYS mk;
    if (g_mkChanged) return;
    mk.cbSize = sizeof(mk);
    if (!SystemParametersInfoW(SPI_GETMOUSEKEYS, sizeof(mk), &mk, 0)) return;
    if (mk.dwFlags & MKF_MOUSEKEYSON) return;           /* 利用者が自分で有効にしている */
    g_mkSaved = mk;
    if (mousekeys_apply()) {
        g_mkChanged = TRUE;
        log_printf(L"マウスキーを有効にしました(操作されている間だけ。Num Lock %s で働く。元 0x%08lX)",
                   g_mkNumLock ? L"入" : L"切", g_mkSaved.dwFlags);
    } else {
        log_printf(L"マウスキーを有効にできませんでした (%lu)", GetLastError());
    }
}

void mousekeys_end(void)
{
    if (!g_mkChanged) return;
    g_mkChanged = FALSE;
    g_mkNumLock = -1;
    if (SystemParametersInfoW(SPI_SETMOUSEKEYS, sizeof(g_mkSaved), &g_mkSaved, 0))
        log_printf(L"マウスキーを元に戻しました");
    else
        log_printf(L"マウスキーを元に戻せませんでした (%lu)", GetLastError());
}

/* ------------------------------------------------------------------ */
/*  自分で描く(draw_cursor=2)                                          */
/* ------------------------------------------------------------------ */

/* 操作されている間に呼ぶ。p はカーソルの位置 */
void cursor_follow(POINT p)
{
    LONG want;
    if (g_drawCursor != 1 && g_mkChanged) mousekeys_end();    /* 設定を変えた */
    if (g_drawCursor == 1) {
        /* マウスキーで Windows が描いていればそれに任せ、描いていなければ自分でも描く。
           動くたびに見直す(マウスキーが効き始めたら引っ込める) */
        mousekeys_sync();
        g_session = g_dryRun || !system_draws_cursor();
    } else if (!g_session && (g_drawCursor == 2 || (g_dryRun && g_drawCursor))) {
        if (g_drawCursor == 2 && !g_dryRun && system_draws_cursor()) {
            /* Windows が描いているなら描かない(マウスがつながった PC) */
        } else {
            g_session = 1;
        }
    }
    if (g_drawCursor && g_cfg.log) {   /* Windows の答えが変わったら記録する(調べるため) */
        CURSORINFO ci;
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (LONG)(ci.flags & CURSOR_SHOWING) != g_lastFlags) {
            if (g_lastFlags >= 0)
                log_printf(L"カーソルの状態が変わりました: %s", (ci.flags & CURSOR_SHOWING)
                           ? L"Windows が描いている" : L"Windows は描いていない");
            g_lastFlags = (LONG)(ci.flags & CURSOR_SHOWING);
        }
    }
    want = g_session;
    InterlockedExchange(&g_x, p.x);
    InterlockedExchange(&g_y, p.y);
    InterlockedExchange(&g_want, want);
    if ((want || g_cw) && !InterlockedExchange(&g_pending, 1) && g_trayWnd)
        PostMessageW(g_trayWnd, WM_APP_CURSOR, 0, 0);
}

void cursor_hide(void)
{
    mousekeys_end();
    g_session   = 0;
    g_lastFlags = -1;
    InterlockedExchange(&g_want, 0);
    if (g_cw && !InterlockedExchange(&g_pending, 1) && g_trayWnd)
        PostMessageW(g_trayWnd, WM_APP_CURSOR, 0, 0);
}

/* 調べるときのために、Windows が描いているかをログに出す */
void cursor_log_state(void)
{
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    if (!g_drawCursor) return;
    if (GetCursorInfo(&ci))
        log_printf(L"カーソルの状態: flags=%lu(%s)、形=%p、方式=%s", ci.flags,
                   (ci.flags & CURSOR_SHOWING) ? L"Windows が描いている" : L"Windows は描いていない",
                   (void *)ci.hCursor, g_drawCursor == 1 ? L"マウスキー＋足りなければ自分で描く" :
                   g_session ? L"自分で描く" : L"描かない");
}

/* ------------------------------------------------------------------ */
/*  UI スレッド                                                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK cursor_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_TIMER) {          /* 後から出たメニューなどの上へ上げ直す */
        if (IsWindowVisible(h))
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        else
            KillTimer(h, 1);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 32bpp の DIB を作る(上から下へ並ぶ向き) */
static HBITMAP dib32(HDC dc, int w, int h, DWORD **bits)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

/* 形を窓に描き込む */
static BOOL render(HCURSOR c)
{
    ICONINFO ii;
    BITMAP   bm;
    int      w, h, i;
    HDC      scr, d1, d2;
    HBITMAP  b1, b2;
    DWORD   *p1, *p2;
    HGDIOBJ  o1, o2;
    BOOL     ok = FALSE;

    if (!GetIconInfo(c, &ii)) return FALSE;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight; }
    else if (GetObjectW(ii.hbmMask, sizeof(bm), &bm))            { w = bm.bmWidth; h = bm.bmHeight / 2; }
    else w = h = 0;
    g_hot.x = (LONG)ii.xHotspot;
    g_hot.y = (LONG)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return FALSE;

    scr = GetDC(NULL);
    d1 = CreateCompatibleDC(scr);
    d2 = CreateCompatibleDC(scr);
    b1 = dib32(scr, w, h, &p1);
    b2 = dib32(scr, w, h, &p2);
    if (b1 && b2) {
        o1 = SelectObject(d1, b1);
        o2 = SelectObject(d2, b2);
        for (i = 0; i < w * h; i++) { p1[i] = 0x000000; p2[i] = 0xFFFFFF; }
        DrawIconEx(d1, 0, 0, c, w, h, 0, NULL, DI_NORMAL);
        DrawIconEx(d2, 0, 0, c, w, h, 0, NULL, DI_NORMAL);
        GdiFlush();
        for (i = 0; i < w * h; i++) {
            int k, a = 0, ch[3];
            for (k = 0; k < 3; k++) {
                int blk = (p1[i] >> (8 * k)) & 0xFF, wht = (p2[i] >> (8 * k)) & 0xFF;
                int al  = 255 - (wht - blk);
                if (al > 255) {             /* 白黒反転の点。黒で描く */
                    al = 255;
                    blk = 0;
                }
                if (al > a) a = al;
                ch[k] = blk;
            }
            for (k = 0; k < 3; k++) if (ch[k] > a) ch[k] = a;      /* 乗算済みの範囲に収める */
            p1[i] = ((DWORD)a << 24) | ((DWORD)ch[2] << 16) | ((DWORD)ch[1] << 8) | (DWORD)ch[0];
        }
        {
            POINT         src = { 0, 0 }, pos = { g_x - g_hot.x, g_y - g_hot.y };
            SIZE          sz = { w, h };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            ok = UpdateLayeredWindow(g_cw, scr, &pos, &sz, d1, &src, 0, &bf, ULW_ALPHA);
        }
        SelectObject(d1, o1);
        SelectObject(d2, o2);
    }
    if (b1) DeleteObject(b1);
    if (b2) DeleteObject(b2);
    DeleteDC(d1);
    DeleteDC(d2);
    ReleaseDC(NULL, scr);
    return ok;
}

void cursor_apply(void)
{
    CURSORINFO ci;
    HCURSOR    shape;

    InterlockedExchange(&g_pending, 0);
    if (!g_want) {
        if (g_cw) { KillTimer(g_cw, 1); ShowWindow(g_cw, SW_HIDE); }
        return;
    }
    if (!g_cw) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = cursor_proc;
        wc.hInstance     = g_inst;
        wc.lpszClassName = CURSOR_CLASS;
        RegisterClassW(&wc);
        g_cw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                               WS_EX_TRANSPARENT, CURSOR_CLASS, APP_NAME, WS_POPUP,
                               0, 0, 0, 0, NULL, NULL, g_inst, NULL);
        if (!g_cw) return;
    }
    /* 形: Windows が覚えている今の形(描いていなくても入っていることがある)。なければ矢印 */
    ci.cbSize = sizeof(ci);
    shape = GetCursorInfo(&ci) && ci.hCursor ? ci.hCursor : LoadCursorW(NULL, IDC_ARROW);
    if (shape != g_shape) {
        if (render(shape)) g_shape = shape;
        else if (shape != LoadCursorW(NULL, IDC_ARROW) && render(LoadCursorW(NULL, IDC_ARROW)))
            g_shape = shape;        /* 取り出せない形は矢印で代える */
    }
    SetWindowPos(g_cw, HWND_TOPMOST, g_x - g_hot.x, g_y - g_hot.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(g_cw, 1, RAISE_MS, NULL);      /* 描いている間だけ動く */
}
