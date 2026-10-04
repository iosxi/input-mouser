/* ==================================================================
 * firewall.c - Windows ファイアウォールの input-mouser の許可を消す
 *
 *  使わなくなるときの後始末。トレイのメニューと -fwremove から呼ぶ。
 *  Windows のファイアウォールの COM(INetFwPolicy2)を使う。
 *
 *  消す対象
 *      名前が「input-mouser」で始まる規則(初回の確認画面が作る
 *      「input-mouser - 1 組の…」の TCP・UDP、手で足した「input-mouser TCP」など。
 *      前に別の場所に置いていた exe の分も含む)
 *      プログラムがこの exe の規則(名前を変えてあっても)
 *
 *  ・規則を読むのは普通の権限でできる。消すには管理者が要るので、普通の権限で
 *    動いているときは、自分を管理者で「-fwremove-now」付きで起動して消させる
 *    (終了コード = 消した数 | 残った数 << 8、失敗は 0xFFFF)。
 *  ・検証では -fwprefix <名前の先頭> で対象の名前を変える(本物の規則を消さないため)。
 *  ・同じ名前の規則が 2 つあっても、Remove(名前) を 1 回呼ぶと 1 つしか消えない
 *    (2026-10-04 実測: TCP と UDP の「imtest-dup」→ Remove 1 回で 2 → 1)。
 *    なので、なくなるまで繰り返す。
 * ================================================================== */

#define COBJMACROS
#include "mouser.h"
#include <initguid.h>
#include <netfw.h>

#define FW_MAX 64

WCHAR g_fwPrefix[32] = L"input-mouser";     /* -fwprefix(検証用)で変える */

typedef struct {
    WCHAR name[128];
    LONG  proto, dir, action, profiles;
} FwRule;

static BOOL is_ours(const WCHAR *name, const WCHAR *app)
{
    int n = lstrlenW(g_fwPrefix);
    if (name && n && lstrlenW(name) >= n &&
        CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, name, n, g_fwPrefix, n) == CSTR_EQUAL)
        return TRUE;
    return app && !lstrcmpiW(app, g_exePath);
}

static INetFwRules *open_rules(INetFwPolicy2 **pol)
{
    INetFwRules *rules = NULL;
    *pol = NULL;
    if (FAILED(CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER,
                                &IID_INetFwPolicy2, (void **)pol)))
        return NULL;
    if (FAILED(INetFwPolicy2_get_Rules(*pol, &rules))) {
        INetFwPolicy2_Release(*pol);
        *pol = NULL;
        return NULL;
    }
    return rules;
}

/* 該当する規則を集める。数を返す(読めなければ -1) */
static int collect(INetFwRules *rules, FwRule *out, int max)
{
    IUnknown     *u = NULL;
    IEnumVARIANT *en = NULL;
    VARIANT       v;
    ULONG         got;
    int           n = 0;

    if (FAILED(INetFwRules_get__NewEnum(rules, &u))) return -1;
    if (FAILED(IUnknown_QueryInterface(u, &IID_IEnumVARIANT, (void **)&en))) { IUnknown_Release(u); return -1; }
    IUnknown_Release(u);
    VariantInit(&v);
    while (IEnumVARIANT_Next(en, 1, &v, &got) == S_OK && got) {
        INetFwRule *r;
        if (v.vt == VT_DISPATCH && v.pdispVal &&
            SUCCEEDED(IDispatch_QueryInterface(v.pdispVal, &IID_INetFwRule, (void **)&r))) {
            BSTR name = NULL, app = NULL;
            INetFwRule_get_Name(r, &name);
            INetFwRule_get_ApplicationName(r, &app);
            if (is_ours(name, app)) {
                if (n < max && out) {
                    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
                    NET_FW_ACTION         act = NET_FW_ACTION_ALLOW;
                    FwRule *f = &out[n];
                    ZeroMemory(f, sizeof(*f));
                    lstrcpynW(f->name, name ? name : L"", ARRAYSIZE(f->name));
                    INetFwRule_get_Protocol(r, &f->proto);
                    INetFwRule_get_Direction(r, &dir);
                    INetFwRule_get_Action(r, &act);
                    INetFwRule_get_Profiles(r, &f->profiles);
                    f->dir = dir;
                    f->action = act;
                }
                n++;
            }
            SysFreeString(name);
            SysFreeString(app);
            INetFwRule_Release(r);
        }
        VariantClear(&v);
    }
    IEnumVARIANT_Release(en);
    return n;
}

/* 一覧を文字にする(確認画面用)。数を返す。読めなければ -1 */
int fw_list(WCHAR *buf, int cch)
{
    INetFwPolicy2 *pol;
    INetFwRules   *rules = open_rules(&pol);
    FwRule         rs[FW_MAX];
    int            n, i;

    buf[0] = 0;
    if (!rules) return -1;
    n = collect(rules, rs, FW_MAX);
    for (i = 0; i < n && i < FW_MAX; i++) {
        WCHAR line[256], prof[48] = L"";
        const FwRule *f = &rs[i];
        if (f->profiles == NET_FW_PROFILE2_ALL || (f->profiles & 7) == 7) lstrcpyW(prof, L"すべて");
        else {
            if (f->profiles & NET_FW_PROFILE2_DOMAIN)  lstrcatW(prof, L"ドメイン ");
            if (f->profiles & NET_FW_PROFILE2_PRIVATE) lstrcatW(prof, L"プライベート ");
            if (f->profiles & NET_FW_PROFILE2_PUBLIC)  lstrcatW(prof, L"パブリック ");
            if (prof[0]) prof[lstrlenW(prof) - 1] = 0;
        }
        wsprintfW(line, L"・%s（%s・%s・%s・%s）\n", f->name,
                  f->proto == 6 ? L"TCP" : f->proto == 17 ? L"UDP" : L"すべての通信",
                  f->dir == NET_FW_RULE_DIR_OUT ? L"送信" : L"受信",
                  f->action == NET_FW_ACTION_BLOCK ? L"ブロック" : L"許可", prof);
        if (lstrlenW(buf) + lstrlenW(line) + 1 < cch) lstrcatW(buf, line);
    }
    if (n > FW_MAX && lstrlenW(buf) + 16 < cch) lstrcatW(buf, L"・ほか\n");
    INetFwRules_Release(rules);
    INetFwPolicy2_Release(pol);
    return n;
}

/* 消す(管理者が要る)。消した数を返し、残った数を *left に入れる。失敗は -1 */
int fw_remove(int *left)
{
    INetFwPolicy2 *pol;
    INetFwRules   *rules = open_rules(&pol);
    FwRule         rs[FW_MAX];
    int            before, now, pass, i;

    *left = 0;
    if (!rules) return -1;
    before = now = collect(rules, NULL, 0);
    for (pass = 0; pass < 16 && now > 0; pass++) {
        int n = collect(rules, rs, FW_MAX);
        for (i = 0; i < n && i < FW_MAX; i++) {
            BSTR b = SysAllocString(rs[i].name);
            HRESULT hr = INetFwRules_Remove(rules, b);
            SysFreeString(b);
            if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) { pass = 99; break; }
        }
        now = collect(rules, NULL, 0);
    }
    INetFwRules_Release(rules);
    INetFwPolicy2_Release(pol);
    if (now < 0) return -1;
    *left = now;
    log_printf(L"ファイアウォールの規則を %d 件削除しました(残り %d 件)", before - now, now);
    return before - now;
}

/* ------------------------------------------------------------------ */
/*  画面                                                                */
/* ------------------------------------------------------------------ */

/* 管理者で自分を起動して消させる */
static int remove_elevated(int *left)
{
    SHELLEXECUTEINFOW sei;
    DWORD code = 0xFFFF;
    WCHAR args[96] = L"-fwremove-now";

    *left = 0;
    if (lstrcmpW(g_fwPrefix, L"input-mouser")) wsprintfW(args, L"-fwremove-now -fwprefix \"%s\"", g_fwPrefix);
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";
    sei.lpFile       = g_exePath;
    sei.lpParameters = args;
    sei.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -2;     /* 断られた */
    WaitForSingleObject(sei.hProcess, 30000);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code == 0xFFFF || code == STILL_ACTIVE) return -1;
    *left = (int)((code >> 8) & 0xFF);
    return (int)(code & 0xFF);
}

#define BTN_EXIT 100

void fw_cleanup_ui(HWND owner, BOOL offerExit)
{
    WCHAR list[4096], content[4600], main[128];
    int   n, removed, left = 0;

    n = fw_list(list, ARRAYSIZE(list));
    if (n < 0) {
        ui_message(owner, L"ファイアウォールの規則を読めませんでした。", NULL, 0, TD_ERROR_ICON);
        return;
    }
    if (n == 0) {
        ui_message(owner, L"Windows ファイアウォールに input-mouser の規則はありません。",
                   L"削除するものはありません。", 0, TD_INFORMATION_ICON);
        return;
    }
    wsprintfW(content,
              L"見つかった規則（%d 件）\n%s\n"
              L"削除すると、ほかの PC からこの PC を操作できなくなります。"
              L"input-mouser を使い続ける場合は、次に起動したときに許可の確認がもう一度出ます。",
              n, list);
    if (ui_message(owner, L"Windows ファイアウォールから input-mouser の許可を削除しますか？", content,
                   TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_WARNING_ICON) != IDYES)
        return;

    if (g_elevated) removed = fw_remove(&left);
    else            removed = remove_elevated(&left);
    if (removed == -2) {
        ui_message(owner, L"管理者として実行できなかったので、削除しませんでした。", NULL, 0, TD_ERROR_ICON);
        return;
    }
    if (removed < 0) {
        ui_message(owner, L"削除できませんでした。", NULL, 0, TD_ERROR_ICON);
        return;
    }

    if (left) wsprintfW(main, L"%d 件を削除しました。%d 件は削除できませんでした。", removed, left);
    else      wsprintfW(main, L"%d 件の規則を削除しました。", removed);
    {
        TASKDIALOGCONFIG  tc;
        TASKDIALOG_BUTTON btn[1] = { { BTN_EXIT, L"input-mouser を終了する" } };
        int pressed = 0;
        ZeroMemory(&tc, sizeof(tc));
        tc.cbSize             = sizeof(tc);
        tc.hwndParent         = owner;
        tc.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        tc.pszWindowTitle     = APP_NAME;
        tc.pszMainIcon        = left ? TD_WARNING_ICON : TD_INFORMATION_ICON;
        tc.pszMainInstruction = main;
        tc.pszContent         = offerExit
            ? L"使うのをやめるなら、input-mouser を終了してから exe と設定ファイル（input-mouser.ini）を削除してください。"
            : L"使うのをやめるなら、exe と設定ファイル（input-mouser.ini）を削除してください。";
        if (offerExit) {
            tc.pButtons = btn;
            tc.cButtons = 1;
        }
        tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        if (SUCCEEDED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) && pressed == BTN_EXIT && g_trayWnd)
            PostMessageW(g_trayWnd, WM_APP_COMMAND, CMD_EXIT, 0);
    }
}
