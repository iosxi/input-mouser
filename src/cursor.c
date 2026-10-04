/* ==================================================================
 * cursor.c - マウスのない PC で、カーソルを自分で描く(スレーブ側)
 *
 *  Windows は、マウスが 1 台もつながっていない PC ではカーソルを描かない
 *  (カーソルの表示カウンタが -1 から始まる)。SendInput で位置は動くが見えない。
 *  表示カウンタはスレッドごとなので、ほかのアプリの分を外から上げる手はない。
 *  そこで、透明でクリックの素通りする小窓にカーソルの形を描き、位置を追わせる
 *  (PowerToys の Mouse Without Borders の「カーソルを描く」と同じ考え)。
 *
 *  描くのは、設定が入っていて、相手から操作されている間で、Windows が
 *  カーソルを描いていない(GetCursorInfo に CURSOR_SHOWING がない)ときだけ。
 *  ini の draw_cursor=2 なら、Windows が描いていても描く(判定が合わない PC 向け)。
 *
 *  形の取り出し: 黒地と白地に DrawIconEx で 1 回ずつ描き、
 *      不透明度 = 255 − (白地 − 黒地)、色(乗算済み) = 黒地の値
 *  とする。影の半透明も取れる。白黒反転で描く形(I ビームなど)は
 *  白地のほうが暗くなるので、黒の不透明として描く。
 *
 *  位置の知らせは net スレッドから来る。毎回窓を動かすと重いので、最新の
 *  位置だけを置いて UI スレッドへ 1 通だけ知らせ、UI スレッドが動かす。
 * ================================================================== */

#include "mouser.h"

volatile LONG g_drawCursor;         /* 0 = 描かない / 1 = Windows が描いていないとき / 2 = いつも */

#define CURSOR_CLASS L"InputMouser.Cursor"

static HWND          g_cw;
static HCURSOR       g_shape;
static POINT         g_hot;
static volatile LONG g_x, g_y, g_want, g_pending;

/* ------------------------------------------------------------------ */
/*  net スレッドから                                                    */
/* ------------------------------------------------------------------ */

static BOOL system_draws_cursor(void)
{
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}

/* 操作されている間に呼ぶ。p はカーソルの位置 */
void cursor_follow(POINT p)
{
    LONG want = g_drawCursor == 2 || g_dryRun || (g_drawCursor == 1 && !system_draws_cursor());
    InterlockedExchange(&g_x, p.x);
    InterlockedExchange(&g_y, p.y);
    InterlockedExchange(&g_want, want);
    if ((want || g_cw) && !InterlockedExchange(&g_pending, 1) && g_trayWnd)
        PostMessageW(g_trayWnd, WM_APP_CURSOR, 0, 0);
}

void cursor_hide(void)
{
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
        log_printf(L"カーソルの状態: flags=%lu(%s)、形=%p、自分で描く=%s", ci.flags,
                   (ci.flags & CURSOR_SHOWING) ? L"Windows が描いている" : L"Windows は描いていない",
                   (void *)ci.hCursor, g_want ? L"はい" : L"いいえ");
}

/* ------------------------------------------------------------------ */
/*  UI スレッド                                                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK cursor_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
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
        if (g_cw) ShowWindow(g_cw, SW_HIDE);
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
}
