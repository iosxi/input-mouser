/* ==================================================================
 * ui_main.c - 設定画面
 *
 *  変更はその場で input-mouser.ini に保存し、フックと通信にもすぐ効かせる。
 *  「OK / 適用」の区別は設けない(kotemado と同じ)。
 *  数値の欄は、欄を離れたときと画面を閉じるときに取り込む。
 * ================================================================== */

#include "mouser.h"
#include "resource.h"

static HWND    g_main;
static DlgLook g_look;
static BOOL    g_filling;
HWND           g_modal;         /* 開いている子ダイアログ(配色の切り替え用) */

static const int k_headings[] = { IDC_H_LAYOUT, IDC_H_THIS, IDC_H_GENERAL, IDC_H_SWITCH };

static HWND layout(void) { return GetDlgItem(g_main, IDC_LAYOUT); }

static void update_buttons(void)
{
    int s = layout_selected(layout());
    EnableWindow(GetDlgItem(g_main, IDC_EDITPEER), s >= 0);
    EnableWindow(GetDlgItem(g_main, IDC_DEL),      s >= 0);
    EnableWindow(GetDlgItem(g_main, IDC_ADD),      g_cfg.npeers < PEER_MAX);
}

static void update_info(void)
{
    WCHAR t[MAX_PATH + 64], names[256];

    if (!g_main) return;
    if (!g_cfg.accept) {
        wsprintfW(t, L"この PC の名前: %s\nほかの PC からは操作できません。", g_hostName);
    } else {
        net_incoming_names(names, ARRAYSIZE(names));
        wsprintfW(t, L"この PC の名前: %s\n%s%s", g_hostName,
                  names[0] ? L"操作している PC: " : L"いま操作している PC はありません。", names);
    }
    SetDlgItemTextW(g_main, IDC_THIS_INFO, t);
    SetDlgItemTextW(g_main, IDC_PASS_STATE, g_cfg.haveKey ? L"設定済み" : L"未設定");
    EnableWindow(GetDlgItem(g_main, IDC_PORT), g_cfg.accept);
    EnableWindow(GetDlgItem(g_main, IDC_PORT_LBL), g_cfg.accept);
}

static void fill(void)
{
    WCHAR t[MAX_PATH + 32];

    g_filling = TRUE;
    CheckDlgButton(g_main, IDC_ACCEPT,  g_cfg.accept ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(g_main, IDC_PORT,   (UINT)g_cfg.port, FALSE);
    SetDlgItemInt(g_main, IDC_DELAY,  (UINT)g_cfg.edgeDelay, FALSE);
    SetDlgItemInt(g_main, IDC_CORNER, (UINT)g_cfg.corner, FALSE);
    CheckDlgButton(g_main, IDC_NODRAG,  g_cfg.noDragSwitch ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_CLIP,    g_cfg.clipboard ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_STARTUP, startup_enabled() ? BST_CHECKED : BST_UNCHECKED);
    hotkey_edit_attach(GetDlgItem(g_main, IDC_HK_HOME), &g_cfg.hkHome);
    hotkey_edit_attach(GetDlgItem(g_main, IDC_HK_LOCK), &g_cfg.hkLock);
    wsprintfW(t, L"設定ファイル: %s", g_iniPath);
    SetDlgItemTextW(g_main, IDC_INIPATH, t);
    g_filling = FALSE;
    update_info();
    update_buttons();
}

/* 数値の欄を取り込む。範囲の外は丸めて書き戻す */
static void commit_number(int id)
{
    BOOL ok;
    int  v = (int)GetDlgItemInt(g_main, id, &ok, FALSE), *dst, lo, hi;

    switch (id) {
    case IDC_PORT:   dst = &g_cfg.port;      lo = 1; hi = 65535; break;
    case IDC_DELAY:  dst = &g_cfg.edgeDelay; lo = 0; hi = 5000;  break;
    case IDC_CORNER: dst = &g_cfg.corner;    lo = 0; hi = 1000;  break;
    default: return;
    }
    if (!ok) v = *dst;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if ((int)GetDlgItemInt(g_main, id, NULL, FALSE) != v) {
        g_filling = TRUE;
        SetDlgItemInt(g_main, id, (UINT)v, FALSE);
        g_filling = FALSE;
    }
    if (v != *dst) {
        *dst = v;
        config_changed();
    }
}

static void commit_all(void)
{
    commit_number(IDC_PORT);
    commit_number(IDC_DELAY);
    commit_number(IDC_CORNER);
}

/* ------------------------------------------------------------------ */

static int find_peer(const WCHAR *host, int port, int except)
{
    int i;
    for (i = 0; i < g_cfg.npeers; i++)
        if (i != except && !lstrcmpiW(g_cfg.peers[i].host, host) && g_cfg.peers[i].port == port) return i;
    return -1;
}

static void do_add(void)
{
    Peer p;
    if (g_cfg.npeers >= PEER_MAX) return;
    peer_defaults(&p);
    if (!ui_edit_peer(g_main, &p, TRUE)) return;
    if (find_peer(p.host, p.port, -1) >= 0) {
        ui_message(g_main, L"その PC はもう登録してあります。", NULL, 0, TD_INFORMATION_ICON);
        return;
    }
    peer_free_cell(&g_cfg, &p.gx, &p.gy);
    g_cfg.peers[g_cfg.npeers++] = p;
    config_changed();
    layout_select(layout(), g_cfg.npeers - 1);
    update_buttons();
}

static void do_edit(void)
{
    int  s = layout_selected(layout());
    Peer p;
    if (s < 0) return;
    p = g_cfg.peers[s];
    if (!ui_edit_peer(g_main, &p, FALSE)) return;
    if (find_peer(p.host, p.port, s) >= 0) {
        ui_message(g_main, L"その PC はもう登録してあります。", NULL, 0, TD_INFORMATION_ICON);
        return;
    }
    g_cfg.peers[s] = p;
    config_changed();
    InvalidateRect(layout(), NULL, FALSE);
}

static void do_delete(void)
{
    WCHAR msg[HOST_MAX + 48];
    int   s = layout_selected(layout());
    if (s < 0) return;
    wsprintfW(msg, L"「%s」を削除しますか？", g_cfg.peers[s].host);
    if (ui_message(g_main, msg, NULL, TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, NULL) != IDYES) return;
    MoveMemory(&g_cfg.peers[s], &g_cfg.peers[s + 1], (g_cfg.npeers - s - 1) * sizeof(Peer));
    g_cfg.npeers--;
    config_changed();
    layout_select(layout(), -1);
    update_buttons();
}

/* ------------------------------------------------------------------ */

static INT_PTR CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR t[64];
        g_main = h;
        wsprintfW(t, L"%s %s", APP_NAME, APP_VERSION);
        SetWindowTextW(h, t);
        ui_set_icons(h);
        dlg_look_init(h, &g_look, k_headings, ARRAYSIZE(k_headings), IDCANCEL);
        fill();
        theme_apply_dialog(h);
        return TRUE;
    }

    case WM_DPICHANGED:
        PostMessageW(h, WM_APP_RELOOK, 0, 0);
        return FALSE;
    case WM_APP_RELOOK:
        dlg_look_free(&g_look);
        dlg_look_init(h, &g_look, k_headings, ARRAYSIZE(k_headings), IDCANCEL);
        InvalidateRect(h, NULL, TRUE);
        return TRUE;

    case WM_ERASEBKGND:
        dlg_look_erase(h, (HDC)wp, &g_look);
        SetWindowLongPtrW(h, DWLP_MSGRESULT, 1);
        return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        int id = GetDlgCtrlID((HWND)lp);
        return (INT_PTR)theme_ctlcolor(msg, (HDC)wp, (HWND)lp,
                                       id == IDC_HINT_LAYOUT || id == IDC_THIS_INFO ||
                                       id == IDC_INIPATH || id == IDC_HINT_SWITCH);
    }

    case WM_NOTIFY: {
        LRESULT res;
        if (((NMHDR *)lp)->code == NM_CUSTOMDRAW && theme_custom_draw_button((NMCUSTOMDRAW *)lp, &res)) {
            SetWindowLongPtrW(h, DWLP_MSGRESULT, res);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDC_LAYOUT) {
            switch (code) {
            case LN_SELCHANGE: update_buttons(); break;
            case LN_ACTIVATE:  do_edit(); break;
            case LN_MOVED:     config_changed(); break;
            case LN_DELETE:    do_delete(); break;
            }
            return TRUE;
        }
        if (code == EN_KILLFOCUS && !g_filling) { commit_number(id); return TRUE; }
        if (code == EN_CHANGE && !g_filling && (id == IDC_HK_HOME || id == IDC_HK_LOCK)) {
            hotkey_edit_get((HWND)lp, id == IDC_HK_HOME ? &g_cfg.hkHome : &g_cfg.hkLock);
            config_changed();
            return TRUE;
        }
        switch (id) {
        case IDOK:          /* Enter。配置図にいれば編集 */
            if (GetFocus() == layout()) do_edit();
            else commit_all();
            return TRUE;
        case IDCANCEL:
            commit_all();
            DestroyWindow(h);
            return TRUE;
        case IDC_ADD:      do_add();    return TRUE;
        case IDC_EDITPEER: do_edit();   return TRUE;
        case IDC_DEL:      do_delete(); return TRUE;
        case IDC_PASS:
            if (ui_password(h)) update_info();
            return TRUE;
        case IDC_ACCEPT:
            g_cfg.accept = IsDlgButtonChecked(h, IDC_ACCEPT) == BST_CHECKED;
            config_changed();
            update_info();
            return TRUE;
        case IDC_NODRAG:
            g_cfg.noDragSwitch = IsDlgButtonChecked(h, IDC_NODRAG) == BST_CHECKED;
            config_changed();
            return TRUE;
        case IDC_CLIP:
            g_cfg.clipboard = IsDlgButtonChecked(h, IDC_CLIP) == BST_CHECKED;
            config_changed();
            return TRUE;
        case IDC_STARTUP: {
            BOOL on = IsDlgButtonChecked(h, IDC_STARTUP) == BST_CHECKED;
            if (!startup_set(on)) {
                ui_message(h, L"スタートアップの設定を変更できませんでした。", NULL, 0, TD_ERROR_ICON);
                CheckDlgButton(h, IDC_STARTUP, startup_enabled() ? BST_CHECKED : BST_UNCHECKED);
            }
            return TRUE;
        }
        }
        break;
    }

    case WM_CLOSE:
        commit_all();
        DestroyWindow(h);
        return TRUE;

    case WM_DESTROY:
        dlg_look_free(&g_look);
        RemovePropW(h, L"mouser.footer");
        g_main = NULL;
        return TRUE;
    }
    return FALSE;
}

void ui_open_main(void)
{
    if (!g_main) {
        CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, main_proc, 0);
        if (!g_main) return;
        ShowWindow(g_main, SW_SHOW);
    } else if (IsIconic(g_main)) {
        ShowWindow(g_main, SW_RESTORE);
    }
    SetForegroundWindow(GetLastActivePopup(g_main));
}

BOOL ui_dialog_message(MSG *m)
{
    return g_main && IsDialogMessageW(g_main, m);
}

void ui_status_changed(void)
{
    if (!g_main) return;
    InvalidateRect(layout(), NULL, FALSE);
    update_info();
}

void ui_theme_changed(void)
{
    if (g_main) {
        theme_apply_dialog(g_main);
        InvalidateRect(layout(), NULL, FALSE);
    }
    if (g_modal) theme_apply_dialog(g_modal);
}
