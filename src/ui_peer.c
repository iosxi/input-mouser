/* ==================================================================
 * ui_peer.c - 相手の PC の追加・編集と、パスワードの設定
 *
 *  相手の画面を開くと、同じネットワークで「操作を受け付けている」
 *  input-mouser を探して一覧に出す(UDP のブロードキャスト。net.c)。
 *  一覧から選ぶと名前とポートが入る。
 * ================================================================== */

#include "mouser.h"
#include "resource.h"

extern HWND g_modal;

static const int k_peerHeadings[] = { IDC_H_PEER, IDC_H_FOUND };

typedef struct {
    Peer   *p;
    DlgLook look;
    int     found;
} PeerDlg;

static INT_PTR common_msg(HWND h, UINT msg, WPARAM wp, LPARAM lp, DlgLook *lk, BOOL *handled)
{
    *handled = TRUE;
    switch (msg) {
    case WM_ERASEBKGND:
        dlg_look_erase(h, (HDC)wp, lk);
        SetWindowLongPtrW(h, DWLP_MSGRESULT, 1);
        return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        int id = GetDlgCtrlID((HWND)lp);
        return (INT_PTR)theme_ctlcolor(msg, (HDC)wp, (HWND)lp,
                                       id == IDC_FIND_STATE || id == IDC_HINT_PHK || id == IDC_PW_HINT);
    }
    case WM_NOTIFY: {
        LRESULT res;
        if (((NMHDR *)lp)->code == NM_CUSTOMDRAW && theme_custom_draw_button((NMCUSTOMDRAW *)lp, &res)) {
            SetWindowLongPtrW(h, DWLP_MSGRESULT, res);
            return TRUE;
        }
        break;
    }
    }
    *handled = FALSE;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  相手の PC                                                           */
/* ------------------------------------------------------------------ */

static void start_find(HWND h, PeerDlg *d)
{
    ListView_DeleteAllItems(GetDlgItem(h, IDC_FOUND));
    d->found = 0;
    SetDlgItemTextW(h, IDC_FIND_STATE, L"探しています…");
    EnableWindow(GetDlgItem(h, IDC_FIND), FALSE);
    net_discover(h);
}

static void add_found(HWND h, PeerDlg *d, const Found *f)
{
    HWND    lv = GetDlgItem(h, IDC_FOUND);
    LVITEMW it;
    WCHAR   addr[96];
    int     i, n = ListView_GetItemCount(lv);

    /* 自分自身(同じ名前・同じポートで受け付けている)は出さない */
    if (!lstrcmpiW(f->name, g_hostName) && f->port == g_cfg.port && g_cfg.accept && !g_bindAddr[0]) return;
    for (i = 0; i < n; i++) {           /* 複数の経路から同じ返事が届く */
        WCHAR nm[HOST_MAX];
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(lv, &it);
        ListView_GetItemText(lv, i, 0, nm, HOST_MAX);
        if (!lstrcmpiW(nm, f->name) && (int)it.lParam == f->port) return;
    }
    if (f->port == DEFAULT_PORT) lstrcpynW(addr, f->addr, ARRAYSIZE(addr));
    else wsprintfW(addr, L"%s : %d", f->addr, f->port);
    ZeroMemory(&it, sizeof(it));
    it.mask    = LVIF_TEXT | LVIF_PARAM;
    it.iItem   = n;
    it.pszText = (WCHAR *)f->name;
    it.lParam  = f->port;
    ListView_InsertItem(lv, &it);
    ListView_SetItemText(lv, n, 1, addr);
    d->found++;
}

static void pick_found(HWND h, int i)
{
    HWND    lv = GetDlgItem(h, IDC_FOUND);
    WCHAR   nm[HOST_MAX];
    LVITEMW it;
    if (i < 0) return;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = i;
    ListView_GetItem(lv, &it);
    ListView_GetItemText(lv, i, 0, nm, HOST_MAX);
    SetDlgItemTextW(h, IDC_HOST, nm);
    SetDlgItemInt(h, IDC_PPORT, (UINT)it.lParam, FALSE);
}

static INT_PTR CALLBACK peer_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    PeerDlg *d = (PeerDlg *)GetWindowLongPtrW(h, DWLP_USER);
    BOOL     handled;
    INT_PTR  r;

    if (d && (r = common_msg(h, msg, wp, lp, &d->look, &handled), handled)) return r;

    switch (msg) {
    case WM_INITDIALOG: {
        HWND      lv = GetDlgItem(h, IDC_FOUND);
        LVCOLUMNW col;
        RECT      rc;
        int       w;

        d = (PeerDlg *)lp;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)d);
        g_modal = h;
        dlg_look_init(h, &d->look, k_peerHeadings, ARRAYSIZE(k_peerHeadings), IDOK);
        SetDlgItemTextW(h, IDC_HOST, d->p->host);
        SetDlgItemInt(h, IDC_PPORT, (UINT)d->p->port, FALSE);
        SendDlgItemMessageW(h, IDC_HOST, EM_SETCUEBANNER, TRUE, (LPARAM)L"例: OFFICE-PC、192.168.1.20");
        hotkey_edit_attach(GetDlgItem(h, IDC_PHK), &d->p->hk);

        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        GetClientRect(lv, &rc);
        w = rc.right - GetSystemMetrics(SM_CXVSCROLL);
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = L"名前";     col.cx = w * 55 / 100;     ListView_InsertColumn(lv, 0, &col);
        col.pszText = L"アドレス"; col.cx = w - w * 55 / 100; ListView_InsertColumn(lv, 1, &col);

        theme_apply_dialog(h);
        start_find(h, d);
        return TRUE;
    }

    case WM_APP_FOUND:
        if (!lp) {
            WCHAR t[64];
            if (d->found) wsprintfW(t, L"%d 台見つかりました", d->found);
            else lstrcpyW(t, L"見つかりませんでした");
            SetDlgItemTextW(h, IDC_FIND_STATE, t);
            EnableWindow(GetDlgItem(h, IDC_FIND), TRUE);
        } else {
            add_found(h, d, (const Found *)lp);
            HeapFree(GetProcessHeap(), 0, (void *)lp);
        }
        return TRUE;

    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom != IDC_FOUND) break;
        if (n->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if ((v->uChanged & LVIF_STATE) && (v->uNewState & LVIS_SELECTED)) pick_found(h, v->iItem);
        } else if (n->code == NM_DBLCLK && ((NMITEMACTIVATE *)lp)->iItem >= 0) {
            pick_found(h, ((NMITEMACTIVATE *)lp)->iItem);
            PostMessageW(h, WM_COMMAND, IDOK, 0);
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FIND:
            start_find(h, d);
            return TRUE;
        case IDOK: {
            WCHAR host[HOST_MAX];
            BOOL  ok;
            int   port, i, j;
            GetDlgItemTextW(h, IDC_HOST, host, HOST_MAX);
            for (i = 0; host[i] == L' ' || host[i] == 0x3000; i++) ;      /* 前後の空白を落とす */
            MoveMemory(host, host + i, (lstrlenW(host + i) + 1) * sizeof(WCHAR));
            for (j = lstrlenW(host); j > 0 && (host[j - 1] == L' ' || host[j - 1] == 0x3000); j--) host[j - 1] = 0;
            if (!host[0]) {
                ui_message(h, L"PC の名前か IP アドレスを入れてください。", NULL, 0, TD_INFORMATION_ICON);
                SetFocus(GetDlgItem(h, IDC_HOST));
                return TRUE;
            }
            port = (int)GetDlgItemInt(h, IDC_PPORT, &ok, FALSE);
            if (!ok || port < 1 || port > 65535) {
                ui_message(h, L"ポートは 1〜65535 の数で入れてください。", NULL, 0, TD_INFORMATION_ICON);
                SetFocus(GetDlgItem(h, IDC_PPORT));
                return TRUE;
            }
            lstrcpynW(d->p->host, host, HOST_MAX);
            d->p->port = port;
            hotkey_edit_get(GetDlgItem(h, IDC_PHK), &d->p->hk);
            EndDialog(h, TRUE);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, FALSE);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (d) { dlg_look_free(&d->look); RemovePropW(h, L"mouser.footer"); }
        g_modal = NULL;
        break;
    }
    return FALSE;
}

BOOL ui_edit_peer(HWND owner, Peer *p, BOOL isNew)
{
    PeerDlg d;
    INT_PTR r;
    ZeroMemory(&d, sizeof(d));
    d.p = p;
    (void)isNew;
    r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PEER), owner, peer_proc, (LPARAM)&d);
    return r == TRUE;
}

/* ------------------------------------------------------------------ */
/*  パスワード                                                          */
/* ------------------------------------------------------------------ */

static INT_PTR CALLBACK pass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    static DlgLook look;
    BOOL    handled;
    INT_PTR r;

    if (msg != WM_INITDIALOG && (r = common_msg(h, msg, wp, lp, &look, &handled), handled)) return r;

    switch (msg) {
    case WM_INITDIALOG:
        g_modal = h;
        dlg_look_init(h, &look, NULL, 0, IDOK);
        if (g_cfg.haveKey)
            SendDlgItemMessageW(h, IDC_PW1, EM_SETCUEBANNER, TRUE, (LPARAM)L"設定済み（変えるときだけ入力）");
        theme_apply_dialog(h);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PW_SHOW: {
            WCHAR ch = IsDlgButtonChecked(h, IDC_PW_SHOW) == BST_CHECKED ? 0 : 0x25CF;    /* ● */
            SendDlgItemMessageW(h, IDC_PW1, EM_SETPASSWORDCHAR, ch, 0);
            SendDlgItemMessageW(h, IDC_PW2, EM_SETPASSWORDCHAR, ch, 0);
            InvalidateRect(GetDlgItem(h, IDC_PW1), NULL, TRUE);
            InvalidateRect(GetDlgItem(h, IDC_PW2), NULL, TRUE);
            return TRUE;
        }
        case IDOK: {
            WCHAR a[256], b[256];
            BOOL  ok;
            GetDlgItemTextW(h, IDC_PW1, a, ARRAYSIZE(a));
            GetDlgItemTextW(h, IDC_PW2, b, ARRAYSIZE(b));
            if (lstrcmpW(a, b)) {
                ui_message(h, L"2 つの欄のパスワードが一致しません。", NULL, 0, TD_INFORMATION_ICON);
                SetFocus(GetDlgItem(h, IDC_PW2));
                SecureZeroMemory(a, sizeof(a)); SecureZeroMemory(b, sizeof(b));
                return TRUE;
            }
            if (!a[0] && ui_message(h, L"パスワードを空にしますか？",
                                    L"空のパスワードでも通信は暗号化されますが、同じネットワークにある "
                                    L"input-mouser ならどれでもこの PC につなげるようになります。",
                                    TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_WARNING_ICON) != IDYES) {
                return TRUE;
            }
            {
                HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
                ok = config_set_password(a);
                SetCursor(old);
            }
            SecureZeroMemory(a, sizeof(a)); SecureZeroMemory(b, sizeof(b));
            SetDlgItemTextW(h, IDC_PW1, L"");
            SetDlgItemTextW(h, IDC_PW2, L"");
            if (!ok) {
                ui_message(h, L"鍵を作れませんでした。", NULL, 0, TD_ERROR_ICON);
                return TRUE;
            }
            EndDialog(h, TRUE);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, FALSE);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        dlg_look_free(&look);
        RemovePropW(h, L"mouser.footer");
        g_modal = NULL;
        break;
    }
    return FALSE;
}

BOOL ui_password(HWND owner)
{
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PASSWORD), owner, pass_proc, 0) == TRUE;
}
