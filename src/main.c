/* ==================================================================
 * main.c - 常駐本体(トレイ、起動引数、多重起動の防止)
 *
 *  input-mouser.exe                 常駐を始める。すでに動いていれば設定画面を出させる
 *  input-mouser.exe -settings       常駐を始めて設定画面を出す
 *  input-mouser.exe -switch <n>     動いている input-mouser に切り替えさせる
 *                                   (0 / home = このPC、1.. = 登録した順の相手)
 *  input-mouser.exe -exit           動いている input-mouser を終わらせる
 *  input-mouser.exe -ini <path>     設定ファイルを指定する(既定は exe と同じ場所)
 *  input-mouser.exe -log            動作を <設定ファイル名>.log に書く
 *  input-mouser.exe -fwremove       Windows ファイアウォールの input-mouser の許可を消す(後始末)
 *  input-mouser.exe -wait <pid>     その PID が終わるのを待ってから始める
 *                                   (管理者として起動し直すときに自分で付ける)
 *  input-mouser.exe -fwremove-now   確認なしで消して終わる(管理者で自分を呼ぶときに付ける)。
 *                                   終了コード = 消した数 | 残った数 << 8、失敗は 0xFFFF
 *
 *  検証用
 *  input-mouser.exe -bind <addr>    受け付けるアドレスを絞る(127.0.0.1 など)
 *  input-mouser.exe -dryrun         受けた入力を再現せず、ログに書くだけにする
 *  input-mouser.exe -nohook         フックを掛けない(画面の確認で、切り替わらないように)
 *  input-mouser.exe -name <名前>    この名前で名乗る(名前で引けない相手への接続を確かめる)
 *  input-mouser.exe -fwprefix <s>   -fwremove で消す規則の名前の先頭を変える(本物の規則を消さずに確かめる)
 *
 *  多重起動の判定は設定ファイルごと。-ini で別の設定を指定すれば
 *  並べて動かせる(検証用)。
 * ================================================================== */

#include "mouser.h"
#include "resource.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <stdarg.h>

HINSTANCE g_inst;
WCHAR     g_exePath[MAX_PATH];
WCHAR     g_iniPath[MAX_PATH];
BOOL      g_customIni;
HWND      g_trayWnd;
WCHAR     g_hostName[HOST_MAX];
WCHAR     g_bindAddr[64];
BOOL      g_dryRun;
BOOL      g_elevated;

static WCHAR           g_logPath[MAX_PATH];
static SRWLOCK         g_logLock = SRWLOCK_INIT;
static NOTIFYICONDATAW g_nid;
static UINT            g_wmTaskbarCreated;
static BOOL            g_trayActive;

#define TRAY_CLASS L"InputMouser.Tray"

/* ------------------------------------------------------------------ */
/*  ログ                                                                */
/* ------------------------------------------------------------------ */

void log_printf(const WCHAR *fmt, ...)
{
    WCHAR      body[1100], line[1200];
    char       u8[3600];
    SYSTEMTIME st;
    va_list    ap;
    HANDLE     f;
    int        n;
    DWORD      wr;

    if (!g_cfg.log || !g_logPath[0]) return;
    va_start(ap, fmt);
    wvsprintfW(body, fmt, ap);          /* 最大 1024 文字で切れる */
    va_end(ap);
    GetLocalTime(&st);
    wsprintfW(line, L"%04d-%02d-%02d %02d:%02d:%02d.%03d %s\r\n", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL);
    if (n <= 1) return;

    AcquireSRWLockExclusive(&g_logLock);
    f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, u8, (DWORD)(n - 1), &wr, NULL);
        CloseHandle(f);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

/* ------------------------------------------------------------------ */
/*  管理者として動かす                                                  */
/* ------------------------------------------------------------------ */

static BOOL is_elevated(void)
{
    HANDLE          t;
    TOKEN_ELEVATION e;
    DWORD           n;
    BOOL            r = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        if (GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n)) r = e.TokenIsElevated != 0;
        CloseHandle(t);
    }
    return r;
}

/* 同じ引数に「-wait <自分>」を足して管理者で起動する。新しい方は、
   こちらが終わるのを待ってから始める(多重起動の判定とポートが空くのを待つ)。 */
BOOL app_relaunch_elevated(const WCHAR *extra)
{
    WCHAR             args[1024];
    SHELLEXECUTEINFOW sei;

    lstrcpynW(args, PathGetArgsW(GetCommandLineW()), 900);
    wsprintfW(args + lstrlenW(args), L" -wait %lu%s%s", GetCurrentProcessId(),
              extra ? L" " : L"", extra ? extra : L"");
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";
    sei.lpFile       = g_exePath;
    sei.lpParameters = args;
    sei.nShow        = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return TRUE;
    log_printf(L"管理者として起動できませんでした (%lu)", GetLastError());
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  トレイ                                                              */
/* ------------------------------------------------------------------ */

static void tray_tip(WCHAR *tip, int cch)
{
    int t = (int)g_target, i, ready = 0;

    if (hook_paused()) { lstrcpynW(tip, L"input-mouser（一時停止中）", cch); return; }
    if (t >= 0 && t < g_cfg.npeers) {
        wsprintfW(tip, L"input-mouser — %s を操作中", g_cfg.peers[t].host);
        return;
    }
    for (i = 0; i < g_cfg.npeers; i++) if (g_peerStatus[i] == PS_READY) ready++;
    if (g_cfg.npeers) wsprintfW(tip, L"input-mouser — %d 台中 %d 台に接続", g_cfg.npeers, ready);
    else if (g_cfg.accept) lstrcpynW(tip, L"input-mouser — 操作を受け付けています", cch);
    else lstrcpynW(tip, L"input-mouser", cch);
    if (g_locked && lstrlenW(tip) + 8 < cch) lstrcatW(tip, L"（固定中）");
    if (g_elevated && lstrlenW(tip) + 8 < cch) lstrcatW(tip, L"（管理者）");
}

void tray_update(void)
{
    BOOL active = g_target >= 0;
    if (!g_nid.hWnd) return;
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    tray_tip(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
    if (active != g_trayActive) {
        HICON old = g_nid.hIcon;
        g_trayActive = active;
        LoadIconMetric(g_inst, MAKEINTRESOURCEW(active ? IDI_ACTIVE : IDI_APP), LIM_SMALL, &g_nid.hIcon);
        g_nid.uFlags |= NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        if (old) DestroyIcon(old);
        return;
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void tray_add(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_trayWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_trayActive           = g_target >= 0;
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(g_trayActive ? IDI_ACTIVE : IDI_APP), LIM_SMALL, &g_nid.hIcon);
    tray_tip(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void tray_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    UINT  cmd;
    int   i;

    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"設定を開く(&S)");
    if (g_cfg.npeers) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (g_target < 0 ? MF_CHECKED : 0), IDM_HOME, L"このPC(&0)");
        for (i = 0; i < g_cfg.npeers; i++) {
            WCHAR t[HOST_MAX + 48];
            BOOL  ready = g_peerStatus[i] == PS_READY;
            if (i < 9) wsprintfW(t, L"%s(&%d)", g_cfg.peers[i].host, i + 1);
            else       lstrcpynW(t, g_cfg.peers[i].host, HOST_MAX);
            if (!ready) { lstrcatW(t, L"\t"); lstrcatW(t, peer_status_text(g_peerStatus[i])); }
            AppendMenuW(m, MF_STRING | (g_target == i ? MF_CHECKED : 0) | (ready && !hook_paused() ? 0 : MF_GRAYED),
                        IDM_PEER0 + i, t);
        }
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (g_locked ? MF_CHECKED : 0), IDM_LOCK, L"切り替えを固定(&L)");
        AppendMenuW(m, MF_STRING | (hook_paused() ? MF_CHECKED : 0), IDM_PAUSE, L"一時停止(&P)");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_FIREWALL, L"ファイアウォールの許可を削除(&F)...");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"終了(&X)");
    SetMenuDefaultItem(m, IDM_SETTINGS, FALSE);

    SetForegroundWindow(g_trayWnd);
    cmd = (UINT)TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY |
                                    (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN),
                                 x, y, g_trayWnd, NULL);
    PostMessageW(g_trayWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) SendMessageW(g_trayWnd, WM_COMMAND, cmd, 0);
}

static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ui_open_main();
            break;
        case WM_CONTEXTMENU:
            tray_menu((short)LOWORD(wp), (short)HIWORD(wp));
            break;
        }
        return 0;

    case WM_APP_COMMAND:
        switch (wp) {
        case CMD_SETTINGS: ui_open_main(); break;
        case CMD_SWITCH:   hook_switch((int)lp - 1); break;
        case CMD_EXIT:     DestroyWindow(h); break;
        }
        return 0;

    case WM_APP_STATUS:
        if (wp == 1) clip_conn_closed((int)lp);
        tray_update();
        ui_status_changed();
        return 0;

    case WM_APP_SWITCHED: {
        int   now = (int)(INT_PTR)wp, prev = (int)lp;
        WCHAR t[HOST_MAX + 32];
        tray_update();
        ui_status_changed();
        if (now >= 0 && now < g_cfg.npeers) {
            log_printf(L"%s へ切り替えました", g_cfg.peers[now].host);
            wsprintfW(t, L"%s を操作中", g_cfg.peers[now].host);
            osd_show(t);
            clip_send_if_changed(now);
        } else if (prev >= 0) {
            log_printf(L"このPC に戻りました");
            osd_show(L"このPC に戻りました");
        }
        return 0;
    }

    case WM_APP_CLIPRECV:
        clip_received((int)wp, (ClipData *)lp);
        return 0;

    case WM_APP_CLIPSEND:
        clip_send_if_changed((int)wp);
        return 0;

    case WM_APP_CURSOR:
        cursor_apply();
        return 0;

    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id >= IDM_PEER0 && id < IDM_PEER0 + PEER_MAX) { hook_switch((int)(id - IDM_PEER0)); return 0; }
        switch (id) {
        case IDM_SETTINGS: ui_open_main(); break;
        case IDM_HOME:     hook_switch(-1); break;
        case IDM_LOCK:     hook_toggle_lock(); break;
        case IDM_PAUSE:    hook_set_paused(!hook_paused()); break;
        case IDM_FIREWALL: fw_cleanup_ui(NULL, TRUE); break;
        case IDM_EXIT:     DestroyWindow(h); break;
        }
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW((const WCHAR *)lp, L"ImmersiveColorSet") && theme_refresh()) {
            theme_allow_dark(h);
            ui_theme_changed();
        }
        return 0;

    case WM_ENDSESSION:
        if (wp) {
            hook_stop();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_nid.hWnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_wmTaskbarCreated && msg) {     /* エクスプローラが再起動した */
        tray_add();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/*  起動                                                                */
/* ------------------------------------------------------------------ */

static BOOL dir_writable(const WCHAR *dir)
{
    WCHAR  p[MAX_PATH + 32];
    HANDLE f;
    wsprintfW(p, L"%s\\input-mouser-%lu.tmp", dir, GetCurrentProcessId());
    f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(f);
    return TRUE;
}

/* 既定は exe と同じ場所。書き込めない場所(Program Files など)に
   置かれたときだけ %APPDATA%\input-mouser に置く。 */
static void default_ini(void)
{
    WCHAR dir[MAX_PATH], app[MAX_PATH], *s;
    PWSTR p = NULL;

    lstrcpynW(dir, g_exePath, MAX_PATH);
    for (s = dir + lstrlenW(dir); s > dir && s[-1] != L'\\'; s--) ;
    if (s > dir) s[-1] = 0;
    wsprintfW(g_iniPath, L"%s\\input-mouser.ini", dir);
    if (GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &p))) {
        wsprintfW(app, L"%s\\input-mouser\\input-mouser.ini", p);
        if (GetFileAttributesW(app) != INVALID_FILE_ATTRIBUTES || !dir_writable(dir)) {
            wsprintfW(app, L"%s\\input-mouser", p);
            CreateDirectoryW(app, NULL);
            wsprintfW(g_iniPath, L"%s\\input-mouser\\input-mouser.ini", p);
        }
        CoTaskMemFree(p);
    }
}

static void mutex_name(WCHAR *out)
{
    WCHAR low[MAX_PATH];
    DWORD h = 2166136261u;
    int   i;
    lstrcpynW(low, g_iniPath, MAX_PATH);
    CharLowerBuffW(low, lstrlenW(low));
    for (i = 0; low[i]; i++) { h ^= low[i]; h *= 16777619u; }      /* FNV-1a */
    wsprintfW(out, L"Local\\InputMouser.%08lX", h);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WCHAR   mname[64];
    HANDLE  mutex;
    WNDCLASSW wc;
    MSG     msg;
    LPWSTR *argv;
    int     argc, i, cmd = 0, cmdArg = 0;
    BOOL    openSettings = FALSE, first, noHook = FALSE, fwRemove = FALSE, fwRemoveNow = FALSE;
    INITCOMMONCONTROLSEX icc;
    DWORD   n, waitPid = 0;
    WCHAR   fakeName[HOST_MAX] = L"";

    (void)prev; (void)cmdline; (void)show;
    g_inst = inst;
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        const WCHAR *a = argv[i];
        if (a[0] == L'/' ) a++;
        else if (a[0] == L'-') { a++; if (a[0] == L'-') a++; }
        if (!lstrcmpiW(a, L"ini") && i + 1 < argc) {
            GetFullPathNameW(argv[++i], MAX_PATH, g_iniPath, NULL);
            g_customIni = TRUE;
        }
        else if (!lstrcmpiW(a, L"log"))      g_cfg.log = TRUE;
        else if (!lstrcmpiW(a, L"exit"))     cmd = CMD_EXIT;
        else if (!lstrcmpiW(a, L"settings")) openSettings = TRUE;
        else if (!lstrcmpiW(a, L"dryrun"))   g_dryRun = TRUE;
        else if (!lstrcmpiW(a, L"nohook"))   noHook = TRUE;
        else if (!lstrcmpiW(a, L"fwremove"))     fwRemove = TRUE;
        else if (!lstrcmpiW(a, L"fwremove-now")) fwRemoveNow = TRUE;
        else if (!lstrcmpiW(a, L"fwprefix") && i + 1 < argc) lstrcpynW(g_fwPrefix, argv[++i], 32);
        else if (!lstrcmpiW(a, L"name") && i + 1 < argc) lstrcpynW(fakeName, argv[++i], HOST_MAX);
        else if (!lstrcmpiW(a, L"wait") && i + 1 < argc) waitPid = (DWORD)StrToIntW(argv[++i]);
        else if (!lstrcmpiW(a, L"bind") && i + 1 < argc) lstrcpynW(g_bindAddr, argv[++i], 64);
        else if (!lstrcmpiW(a, L"switch") && i + 1 < argc) {
            const WCHAR *v = argv[++i];
            cmd    = CMD_SWITCH;
            cmdArg = (!lstrcmpiW(v, L"home") || !lstrcmpiW(v, L"0")) ? 0 : StrToIntW(v);
        }
    }
    if (argv) LocalFree(argv);

    if (fwRemoveNow) {              /* 管理者で呼ばれて、消すだけ */
        int left = 0, n;
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        n = fw_remove(&left);
        CoUninitialize();
        return n < 0 ? 0xFFFF : (n & 0xFF) | ((left & 0xFF) << 8);
    }
    if (!g_customIni) default_ini();
    if (fwRemove) {                 /* 常駐せず、後始末の画面だけ出す */
        INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        InitCommonControlsEx(&ic);
        config_load();
        theme_init();
        g_elevated = is_elevated();
        fw_cleanup_ui(NULL, FALSE);
        CoUninitialize();
        return 0;
    }
    if (waitPid) {                  /* 起動し直す前の自分が終わるまで待つ */
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
        if (old) { WaitForSingleObject(old, 15000); CloseHandle(old); }
    }

    /* 同じ設定ファイルで動いているものがあれば、そちらに頼んで終わる */
    mutex_name(mname);
    mutex = CreateMutexW(NULL, FALSE, mname);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(TRAY_CLASS, g_iniPath);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_COMMAND, cmd ? cmd : CMD_SETTINGS, cmdArg);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (cmd) {                      /* -exit / -switch の相手がいない */
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    lstrcpynW(g_logPath, g_iniPath, MAX_PATH);
    {
        int len = lstrlenW(g_logPath);
        if (len > 4 && !lstrcmpiW(g_logPath + len - 4, L".ini")) g_logPath[len - 4] = 0;
        if (lstrlenW(g_logPath) + 5 < MAX_PATH) lstrcatW(g_logPath, L".log");
    }
    n = HOST_MAX;
    if (!GetComputerNameExW(ComputerNameDnsHostname, g_hostName, &n)) {
        n = HOST_MAX;
        GetComputerNameW(g_hostName, &n);
    }
    if (fakeName[0]) lstrcpynW(g_hostName, fakeName, HOST_MAX);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    if (!crypto_init()) {
        ui_message(NULL, L"暗号化の機能を初期化できませんでした。", NULL, 0, TD_ERROR_ICON);
        return 1;
    }

    first = GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES;
    config_load();
    g_elevated = is_elevated();
    if (g_cfg.admin && !g_elevated) {
        /* 管理者として動かす設定なので、起動し直す。断られたら普通の権限のまま動く */
        if (mutex) CloseHandle(mutex);
        if (app_relaunch_elevated(openSettings ? L"-settings" : NULL)) return 0;
        mutex = CreateMutexW(NULL, FALSE, mname);
    }
    theme_init();                   /* ini の theme= を見るので読み込みの後 */
    layout_register();
    if (first) {
        config_save();              /* 次からは黙って常駐を始めるように */
        openSettings = TRUE;
    }
    log_printf(L"input-mouser %s 起動 (%s、設定 %s%s%s)", APP_VERSION, g_hostName, g_iniPath,
               g_elevated ? L"、管理者" : L"", g_dryRun ? L"、dryrun" : L"");

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = tray_proc;
    wc.hInstance     = inst;
    wc.lpszClassName = TRAY_CLASS;
    RegisterClassW(&wc);
    /* 別のインスタンスが探せるよう、タイトルに設定ファイルのパスを入れておく */
    g_trayWnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, g_iniPath, WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_trayWnd) return 1;
    ChangeWindowMessageFilterEx(g_trayWnd, g_wmTaskbarCreated, MSGFLT_ALLOW, NULL);
    /* 管理者で動いているときも、普通の権限からの -exit / -switch / 二重起動を受ける */
    ChangeWindowMessageFilterEx(g_trayWnd, WM_APP_COMMAND, MSGFLT_ALLOW, NULL);
    theme_allow_dark(g_trayWnd);
    tray_add();

    if (!net_start()) log_printf(L"通信を始められませんでした");
    if (!noHook) hook_start();
    if (openSettings) ui_open_main();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (ui_dialog_message(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    hook_stop();
    net_stop();
    log_printf(L"input-mouser 終了");
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
