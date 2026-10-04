/* ==================================================================
 * config.c - input-mouser.ini の読み書き
 *
 *  レジストリは使わない。設定は exe と同じ場所の input-mouser.ini
 *  (UTF-8)にだけ置く。相手の PC は [peer] の節を並べる。
 *
 *      [general]
 *      accept=1                受け付ける
 *      port=31860
 *      key=<64 桁の 16 進>     パスワードから作った鍵
 *      hotkey_home=Ctrl+Alt+Home
 *
 *      [peer]
 *      host=OFFICE-PC
 *      port=31860
 *      x=1                     このPC から見た位置(右が +、下が +)
 *      y=0
 *      hotkey=Ctrl+Alt+F2
 *
 *  GetPrivateProfileString は UTF-8 を扱えないので自前で読む。
 * ================================================================== */

#include "mouser.h"

Config g_cfg;

static BOOL g_logInIni;

void peer_defaults(Peer *p)
{
    ZeroMemory(p, sizeof(*p));
    p->port = DEFAULT_PORT;
    p->gx   = 1;
}

int peer_at(const Config *c, int gx, int gy)
{
    int i;
    for (i = 0; i < c->npeers; i++)
        if (c->peers[i].gx == gx && c->peers[i].gy == gy) return i;
    return -1;
}

/* 新しく足す相手の置き場所。右へ、次に左・上・下の順で空きを探す */
void peer_free_cell(const Config *c, int *gx, int *gy)
{
    int d;
    for (d = 1; d < 16; d++) {
        if (peer_at(c,  d, 0) < 0) { *gx =  d; *gy = 0; return; }
        if (peer_at(c, -d, 0) < 0) { *gx = -d; *gy = 0; return; }
        if (peer_at(c, 0, -d) < 0) { *gx = 0; *gy = -d; return; }
        if (peer_at(c, 0,  d) < 0) { *gx = 0; *gy =  d; return; }
    }
    *gx = 16; *gy = 0;
}

/* ------------------------------------------------------------------ */
/*  ホットキーの文字表現  "Ctrl+Alt+Home"                               */
/* ------------------------------------------------------------------ */

static const struct { BYTE vk; const WCHAR *name; } k_keys[] = {
    { VK_BACK, L"Backspace" }, { VK_TAB, L"Tab" }, { VK_RETURN, L"Enter" },
    { VK_PAUSE, L"Pause" }, { VK_CAPITAL, L"CapsLock" }, { VK_ESCAPE, L"Esc" },
    { VK_SPACE, L"Space" }, { VK_PRIOR, L"PageUp" }, { VK_NEXT, L"PageDown" },
    { VK_END, L"End" }, { VK_HOME, L"Home" }, { VK_LEFT, L"Left" }, { VK_UP, L"Up" },
    { VK_RIGHT, L"Right" }, { VK_DOWN, L"Down" }, { VK_SNAPSHOT, L"PrintScreen" },
    { VK_INSERT, L"Insert" }, { VK_DELETE, L"Delete" }, { VK_APPS, L"Menu" },
    { VK_MULTIPLY, L"NumMul" }, { VK_ADD, L"NumAdd" }, { VK_SUBTRACT, L"NumSub" },
    { VK_DECIMAL, L"NumDec" }, { VK_DIVIDE, L"NumDiv" }, { VK_NUMLOCK, L"NumLock" },
    { VK_SCROLL, L"ScrollLock" },
    { VK_CONVERT, L"変換" }, { VK_NONCONVERT, L"無変換" },
};

static void key_name(BYTE vk, WCHAR *buf)
{
    int i;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { buf[0] = vk; buf[1] = 0; return; }
    if (vk >= VK_F1 && vk <= VK_F24) { wsprintfW(buf, L"F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { wsprintfW(buf, L"Num%d", vk - VK_NUMPAD0); return; }
    for (i = 0; i < (int)ARRAYSIZE(k_keys); i++)
        if (k_keys[i].vk == vk) { lstrcpyW(buf, k_keys[i].name); return; }
    wsprintfW(buf, L"VK%02X", vk);
}

void hotkey_format(const Hotkey *hk, WCHAR *buf, int cch)
{
    WCHAR t[96], k[16];
    t[0] = 0;
    if (!hk->vk) { lstrcpynW(buf, L"", cch); return; }
    if (hk->mods & HK_CTRL)  lstrcatW(t, L"Ctrl + ");
    if (hk->mods & HK_ALT)   lstrcatW(t, L"Alt + ");
    if (hk->mods & HK_SHIFT) lstrcatW(t, L"Shift + ");
    if (hk->mods & HK_WIN)   lstrcatW(t, L"Win + ");
    key_name(hk->vk, k);
    lstrcatW(t, k);
    lstrcpynW(buf, t, cch);
}

BOOL hotkey_parse(const WCHAR *src, Hotkey *hk)
{
    WCHAR s[96], *p, *tok;
    int   i;

    hk->vk = 0; hk->mods = 0;
    lstrcpynW(s, src, ARRAYSIZE(s));
    for (p = s; *p; ) {
        WCHAR nm[16];
        while (*p == L' ' || *p == L'+') p++;
        if (!*p) break;
        tok = p;
        while (*p && *p != L'+') p++;
        if (*p) *p++ = 0;
        {   /* 末尾の空白を落とす */
            int n = lstrlenW(tok);
            while (n > 0 && tok[n - 1] == L' ') tok[--n] = 0;
        }
        if (!lstrcmpiW(tok, L"Ctrl"))       { hk->mods |= HK_CTRL;  continue; }
        if (!lstrcmpiW(tok, L"Alt"))        { hk->mods |= HK_ALT;   continue; }
        if (!lstrcmpiW(tok, L"Shift"))      { hk->mods |= HK_SHIFT; continue; }
        if (!lstrcmpiW(tok, L"Win"))        { hk->mods |= HK_WIN;   continue; }
        for (i = 1; i < 256; i++) {
            key_name((BYTE)i, nm);
            if (!lstrcmpiW(tok, nm)) { hk->vk = (BYTE)i; break; }
        }
        if (i == 256) { hk->vk = 0; hk->mods = 0; return FALSE; }
    }
    if (!hk->vk) hk->mods = 0;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  読み込み                                                            */
/* ------------------------------------------------------------------ */

static void trim(WCHAR *s)
{
    int n = lstrlenW(s), i = 0;
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r')) s[--n] = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) MoveMemory(s, s + i, (n - i + 1) * sizeof(WCHAR));
}

static int to_int(const WCHAR *s, int def, int lo, int hi)
{
    int sign = 1, v = 0, digits = 0;
    if (*s == L'-') { sign = -1; s++; }
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (*s++ - L'0'); if (v > 10000000) break; digits++; }
    if (!digits) return def;
    v *= sign;
    return v < lo ? lo : v > hi ? hi : v;
}

static WCHAR *read_text_file(const WCHAR *path)
{
    HANDLE f;
    DWORD  size, got;
    char  *buf;
    WCHAR *w;
    int    off = 0, len;

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) { CloseHandle(f); return NULL; }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, size + 1);
    if (!buf) { CloseHandle(f); return NULL; }
    if (!ReadFile(f, buf, size, &got, NULL)) got = 0;
    CloseHandle(f);
    buf[got] = 0;
    if (got >= 3 && (BYTE)buf[0] == 0xEF && (BYTE)buf[1] == 0xBB && (BYTE)buf[2] == 0xBF) off = 3;
    len = MultiByteToWideChar(CP_UTF8, 0, buf + off, got - off, NULL, 0);
    w = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (w) {
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, buf + off, got - off, w, len);
        w[len > 0 ? len : 0] = 0;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return w;
}

static BOOL hex_key(const WCHAR *s, BYTE out[32])
{
    int i;
    if (lstrlenW(s) != 64) return FALSE;
    for (i = 0; i < 64; i++) {
        WCHAR c = s[i];
        int   v = (c >= L'0' && c <= L'9') ? c - L'0' :
                  (c >= L'a' && c <= L'f') ? c - L'a' + 10 :
                  (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
        if (v < 0) return FALSE;
        if (i & 1) out[i / 2] |= (BYTE)v; else out[i / 2] = (BYTE)(v << 4);
    }
    return TRUE;
}

static void set_general(Config *c, const WCHAR *k, const WCHAR *v)
{
    if (!lstrcmpiW(k, L"accept"))          c->accept = (v[0] == L'1');
    else if (!lstrcmpiW(k, L"port"))       c->port = to_int(v, DEFAULT_PORT, 1, 65535);
    else if (!lstrcmpiW(k, L"key"))        c->haveKey = hex_key(v, c->key);
    else if (!lstrcmpiW(k, L"edge_delay")) c->edgeDelay = to_int(v, 0, 0, 5000);
    else if (!lstrcmpiW(k, L"corner"))     c->corner = to_int(v, 0, 0, 1000);
    else if (!lstrcmpiW(k, L"no_drag_switch")) c->noDragSwitch = (v[0] == L'1');
    else if (!lstrcmpiW(k, L"clipboard"))  c->clipboard = (v[0] == L'1');
    else if (!lstrcmpiW(k, L"clipboard_max_mb")) c->clipMaxMB = to_int(v, 32, 1, 512);
    else if (!lstrcmpiW(k, L"hotkey_home")) hotkey_parse(v, &c->hkHome);
    else if (!lstrcmpiW(k, L"hotkey_lock")) hotkey_parse(v, &c->hkLock);
    else if (!lstrcmpiW(k, L"osd"))        c->osd = (v[0] == L'1');
    else if (!lstrcmpiW(k, L"admin"))      c->admin = (v[0] == L'1');
    else if (!lstrcmpiW(k, L"draw_cursor")) c->drawCursor = to_int(v, 0, 0, 2);
    else if (!lstrcmpiW(k, L"log"))        c->log = (v[0] == L'1');
    else if (!lstrcmpiW(k, L"theme"))
        c->theme = !lstrcmpiW(v, L"light") ? 1 : !lstrcmpiW(v, L"dark") ? 2 : 0;
}

static void set_peer(Peer *p, const WCHAR *k, const WCHAR *v)
{
    if (!lstrcmpiW(k, L"host"))        lstrcpynW(p->host, v, HOST_MAX);
    else if (!lstrcmpiW(k, L"port"))   p->port = to_int(v, DEFAULT_PORT, 1, 65535);
    else if (!lstrcmpiW(k, L"x"))      p->gx = to_int(v, 1, -15, 15);
    else if (!lstrcmpiW(k, L"y"))      p->gy = to_int(v, 0, -15, 15);
    else if (!lstrcmpiW(k, L"hotkey")) hotkey_parse(v, &p->hk);
}

void config_load(void)
{
    Config c;
    WCHAR *text, *p, *line;
    Peer  *cur = NULL;
    BOOL   inGeneral = FALSE;
    int    i;

    ZeroMemory(&c, sizeof(c));
    c.port         = DEFAULT_PORT;
    c.accept       = TRUE;
    c.noDragSwitch = TRUE;
    c.clipboard    = TRUE;
    c.clipMaxMB    = 32;
    c.osd          = TRUE;
    c.hkHome.vk    = VK_HOME;
    c.hkHome.mods  = HK_CTRL | HK_ALT;

    text = read_text_file(g_iniPath);
    if (text) {
        for (p = text; *p; ) {
            WCHAR *eq;
            line = p;
            while (*p && *p != L'\n') p++;
            if (*p) *p++ = 0;
            trim(line);
            if (!line[0] || line[0] == L';' || line[0] == L'#') continue;
            if (line[0] == L'[') {
                inGeneral = !lstrcmpiW(line, L"[general]");
                cur = NULL;
                if (!lstrcmpiW(line, L"[peer]") && c.npeers < PEER_MAX) {
                    cur = &c.peers[c.npeers++];
                    peer_defaults(cur);
                    cur->gx = 0x7FFF;       /* 位置が書いてなければ後で空きに置く */
                }
                continue;
            }
            eq = line;
            while (*eq && *eq != L'=') eq++;
            if (!*eq) continue;
            *eq = 0;
            trim(line);
            trim(eq + 1);
            if (cur)            set_peer(cur, line, eq + 1);
            else if (inGeneral) set_general(&c, line, eq + 1);
        }
        HeapFree(GetProcessHeap(), 0, text);
    }

    /* 名前の無い相手を落とし、位置の重なり・未指定を直す */
    for (i = 0; i < c.npeers; ) {
        if (!c.peers[i].host[0]) {
            MoveMemory(&c.peers[i], &c.peers[i + 1], (c.npeers - i - 1) * sizeof(Peer));
            c.npeers--;
            continue;
        }
        i++;
    }
    for (i = 0; i < c.npeers; i++) {
        Peer *pp = &c.peers[i];
        int   j, clash = (pp->gx == 0 && pp->gy == 0) || pp->gx == 0x7FFF;
        for (j = 0; j < i && !clash; j++)
            if (c.peers[j].gx == pp->gx && c.peers[j].gy == pp->gy) clash = 1;
        if (clash) {
            Config tmp = c;
            tmp.npeers = i;
            peer_free_cell(&tmp, &pp->gx, &pp->gy);
        }
    }

    g_logInIni = c.log;
    if (g_cfg.log) c.log = TRUE;        /* -log の指定は残す */
    g_cfg = c;
}

/* ------------------------------------------------------------------ */
/*  保存                                                                */
/* ------------------------------------------------------------------ */

typedef struct { WCHAR *p; int len, cap; } SB;

static void sb_add(SB *b, const WCHAR *s)
{
    int n = lstrlenW(s);
    if (b->len + n + 1 > b->cap) {
        int    ncap = max(b->cap * 2, b->len + n + 1024);
        WCHAR *np = b->p
            ? (WCHAR *)HeapReAlloc(GetProcessHeap(), 0, b->p, ncap * sizeof(WCHAR))
            : (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ncap * sizeof(WCHAR));
        if (!np) return;
        b->p = np; b->cap = ncap;
    }
    CopyMemory(b->p + b->len, s, (n + 1) * sizeof(WCHAR));
    b->len += n;
}

static void sb_kv(SB *b, const WCHAR *k, const WCHAR *v)
{
    sb_add(b, k); sb_add(b, L"="); sb_add(b, v); sb_add(b, L"\r\n");
}

static void sb_kn(SB *b, const WCHAR *k, int v)
{
    WCHAR n[16];
    wsprintfW(n, L"%d", v);
    sb_kv(b, k, n);
}

static void sb_hk(SB *b, const WCHAR *k, const Hotkey *hk)
{
    WCHAR t[96], *s, *d;
    hotkey_format(hk, t, ARRAYSIZE(t));
    for (s = d = t; *s; s++) if (*s != L' ') *d++ = *s;     /* ini では空白を詰める */
    *d = 0;
    sb_kv(b, k, t);
}

BOOL config_save(void)
{
    SB     b = { 0 };
    WCHAR  tmp[MAX_PATH + 8], hex[65];
    char  *u8;
    int    i, n;
    HANDLE h;
    DWORD  wr;
    BOOL   ok = FALSE;

    sb_add(&b, L"; input-mouser の設定。設定画面で変えると書き直される。\r\n");
    sb_add(&b, L"; [peer] は操作する相手の PC。x, y はこの PC から見た位置(右・下が +)。\r\n\r\n");
    sb_add(&b, L"[general]\r\n");
    sb_kv(&b, L"accept", g_cfg.accept ? L"1" : L"0");
    sb_kn(&b, L"port", g_cfg.port);
    if (g_cfg.haveKey) {
        for (i = 0; i < 32; i++) wsprintfW(hex + i * 2, L"%02x", g_cfg.key[i]);
        sb_kv(&b, L"key", hex);
    }
    sb_kn(&b, L"edge_delay", g_cfg.edgeDelay);
    sb_kn(&b, L"corner", g_cfg.corner);
    sb_kv(&b, L"no_drag_switch", g_cfg.noDragSwitch ? L"1" : L"0");
    sb_kv(&b, L"clipboard", g_cfg.clipboard ? L"1" : L"0");
    sb_kn(&b, L"clipboard_max_mb", g_cfg.clipMaxMB);
    sb_hk(&b, L"hotkey_home", &g_cfg.hkHome);
    sb_hk(&b, L"hotkey_lock", &g_cfg.hkLock);
    sb_kv(&b, L"osd", g_cfg.osd ? L"1" : L"0");
    sb_kv(&b, L"admin", g_cfg.admin ? L"1" : L"0");
    sb_kn(&b, L"draw_cursor", g_cfg.drawCursor);
    if (g_cfg.theme) sb_kv(&b, L"theme", g_cfg.theme == 1 ? L"light" : L"dark");
    if (g_logInIni)  sb_kv(&b, L"log", L"1");

    for (i = 0; i < g_cfg.npeers; i++) {
        const Peer *p = &g_cfg.peers[i];
        sb_add(&b, L"\r\n[peer]\r\n");
        sb_kv(&b, L"host", p->host);
        sb_kn(&b, L"port", p->port);
        sb_kn(&b, L"x", p->gx);
        sb_kn(&b, L"y", p->gy);
        sb_hk(&b, L"hotkey", &p->hk);
    }
    if (!b.p) return FALSE;

    n  = WideCharToMultiByte(CP_UTF8, 0, b.p, b.len, NULL, 0, NULL, NULL);
    u8 = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (u8) {
        WideCharToMultiByte(CP_UTF8, 0, b.p, b.len, u8, n, NULL, NULL);
        /* 書きかけで落ちても元の設定が残るよう、別名に書いてから置き換える */
        wsprintfW(tmp, L"%s.tmp", g_iniPath);
        h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            ok = WriteFile(h, u8, n, &wr, NULL) && wr == (DWORD)n;
            CloseHandle(h);
            if (ok) ok = MoveFileExW(tmp, g_iniPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok) DeleteFileW(tmp);
        }
        HeapFree(GetProcessHeap(), 0, u8);
    }
    HeapFree(GetProcessHeap(), 0, b.p);
    if (!ok) log_printf(L"設定を保存できませんでした: %s (%lu)", g_iniPath, GetLastError());
    return ok;
}

void config_changed(void)
{
    config_save();
    hook_config_changed();
    net_config_changed();
}

BOOL config_set_password(const WCHAR *pw)
{
    BYTE k[32];
    if (!pw[0]) {
        g_cfg.haveKey = FALSE;
        ZeroMemory(g_cfg.key, sizeof(g_cfg.key));
        config_changed();
        return TRUE;
    }
    if (!crypto_derive(pw, k)) return FALSE;
    CopyMemory(g_cfg.key, k, 32);
    SecureZeroMemory(k, sizeof(k));
    g_cfg.haveKey = TRUE;
    config_changed();
    return TRUE;
}
