/* ==================================================================
 * ui_common.c - ダイアログ共通の見た目と小物
 *
 *  ・見出しの太字と、Windows 11 のタスク ダイアログ風のフッタ帯
 *    (kotemado と同じ作り)
 *  ・確認やお知らせ(TaskDialog)
 *  ・ホットキーの入力欄(エディットを置き換える。キーを押すとそのまま入る)
 *  ・切り替えたときの小さな知らせ(画面下の中央に少しだけ出る)
 * ================================================================== */

#include "mouser.h"
#include "resource.h"
#include <dwmapi.h>
#include <imm.h>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

/* ------------------------------------------------------------------ */
/*  見出しとフッタ帯                                                    */
/* ------------------------------------------------------------------ */

void dlg_look_init(HWND dlg, DlgLook *lk, const int *ids, int n, int anchorId)
{
    LOGFONTW lf;
    HFONT    base = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
    RECT     r, pad = { 0, 0, 0, 7 };
    int      i;

    lk->heading = NULL;
    if (base && GetObjectW(base, sizeof(lf), &lf)) {
        lf.lfWeight = FW_SEMIBOLD;
        lf.lfHeight = MulDiv(lf.lfHeight, 118, 100);
        lk->heading = CreateFontIndirectW(&lf);
    }
    for (i = 0; i < n; i++)
        if (lk->heading) SendDlgItemMessageW(dlg, ids[i], WM_SETFONT, (WPARAM)lk->heading, TRUE);

    lk->footerTop = 0;
    if (anchorId) {
        GetWindowRect(GetDlgItem(dlg, anchorId), &r);
        MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
        MapDialogRect(dlg, &pad);
        lk->footerTop = r.top - pad.bottom;
    }
    SetPropW(dlg, L"mouser.footer", (HANDLE)(INT_PTR)lk->footerTop);
}

void dlg_look_free(DlgLook *lk)
{
    if (lk->heading) DeleteObject(lk->heading);
    lk->heading = NULL;
}

BOOL dlg_look_erase(HWND dlg, HDC dc, const DlgLook *lk)
{
    RECT c, f;
    GetClientRect(dlg, &c);
    f = c;
    if (lk->footerTop > 0) {
        c.bottom = lk->footerTop;
        f.top    = lk->footerTop;
        FillRect(dc, &f, theme_footer_brush());
        {
            HBRUSH line = CreateSolidBrush(theme_line());
            RECT   l = f;
            l.bottom = l.top + 1;
            FillRect(dc, &l, line);
            DeleteObject(line);
        }
    }
    FillRect(dc, &c, theme_back_brush());
    return TRUE;
}

int ui_message(HWND owner, const WCHAR *main, const WCHAR *content,
               TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon)
{
    TASKDIALOGCONFIG tc;
    int pressed = IDCANCEL;

    ZeroMemory(&tc, sizeof(tc));
    tc.cbSize             = sizeof(tc);
    tc.hwndParent         = owner;
    tc.hInstance          = g_inst;
    tc.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW |
                            TDF_SIZE_TO_CONTENT;
    tc.pszWindowTitle     = APP_NAME;
    tc.pszMainIcon        = icon;
    tc.pszMainInstruction = main;
    tc.pszContent         = content;
    tc.dwCommonButtons    = buttons ? buttons : TDCBF_OK_BUTTON;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) pressed = IDCANCEL;
    return pressed;
}

void ui_set_icons(HWND h)
{
    HICON big = NULL, small = NULL;
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big);
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &small);
    if (big)   SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small);
}

const WCHAR *peer_status_text(int st)
{
    switch (st) {
    case PS_CONNECTING:  return L"接続しています…";
    case PS_READY:       return L"接続済み";
    case PS_UNREACHABLE: return L"見つかりません";
    case PS_BADPASS:     return L"パスワードが違います";
    case PS_REFUSED:     return L"版が合いません";
    }
    return L"未接続";
}

/* ------------------------------------------------------------------ */
/*  ホットキーの入力欄                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    Hotkey hk;
    int    held;        /* 押している途中の修飾キー */
} HkEdit;

static int held_mods(void)
{
    int m = 0;
    if (GetKeyState(VK_CONTROL) < 0) m |= HK_CTRL;
    if (GetKeyState(VK_MENU) < 0)    m |= HK_ALT;
    if (GetKeyState(VK_SHIFT) < 0)   m |= HK_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= HK_WIN;
    return m;
}

static void hk_show(HWND h, const HkEdit *e, BOOL partial)
{
    WCHAR t[96];
    if (partial && e->held) {
        Hotkey tmp = { 0, 0 };
        t[0] = 0;
        tmp.mods = (BYTE)e->held;
        if (tmp.mods & HK_CTRL)  lstrcatW(t, L"Ctrl + ");
        if (tmp.mods & HK_ALT)   lstrcatW(t, L"Alt + ");
        if (tmp.mods & HK_SHIFT) lstrcatW(t, L"Shift + ");
        if (tmp.mods & HK_WIN)   lstrcatW(t, L"Win + ");
    } else {
        hotkey_format(&e->hk, t, ARRAYSIZE(t));
    }
    SetWindowTextW(h, t);
    SendMessageW(h, EM_SETSEL, lstrlenW(t), lstrlenW(t));
}

static BOOL is_modifier(WPARAM vk)
{
    return vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LSHIFT || vk == VK_RSHIFT;
}

static LRESULT CALLBACK hk_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    HkEdit *e = (HkEdit *)ref;

    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_SETFOCUS: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        InterlockedExchange(&g_hotkeyCapture, 1);
        HideCaret(h);
        return r;
    }
    case WM_KILLFOCUS:
        InterlockedExchange(&g_hotkeyCapture, 0);
        e->held = 0;
        hk_show(h, e, FALSE);
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int mods = held_mods();
        if (wp == VK_TAB && !(mods & (HK_CTRL | HK_ALT | HK_WIN))) {
            SendMessageW(GetParent(h), WM_NEXTDLGCTL, (mods & HK_SHIFT) != 0, 0);
            return 0;
        }
        if (wp == VK_ESCAPE && !mods) {
            PostMessageW(GetParent(h), WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
        if (is_modifier(wp)) {
            e->held = mods;
            hk_show(h, e, TRUE);
            return 0;
        }
        if ((wp == VK_BACK || wp == VK_DELETE) && !mods) {
            e->hk.vk = 0; e->hk.mods = 0;
        } else {
            /* 文字キーを修飾なしで登録すると、ふだんの入力ができなくなる */
            BOOL plain = (wp >= '0' && wp <= 'Z') || wp == VK_SPACE || wp == VK_RETURN ||
                         (wp >= VK_OEM_1 && wp <= VK_OEM_102) || (wp >= VK_NUMPAD0 && wp <= VK_DIVIDE);
            if (plain && !(mods & (HK_CTRL | HK_ALT | HK_WIN))) { MessageBeep(MB_OK); return 0; }
            e->hk.vk   = (BYTE)wp;
            e->hk.mods = (BYTE)mods;
        }
        e->held = 0;
        hk_show(h, e, FALSE);
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(id, EN_CHANGE), (LPARAM)h);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        e->held = held_mods() & ~(wp == VK_CONTROL ? HK_CTRL : wp == VK_MENU ? HK_ALT :
                                  wp == VK_SHIFT ? HK_SHIFT : 0);
        hk_show(h, e, TRUE);
        return 0;

    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_CONTEXTMENU:
    case WM_IME_STARTCOMPOSITION:
        return 0;

    case WM_NCDESTROY:
        RemoveWindowSubclass(h, hk_proc, id);
        HeapFree(GetProcessHeap(), 0, e);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void hotkey_edit_attach(HWND edit, const Hotkey *hk)
{
    HkEdit *e = (HkEdit *)GetPropW(edit, L"mouser.hk");
    if (!e) {
        e = (HkEdit *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HkEdit));
        if (!e) return;
        SetPropW(edit, L"mouser.hk", e);
        SetWindowSubclass(edit, hk_proc, (UINT_PTR)GetDlgCtrlID(edit), (DWORD_PTR)e);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"なし");
        ImmAssociateContextEx(edit, NULL, 0);      /* 日本語入力を使わせない */
    }
    e->hk = *hk;
    hk_show(edit, e, FALSE);
}

void hotkey_edit_get(HWND edit, Hotkey *hk)
{
    HkEdit *e = (HkEdit *)GetPropW(edit, L"mouser.hk");
    if (e) *hk = e->hk;
    else   hk->vk = hk->mods = 0;
}

/* ------------------------------------------------------------------ */
/*  切り替えたときの小さな知らせ                                        */
/* ------------------------------------------------------------------ */

#define OSD_CLASS L"InputMouser.Osd"
#define OSD_MS    1300
static HWND  g_osd;
static WCHAR g_osdText[128];

static LRESULT CALLBACK osd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC    dc = BeginPaint(h, &ps);
        RECT   c;
        BOOL   dark = theme_is_dark();
        HBRUSH bg = CreateSolidBrush(dark ? RGB(43, 43, 43) : RGB(249, 249, 249));
        HFONT  f, of;
        UINT   dpi = GetDpiForWindow(h);

        GetClientRect(h, &c);
        FillRect(dc, &c, bg);
        DeleteObject(bg);
        f = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                        0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
        of = (HFONT)SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dark ? RGB(255, 255, 255) : RGB(26, 26, 26));
        DrawTextW(dc, g_osdText, -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(h, 1);
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void osd_show(const WCHAR *text)
{
    POINT       z = { 0, 0 };
    MONITORINFO mi;
    HDC         dc;
    HFONT       f, of;
    SIZE        sz;
    UINT        dpi;
    int         w, hgt, x, y;

    if (!g_cfg.osd) return;
    if (!g_osd) {
        WNDCLASSW wc;
        DWORD     corner = 2;     /* 丸める */
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = osd_proc;
        wc.hInstance     = g_inst;
        wc.lpszClassName = OSD_CLASS;
        RegisterClassW(&wc);
        g_osd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                WS_EX_TRANSPARENT, OSD_CLASS, APP_NAME, WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, g_inst, NULL);
        if (!g_osd) return;
        SetLayeredWindowAttributes(g_osd, 0, 240, LWA_ALPHA);
        DwmSetWindowAttribute(g_osd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }
    lstrcpynW(g_osdText, text, ARRAYSIZE(g_osdText));

    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(z, MONITOR_DEFAULTTOPRIMARY), &mi);
    {
        UINT dx = 96, dy = 96;
        HMODULE sh = GetModuleHandleW(L"shcore.dll");
        typedef HRESULT (WINAPI *fnGetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
        fnGetDpiForMonitor p = sh ? (fnGetDpiForMonitor)(void *)GetProcAddress(sh, "GetDpiForMonitor") : NULL;
        if (p && SUCCEEDED(p(MonitorFromPoint(z, MONITOR_DEFAULTTOPRIMARY), 0, &dx, &dy))) dpi = dx;
        else dpi = GetDpiForSystem();
    }
    dc = GetDC(NULL);
    f  = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                     0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    of = (HFONT)SelectObject(dc, f);
    GetTextExtentPoint32W(dc, g_osdText, lstrlenW(g_osdText), &sz);
    SelectObject(dc, of);
    DeleteObject(f);
    ReleaseDC(NULL, dc);

    w   = sz.cx + MulDiv(48, dpi, 96);
    hgt = sz.cy + MulDiv(24, dpi, 96);
    x   = (mi.rcWork.left + mi.rcWork.right - w) / 2;
    y   = mi.rcWork.bottom - hgt - MulDiv(48, dpi, 96);
    SetWindowPos(g_osd, HWND_TOPMOST, x, y, w, hgt, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_osd, NULL, TRUE);
    SetTimer(g_osd, 1, OSD_MS, NULL);
}
