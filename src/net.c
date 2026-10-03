/* ==================================================================
 * net.c - 通信(TCP 1 本で入力とクリップボード、UDP で PC の探索)
 *
 *  すべてのソケットを 1 本のスレッドで扱う(WSAEventSelect のループ)。
 *  ほかのスレッドは net_send() で待ち行列に積むだけ。マウスの移動は
 *  まだ送っていないものに足し込むので、詰まっても行列は伸びない。
 *
 *  接続(こちらがマスター)は相手ごとの「接続スレッド」が名前解決と
 *  connect まで行い、つながったソケットをこのループへ渡す。
 *  名前解決は数秒止まることがあり、ループを止めないため。
 *
 *  通信の形
 *      [u32 長さ][本体]            長さはリトル エンディアン
 *  握手(平文)
 *      M→S  "IMSR" 版 乱数M[16] 名前の長さ 名前(UTF-8)
 *      S→M  "IMSR" 版 乱数S[16] 確認S[32] 名前の長さ 名前
 *      M→S  確認M[32]
 *  以後は本体が AES-256-GCM の暗号文 + tag 16 バイト(crypto.c)。
 *  平文の 1 バイト目が種類(M_*)。
 *
 *  探索(UDP、TCP と同じポート番号)
 *      "IMSR?" 版                         → ブロードキャスト
 *      "IMSR!" 版 ポート[u16] 名前の長さ 名前  ← 受け付けている PC が返す
 * ================================================================== */

#include "mouser.h"
#include <iphlpapi.h>

volatile LONG g_peerStatus[PEER_MAX];

#define PROTO_VER  1
#define HS_MAX     512
#define PING_MS    2000
#define DEAD_MS    7000
#define HS_TIMEOUT 5000
#define DISC_MS    1500

/* ------------------------------------------------------------------ */
/*  小物                                                                */
/* ------------------------------------------------------------------ */

static UINT16 le16(const BYTE *p) { return (UINT16)(p[0] | (p[1] << 8)); }
static UINT32 le32(const BYTE *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }
static void put16(BYTE *p, UINT16 v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void put32(BYTE *p, UINT32 v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
static void  mem_free(void *p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }

typedef struct { BYTE *p; int len, off, cap; } Buf;

static BOOL buf_room(Buf *b, int add)
{
    if (b->off && b->off == b->len) b->off = b->len = 0;
    if (b->len + add <= b->cap) return TRUE;
    if (b->off) {                               /* 読み終えた分を詰める */
        MoveMemory(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off  = 0;
        if (b->len + add <= b->cap) return TRUE;
    }
    {
        int   ncap = max(b->cap * 2, b->len + add + 4096);
        BYTE *np   = b->p ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, b->p, ncap) : (BYTE *)mem_alloc(ncap);
        if (!np) return FALSE;
        b->p = np; b->cap = ncap;
    }
    return TRUE;
}

static void buf_free(Buf *b) { mem_free(b->p); ZeroMemory(b, sizeof(*b)); }

static void addr_text(const struct sockaddr *sa, WCHAR *out, int cch)
{
    out[0] = 0;
    if (sa->sa_family == AF_INET) {
        InetNtopW(AF_INET, (void *)&((const struct sockaddr_in *)sa)->sin_addr, out, cch);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr))
            InetNtopW(AF_INET, (void *)&s6->sin6_addr.s6_addr[12], out, cch);
        else
            InetNtopW(AF_INET6, (void *)&s6->sin6_addr, out, cch);
    }
}

static void set_status(int peer, int st)
{
    if (peer < 0 || peer >= PEER_MAX) return;
    if (InterlockedExchange(&g_peerStatus[peer], st) != st && g_trayWnd)
        PostMessageW(g_trayWnd, WM_APP_STATUS, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  ほかのスレッドからの待ち行列                                        */
/* ------------------------------------------------------------------ */

enum { Q_SEND, Q_CONFIG, Q_SOCKET, Q_DISCOVER, Q_RECONNECT, Q_STOP };

typedef struct QMsg {
    struct QMsg *next;
    int      kind;
    int      conn;
    BYTE     type;
    int      len;
    BYTE    *data;      /* inl を指すか、HeapAlloc したもの */
    LONG_PTR arg;
    BYTE     inl[16];
} QMsg;

static SRWLOCK g_qLock = SRWLOCK_INIT;
static QMsg   *g_qHead, *g_qTail;
static HANDLE  g_qEvent, g_thread;

static void q_push(QMsg *m)
{
    m->next = NULL;
    AcquireSRWLockExclusive(&g_qLock);
    if (g_qTail) g_qTail->next = m; else g_qHead = m;
    g_qTail = m;
    ReleaseSRWLockExclusive(&g_qLock);
    SetEvent(g_qEvent);
}

static QMsg *q_new(int kind)
{
    QMsg *m = (QMsg *)mem_alloc(sizeof(QMsg));
    if (m) { ZeroMemory(m, sizeof(*m)); m->kind = kind; m->data = m->inl; }
    return m;
}

static void q_free(QMsg *m)
{
    if (m->data && m->data != m->inl) mem_free(m->data);
    mem_free(m);
}

void net_send(int conn, BYTE type, const void *data, int len)
{
    QMsg *m;

    if (!g_qEvent) return;
    if (type == M_MOVE && len == 8) {           /* まだ送っていない移動に足す */
        AcquireSRWLockExclusive(&g_qLock);
        if (g_qTail && g_qTail->kind == Q_SEND && g_qTail->type == M_MOVE && g_qTail->conn == conn) {
            BYTE *p = g_qTail->data;
            put32(p,     (UINT32)((INT32)le32(p)     + (INT32)le32((const BYTE *)data)));
            put32(p + 4, (UINT32)((INT32)le32(p + 4) + (INT32)le32((const BYTE *)data + 4)));
            ReleaseSRWLockExclusive(&g_qLock);
            return;
        }
        ReleaseSRWLockExclusive(&g_qLock);
    }
    m = q_new(Q_SEND);
    if (!m) return;
    m->conn = conn;
    m->type = type;
    m->len  = len;
    if (len > (int)sizeof(m->inl)) {
        m->data = (BYTE *)mem_alloc(len);
        if (!m->data) { mem_free(m); return; }
    }
    if (len) CopyMemory(m->data, data, len);
    q_push(m);
}

void net_send_owned(int conn, BYTE type, void *heapData, int len)
{
    QMsg *m = q_new(Q_SEND);
    if (!m || !g_qEvent) { mem_free(heapData); mem_free(m); return; }
    m->conn = conn;
    m->type = type;
    m->len  = len;
    m->data = (BYTE *)heapData;
    q_push(m);
}

void net_config_changed(void)
{
    QMsg   *m = q_new(Q_CONFIG);
    Config *c;
    if (!m) return;
    c = (Config *)mem_alloc(sizeof(Config));
    if (!c) { mem_free(m); return; }
    *c = g_cfg;
    m->arg = (LONG_PTR)c;
    q_push(m);
}

void net_discover(HWND notify)
{
    QMsg *m = q_new(Q_DISCOVER);
    if (!m) return;
    m->arg = (LONG_PTR)notify;
    q_push(m);
}

void net_reconnect_now(void)
{
    QMsg *m = q_new(Q_RECONNECT);
    if (m) q_push(m);
}

/* ------------------------------------------------------------------ */
/*  探索の呼びかけ(UDP のブロードキャスト)                              */
/* ------------------------------------------------------------------ */

static SOCKET disc_socket(void)
{
    BOOL   on = TRUE;
    struct sockaddr_in me;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
    ZeroMemory(&me, sizeof(me));
    me.sin_family = AF_INET;
    bind(s, (struct sockaddr *)&me, sizeof(me));
    return s;
}

static void disc_send_to(SOCKET s, ULONG addr, int port)
{
    struct sockaddr_in to;
    BYTE q[6];
    CopyMemory(q, "IMSR?", 5);
    q[5] = PROTO_VER;
    ZeroMemory(&to, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_port        = htons((u_short)port);
    to.sin_addr.s_addr = addr;
    sendto(s, (const char *)q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to));
}

/* 255.255.255.255 は 1 つの経路にしか出ないことがあるので、
   つながっているネットワークごとの宛先(192.168.0.255 など)にも送る */
static void disc_broadcast(SOCKET s, const int *ports, int np)
{
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *aa = NULL, *a;
    int   i;

    for (i = 0; i < np; i++) {
        disc_send_to(s, INADDR_BROADCAST, ports[i]);
        if (g_bindAddr[0]) disc_send_to(s, htonl(INADDR_LOOPBACK), ports[i]);    /* 検証用 */
    }
    aa = (IP_ADAPTER_ADDRESSES *)mem_alloc(size);
    if (aa && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                            GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        mem_free(aa);
        aa = (IP_ADAPTER_ADDRESSES *)mem_alloc(size);
        if (aa && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &size) != NO_ERROR) {
            mem_free(aa);
            aa = NULL;
        }
    }
    for (a = aa; a; a = a->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (u = a->FirstUnicastAddress; u; u = u->Next) {
            ULONG ip, mask;
            int   pl = u->OnLinkPrefixLength;
            if (u->Address.lpSockaddr->sa_family != AF_INET || pl <= 0 || pl >= 31) continue;
            ip   = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
            mask = 0xFFFFFFFFu << (32 - pl);
            for (i = 0; i < np; i++) disc_send_to(s, htonl(ip | ~mask), ports[i]);
        }
    }
    mem_free(aa);
}

/* 返事の名前と、登録した名前が同じ PC を指すか(大文字小文字と、
   「.local」などドメインの付け外しは区別しない) */
static BOOL same_host(const WCHAR *host, const WCHAR *name)
{
    WCHAR a[HOST_MAX], b[HOST_MAX], *p;
    if (!lstrcmpiW(host, name)) return TRUE;
    lstrcpynW(a, host, HOST_MAX);
    lstrcpynW(b, name, HOST_MAX);
    for (p = a; *p && *p != L'.'; p++) ;
    *p = 0;
    for (p = b; *p && *p != L'.'; p++) ;
    *p = 0;
    return a[0] && !lstrcmpiW(a, b);
}

/* ------------------------------------------------------------------ */
/*  接続スレッド(こちらがマスター。相手ごとに 1 本)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile LONG ref;
    volatile LONG stop;
    HANDLE        wake;         /* 切れた・やめる、の知らせ(自動リセット) */
    int           peer;
    LONG          gen;
    WCHAR         host[HOST_MAX];
    int           port;
} Connector;

static Connector *g_connectors[PEER_MAX];
static void read_name(const BYTE *p, int avail, WCHAR *out);
static LONG       g_gen[PEER_MAX];

static void connector_release(Connector *c)
{
    if (InterlockedDecrement(&c->ref) == 0) {
        CloseHandle(c->wake);
        mem_free(c);
    }
}

/* 1 つの宛先へ。つながるまで最大 3 秒待つ。やめる指示があれば早めに戻る */
static SOCKET try_connect(Connector *c, const struct sockaddr *sa, int salen)
{
    u_long nb = 1;
    int    i, err = 0, elen = sizeof(err);
    SOCKET s = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);

    if (s == INVALID_SOCKET) return s;
    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, sa, salen) == 0) return s;
    if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
    for (i = 0; i < 12 && !c->stop; i++) {          /* 250ms × 12 */
        fd_set w, e;
        struct timeval tv = { 0, 250000 };
        FD_ZERO(&w); FD_ZERO(&e);
        FD_SET(s, &w); FD_SET(s, &e);
        if (select(0, NULL, &w, &e, &tv) > 0) break;
    }
    if (!c->stop && getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) == 0 && err == 0) {
        fd_set w;
        struct timeval tv = { 0, 0 };
        FD_ZERO(&w); FD_SET(s, &w);
        if (select(0, NULL, &w, NULL, &tv) > 0) return s;
    }
    closesocket(s);
    return INVALID_SOCKET;
}

/* input-mouser 自身の PC 探索で、登録した名前・ポートの PC を探す。
   Windows の名前解決(ルーターの DNS、LLMNR、NetBIOS)に頼らない。
   ルーターが名前を知らず、相手のネットワークが「パブリック」で名前の
   問い合わせに答えないときでも、探索に答える PC なら見つかる。 */
static BOOL find_by_name(Connector *c, struct sockaddr_in *out)
{
    SOCKET s = disc_socket();
    DWORD  until = GetTickCount() + 1000;
    BOOL   found = FALSE;

    if (s == INVALID_SOCKET) return FALSE;
    disc_broadcast(s, &c->port, 1);
    while (!found && !c->stop && (LONG)(until - GetTickCount()) > 0) {
        BYTE   buf[600];
        WCHAR  name[HOST_MAX];
        struct sockaddr_in from;
        int    fl = sizeof(from), n;
        fd_set r;
        struct timeval tv = { 0, 100000 };

        FD_ZERO(&r); FD_SET(s, &r);
        if (select(0, &r, NULL, NULL, &tv) <= 0) continue;
        n = recvfrom(s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 9 || memcmp(buf, "IMSR!", 5) || from.sin_family != AF_INET) continue;
        if (le16(buf + 6) != c->port) continue;
        read_name(buf + 8, n - 8, name);
        if (!same_host(c->host, name)) continue;
        *out = from;
        out->sin_port = htons((u_short)c->port);
        found = TRUE;
    }
    closesocket(s);
    return found;
}

static BOOL is_ip_literal(const WCHAR *host)
{
    BYTE b[16];
    return InetPtonW(AF_INET, host, b) == 1 || InetPtonW(AF_INET6, host, b) == 1;
}

static SOCKET connect_host(Connector *c)
{
    ADDRINFOW  hints, *res = NULL, *ai;
    WCHAR      port[8];
    SOCKET     s = INVALID_SOCKET;
    BOOL       resolved;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    wsprintfW(port, L"%d", c->port);
    resolved = GetAddrInfoW(c->host, port, &hints, &res) == 0;
    if (resolved) {
        for (ai = res; ai && !c->stop && s == INVALID_SOCKET; ai = ai->ai_next)
            s = try_connect(c, ai->ai_addr, (int)ai->ai_addrlen);
        FreeAddrInfoW(res);
    }

    /* 名前が引けない、または引けた IP にいない(IP が変わった)とき */
    if (s == INVALID_SOCKET && !c->stop && !is_ip_literal(c->host)) {
        struct sockaddr_in a;
        if (find_by_name(c, &a)) {
            WCHAR ip[64];
            addr_text((struct sockaddr *)&a, ip, 64);
            s = try_connect(c, (struct sockaddr *)&a, sizeof(a));
            if (s != INVALID_SOCKET)
                log_printf(resolved ? L"%s は名前で引いた IP にいなかったので、PC 探索で見つけた %s につなぎました"
                                    : L"%s の名前を引けなかったので、PC 探索で見つけた %s につなぎました",
                           c->host, ip);
        }
    }
    return s;
}

static DWORD WINAPI connector_thread(LPVOID arg)
{
    Connector *c = (Connector *)arg;

    while (!c->stop) {
        SOCKET s;
        /* 「接続しています」は最初の 1 回だけ。断られた・見つからない表示は、
           つながるまで残す(試すたびに表示が入れ替わってちらつかないように) */
        if (g_peerStatus[c->peer] == PS_OFF) set_status(c->peer, PS_CONNECTING);
        s = connect_host(c);
        if (c->stop) { if (s != INVALID_SOCKET) closesocket(s); break; }
        if (s != INVALID_SOCKET) {
            QMsg *m = q_new(Q_SOCKET);
            if (m) {
                m->conn = c->peer;
                m->len  = (int)c->gen;
                m->arg  = (LONG_PTR)s;
                q_push(m);
                WaitForSingleObject(c->wake, INFINITE);     /* 切れるまで待つ */
            } else {
                closesocket(s);
            }
            if (c->stop) break;
            /* パスワード違いなどで断られたときは間を空ける */
            WaitForSingleObject(c->wake, (g_peerStatus[c->peer] == PS_BADPASS ||
                                          g_peerStatus[c->peer] == PS_REFUSED) ? 10000 : 1000);
        } else {
            set_status(c->peer, PS_UNREACHABLE);
            WaitForSingleObject(c->wake, 3000);
        }
    }
    connector_release(c);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  接続                                                                */
/* ------------------------------------------------------------------ */

enum { CS_HELLO, CS_PROOF, CS_READY };

typedef struct {
    SOCKET   s;
    WSAEVENT ev;
    BOOL     out;           /* こちらから(相手を操作する) */
    int      peer;          /* out のとき */
    int      id;            /* 接続 ID(out は peer と同じ) */
    int      state;
    BYTE     nM[16], nS[16];
    Gcm      tx, rx;
    Buf      in, ob;
    DWORD    since, lastRx, lastPing;
    WCHAR    name[HOST_MAX];
    WCHAR    addr[64];
    BOOL     dead;
    int      endStatus;     /* 閉じたときの相手の状態(out) */
} Conn;

#define CONN_MAX (PEER_MAX + INCOMING_MAX)

static Conn   *g_conns[CONN_MAX];
static int     g_nextInId = CONN_IN_BASE;
static Config  g_nc;                /* このスレッドが使う設定の写し */
static BYTE    g_key[32];
static SOCKET  g_listen = INVALID_SOCKET, g_udp = INVALID_SOCKET, g_disc = INVALID_SOCKET;
static WSAEVENT g_listenEv, g_udpEv, g_discEv;
static HWND    g_discHwnd;
static DWORD   g_discUntil;
static int     g_listenPort, g_listenAccept = -1;
static WCHAR   g_listenBind[64];

static SRWLOCK g_namesLock = SRWLOCK_INIT;
static WCHAR   g_inNames[512];

static void update_in_names(void)
{
    WCHAR buf[512];
    int   i;
    buf[0] = 0;
    for (i = 0; i < CONN_MAX; i++) {
        Conn *c = g_conns[i];
        if (!c || c->out || c->state != CS_READY) continue;
        if (buf[0] && lstrlenW(buf) + 2 < (int)ARRAYSIZE(buf)) lstrcatW(buf, L"、");
        if (lstrlenW(buf) + lstrlenW(c->name) + 1 < (int)ARRAYSIZE(buf)) lstrcatW(buf, c->name);
    }
    AcquireSRWLockExclusive(&g_namesLock);
    lstrcpyW(g_inNames, buf);
    ReleaseSRWLockExclusive(&g_namesLock);
    if (g_trayWnd) PostMessageW(g_trayWnd, WM_APP_STATUS, 0, 0);
}

int net_incoming_names(WCHAR *buf, int cch)
{
    AcquireSRWLockShared(&g_namesLock);
    lstrcpynW(buf, g_inNames, cch);
    ReleaseSRWLockShared(&g_namesLock);
    return lstrlenW(buf);
}

static Conn *conn_find(int id)
{
    int i;
    for (i = 0; i < CONN_MAX; i++)
        if (g_conns[i] && g_conns[i]->id == id && !g_conns[i]->dead) return g_conns[i];
    return NULL;
}

static void conn_flush(Conn *c)
{
    while (!c->dead && c->ob.off < c->ob.len) {
        int n = send(c->s, (const char *)c->ob.p + c->ob.off, min(c->ob.len - c->ob.off, 1 << 20), 0);
        if (n > 0) { c->ob.off += n; continue; }
        if (WSAGetLastError() != WSAEWOULDBLOCK) c->dead = TRUE;
        break;
    }
    if (c->ob.off == c->ob.len) c->ob.off = c->ob.len = 0;
}

static void conn_raw(Conn *c, const BYTE *data, int len)
{
    if (!buf_room(&c->ob, len + 4)) { c->dead = TRUE; return; }
    put32(c->ob.p + c->ob.len, (UINT32)len);
    CopyMemory(c->ob.p + c->ob.len + 4, data, len);
    c->ob.len += 4 + len;
    conn_flush(c);
}

static void conn_msg(Conn *c, BYTE type, const BYTE *data, int len)
{
    BYTE  small[64], *plain = small;
    int   n = 1 + len;

    if (c->dead || c->state != CS_READY) return;
    if (c->ob.len - c->ob.off > 256 * 1024 * 1024) { c->dead = TRUE; return; }   /* 相手が受け取らない */
    if (n > (int)sizeof(small)) {
        plain = (BYTE *)mem_alloc(n);
        if (!plain) return;
    }
    plain[0] = type;
    if (len) CopyMemory(plain + 1, data, len);
    if (buf_room(&c->ob, 4 + n + 16)) {
        BYTE *dst = c->ob.p + c->ob.len;
        put32(dst, (UINT32)(n + 16));
        if (gcm_seal(&c->tx, plain, (ULONG)n, dst + 4)) c->ob.len += 4 + n + 16;
        else c->dead = TRUE;
    } else {
        c->dead = TRUE;
    }
    if (plain != small) { SecureZeroMemory(plain, n); mem_free(plain); }
    conn_flush(c);
}

static int hello(BYTE *out, const char *magic, const BYTE nonce[16], const BYTE *proof)
{
    char name[HOST_MAX * 3];
    int  n = 0, nl;
    CopyMemory(out, magic, 4); n = 4;
    out[n++] = PROTO_VER;
    CopyMemory(out + n, nonce, 16); n += 16;
    if (proof) { CopyMemory(out + n, proof, 32); n += 32; }
    nl = WideCharToMultiByte(CP_UTF8, 0, g_hostName, -1, name, sizeof(name), NULL, NULL) - 1;
    if (nl < 0) nl = 0;
    if (nl > 200) nl = 200;
    out[n++] = (BYTE)nl;
    CopyMemory(out + n, name, nl);
    return n + nl;
}

static void read_name(const BYTE *p, int avail, WCHAR *out)
{
    int nl = avail > 0 ? p[0] : 0;
    out[0] = 0;
    if (nl > avail - 1) nl = avail - 1;
    if (nl > 0) {
        int w = MultiByteToWideChar(CP_UTF8, 0, (const char *)p + 1, nl, out, HOST_MAX - 1);
        out[w > 0 ? w : 0] = 0;
    }
}

static void session_keys(Conn *c)
{
    BYTE k[32];
    crypto_hmac(g_key, "K", 1, c->nM, 16, c->nS, 16, k);
    gcm_init(&c->tx, k, c->out ? 'M' : 'S');
    gcm_init(&c->rx, k, c->out ? 'S' : 'M');
    SecureZeroMemory(k, sizeof(k));
}

static Conn *conn_new(SOCKET s, BOOL out, int peer)
{
    int   i;
    Conn *c;
    BOOL  nodelay = TRUE;

    for (i = 0; i < CONN_MAX && g_conns[i]; i++) ;
    if (i == CONN_MAX) { closesocket(s); return NULL; }
    c = (Conn *)mem_alloc(sizeof(Conn));
    if (!c) { closesocket(s); return NULL; }
    ZeroMemory(c, sizeof(*c));
    c->s     = s;
    c->ev    = WSACreateEvent();
    c->out   = out;
    c->peer  = out ? peer : -1;
    c->id    = out ? peer : g_nextInId++;
    c->state = CS_HELLO;
    c->since = c->lastRx = c->lastPing = GetTickCount();
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    {
        BOOL ka = TRUE;
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&ka, sizeof(ka));
    }
    {
        struct sockaddr_storage ss;
        int sl = sizeof(ss);
        if (getpeername(s, (struct sockaddr *)&ss, &sl) == 0) addr_text((struct sockaddr *)&ss, c->addr, 64);
    }
    WSAEventSelect(s, c->ev, FD_READ | FD_WRITE | FD_CLOSE);
    g_conns[i] = c;

    if (out) {
        BYTE h[HS_MAX];
        crypto_random(c->nM, 16);
        conn_raw(c, h, hello(h, "IMSR", c->nM, NULL));
        lstrcpynW(c->name, g_nc.peers[peer].host, HOST_MAX);
    }
    return c;
}

static void conn_close(int slot)
{
    Conn *c = g_conns[slot];
    BOOL  wasReady;

    if (!c) return;
    g_conns[slot] = NULL;
    wasReady = c->state == CS_READY;
    closesocket(c->s);
    WSACloseEvent(c->ev);
    gcm_free(&c->tx);
    gcm_free(&c->rx);
    buf_free(&c->in);
    buf_free(&c->ob);

    if (c->out) {
        if (wasReady) log_printf(L"%s との接続が切れました", c->name);
        set_status(c->peer, c->endStatus ? c->endStatus : PS_OFF);
        hook_peer_down(c->peer);
        if (g_connectors[c->peer]) SetEvent(g_connectors[c->peer]->wake);
    } else {
        if (wasReady) log_printf(L"%s (%s) からの操作が終わりました", c->name, c->addr);
        else if (c->state == CS_PROOF)      /* 確認を見た相手が去った = 鍵が違う */
            log_printf(L"%s (%s) からの接続は、パスワードが一致しませんでした", c->name, c->addr);
        inj_conn_closed(c->id);
        if (g_trayWnd) PostMessageW(g_trayWnd, WM_APP_STATUS, 1, c->id);
        update_in_names();
    }
    mem_free(c);
}

static void post_clip(int conn, const BYTE *p, int len)
{
    ClipData *cd;
    if (!g_nc.clipboard || !g_trayWnd) return;
    cd = (ClipData *)mem_alloc(sizeof(ClipData) + len);
    if (!cd) return;
    cd->len = len;
    CopyMemory(cd->data, p, len);
    if (!PostMessageW(g_trayWnd, WM_APP_CLIPRECV, (WPARAM)conn, (LPARAM)cd)) mem_free(cd);
}

/* 握手 */
static void on_handshake(Conn *c, const BYTE *p, int n)
{
    BYTE proof[32], h[HS_MAX];

    if (c->out) {                                   /* 相手(スレーブ)の返事 */
        if (n < 4 + 1 + 16 + 32 + 1 || memcmp(p, "IMSR", 4)) { c->dead = TRUE; return; }
        if (p[4] != PROTO_VER) {
            log_printf(L"%s: 版が合いません(相手 %d、こちら %d)", c->name, p[4], PROTO_VER);
            c->endStatus = PS_REFUSED;
            c->dead = TRUE;
            return;
        }
        CopyMemory(c->nS, p + 5, 16);
        crypto_hmac(g_key, "S", 1, c->nM, 16, c->nS, 16, proof);
        if (memcmp(proof, p + 21, 32)) {
            if (g_peerStatus[c->peer] != PS_BADPASS) log_printf(L"%s: パスワードが一致しません", c->name);
            c->endStatus = PS_BADPASS;
            c->dead = TRUE;
            return;
        }
        crypto_hmac(g_key, "C", 1, c->nM, 16, c->nS, 16, proof);
        conn_raw(c, proof, 32);
        session_keys(c);
        c->state = CS_READY;
        log_printf(L"%s (%s) につながりました", c->name, c->addr);
        set_status(c->peer, PS_READY);
        return;
    }

    if (c->state == CS_HELLO) {                     /* マスターの挨拶 */
        if (n < 4 + 1 + 16 + 1 || memcmp(p, "IMSR", 4)) { c->dead = TRUE; return; }
        if (p[4] != PROTO_VER) { c->dead = TRUE; return; }
        CopyMemory(c->nM, p + 5, 16);
        read_name(p + 21, n - 21, c->name);
        if (!c->name[0]) lstrcpynW(c->name, c->addr, HOST_MAX);
        crypto_random(c->nS, 16);
        crypto_hmac(g_key, "S", 1, c->nM, 16, c->nS, 16, proof);
        conn_raw(c, h, hello(h, "IMSR", c->nS, proof));
        c->state = CS_PROOF;
        return;
    }
    /* CS_PROOF: マスターの確認 */
    crypto_hmac(g_key, "C", 1, c->nM, 16, c->nS, 16, proof);
    if (n != 32 || memcmp(proof, p, 32)) {
        log_printf(L"%s (%s) からの接続を断りました(パスワードが一致しません)", c->name, c->addr);
        c->dead = TRUE;
        return;
    }
    session_keys(c);
    c->state = CS_READY;
    log_printf(L"%s (%s) から操作できるようになりました", c->name, c->addr);
    update_in_names();
}

static void on_message(Conn *c, const BYTE *p, int n)
{
    BYTE type = p[0];
    p++; n--;

    if (c->out) {
        switch (type) {
        case M_EDGE:
            if (n >= 5) hook_peer_edge(c->peer, p[0], le16(p + 1), le16(p + 3));
            break;
        case M_CLIP:
            post_clip(c->id, p, n);
            break;
        }
        return;
    }
    switch (type) {
    case M_ENTER:   if (n >= 3) inj_enter(c->id, p[0], le16(p + 1)); break;
    case M_LEAVE:
        inj_leave(c->id);
        if (g_nc.clipboard && g_trayWnd) PostMessageW(g_trayWnd, WM_APP_CLIPSEND, (WPARAM)c->id, 0);
        break;
    case M_MOVE:    if (n >= 8) inj_move(c->id, (INT32)le32(p), (INT32)le32(p + 4)); break;
    case M_BUTTON:  if (n >= 2) inj_button(p[0], p[1] != 0); break;
    case M_WHEEL:   if (n >= 3) inj_wheel(p[0] != 0, (INT16)le16(p + 1)); break;
    case M_KEY:     if (n >= 5) inj_key(le16(p), le16(p + 2), p[4]); break;
    case M_RELEASE: inj_release_all(); break;
    case M_CLIP:    post_clip(c->id, p, n); break;
    case M_PING:    conn_msg(c, M_PONG, NULL, 0); break;
    }
}

static void conn_read(Conn *c)
{
    UINT32 limit = c->state == CS_READY ? (UINT32)(g_nc.clipMaxMB + 1) * 1024 * 1024 + 64 : HS_MAX;

    for (;;) {
        int n;
        if (!buf_room(&c->in, 65536)) { c->dead = TRUE; return; }
        n = recv(c->s, (char *)c->in.p + c->in.len, c->in.cap - c->in.len, 0);
        if (n > 0) { c->in.len += n; c->lastRx = GetTickCount(); }
        else if (n == 0) { c->dead = TRUE; break; }
        else { if (WSAGetLastError() != WSAEWOULDBLOCK) c->dead = TRUE; break; }

        while (!c->dead && c->in.len - c->in.off >= 4) {
            BYTE  *f = c->in.p + c->in.off;
            UINT32 fl = le32(f);
            if (fl > limit || fl == 0) { c->dead = TRUE; break; }
            if ((UINT32)(c->in.len - c->in.off - 4) < fl) break;
            if (c->state != CS_READY) {
                on_handshake(c, f + 4, (int)fl);
            } else {
                BYTE  small[256], *plain = small;
                ULONG pn = fl - 16;
                if (fl < 17) { c->dead = TRUE; break; }
                if (pn > sizeof(small)) plain = (BYTE *)mem_alloc(pn);
                if (!plain) { c->dead = TRUE; break; }
                if (gcm_open(&c->rx, c->out ? 'S' : 'M', f + 4, fl, plain)) on_message(c, plain, (int)pn);
                else { log_printf(L"%s: 復号できない通信を受けたので切断します", c->name); c->dead = TRUE; }
                if (plain != small) mem_free(plain);
            }
            c->in.off += 4 + (int)fl;
            limit = c->state == CS_READY ? (UINT32)(g_nc.clipMaxMB + 1) * 1024 * 1024 + 64 : HS_MAX;
        }
        if (c->dead) break;
    }
}

/* ------------------------------------------------------------------ */
/*  待ち受けと探索                                                      */
/* ------------------------------------------------------------------ */

static void close_listen(void)
{
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    if (g_udp != INVALID_SOCKET)    { closesocket(g_udp);    g_udp = INVALID_SOCKET; }
}

static void open_listen(void)
{
    struct sockaddr_storage ss;
    int    sl;
    BOOL   on = TRUE;

    close_listen();
    g_listenAccept = g_nc.accept;
    g_listenPort   = g_nc.port;
    lstrcpynW(g_listenBind, g_bindAddr, 64);
    if (!g_nc.accept) return;

    ZeroMemory(&ss, sizeof(ss));
    if (g_bindAddr[0]) {                    /* 検証用: 指定のアドレスだけで受け付ける */
        ADDRINFOW hints, *res = NULL;
        WCHAR     port[8];
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_flags  = AI_NUMERICHOST | AI_PASSIVE;
        hints.ai_family = AF_UNSPEC;
        wsprintfW(port, L"%d", g_nc.port);
        if (GetAddrInfoW(g_bindAddr, port, &hints, &res) != 0 || !res) {
            log_printf(L"-bind のアドレス %s を解釈できません", g_bindAddr);
            return;
        }
        CopyMemory(&ss, res->ai_addr, res->ai_addrlen);
        sl = (int)res->ai_addrlen;
        FreeAddrInfoW(res);
        g_listen = socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
    } else {                                /* IPv6 と IPv4 の両方を 1 本で */
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
        DWORD v6only = 0;
        g_listen = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (g_listen != INVALID_SOCKET) {
            setsockopt(g_listen, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
            a6->sin6_family = AF_INET6;
            a6->sin6_port   = htons((u_short)g_nc.port);
            sl = sizeof(*a6);
        } else {
            struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
            g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            a4->sin_family = AF_INET;
            a4->sin_port   = htons((u_short)g_nc.port);
            sl = sizeof(*a4);
        }
    }
    if (g_listen == INVALID_SOCKET) return;
    setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
    if (bind(g_listen, (struct sockaddr *)&ss, sl) != 0 || listen(g_listen, 8) != 0) {
        log_printf(L"ポート %d で受け付けられません (%d)", g_nc.port, WSAGetLastError());
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return;
    }
    WSAEventSelect(g_listen, g_listenEv, FD_ACCEPT);
    log_printf(L"ポート %d で操作を受け付けています", g_nc.port);

    /* 探索に答える UDP。IPv4 のブロードキャストだけ */
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp != INVALID_SOCKET) {
        struct sockaddr_in a4;
        ZeroMemory(&a4, sizeof(a4));
        a4.sin_family = AF_INET;
        a4.sin_port   = htons((u_short)g_nc.port);
        if (ss.ss_family == AF_INET) a4.sin_addr = ((struct sockaddr_in *)&ss)->sin_addr;
        if (bind(g_udp, (struct sockaddr *)&a4, sizeof(a4)) != 0) {
            closesocket(g_udp);
            g_udp = INVALID_SOCKET;
        } else {
            WSAEventSelect(g_udp, g_udpEv, FD_READ);
        }
    }
}

static void on_accept(void)
{
    int i, nin = 0;
    for (;;) {
        SOCKET s = accept(g_listen, NULL, NULL);
        if (s == INVALID_SOCKET) break;
        for (i = 0, nin = 0; i < CONN_MAX; i++) if (g_conns[i] && !g_conns[i]->out) nin++;
        if (nin >= INCOMING_MAX) { closesocket(s); continue; }
        conn_new(s, FALSE, -1);
    }
}

static void on_udp(void)
{
    BYTE buf[600], out[300];
    char name[HOST_MAX * 3];
    struct sockaddr_storage from;
    int  fl, n, nl;

    for (;;) {
        fl = sizeof(from);
        n  = recvfrom(g_udp, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        if (n < 6 || memcmp(buf, "IMSR?", 5)) continue;
        CopyMemory(out, "IMSR!", 5);
        out[5] = PROTO_VER;
        put16(out + 6, (UINT16)g_nc.port);
        nl = WideCharToMultiByte(CP_UTF8, 0, g_hostName, -1, name, sizeof(name), NULL, NULL) - 1;
        if (nl < 0) nl = 0;
        if (nl > 200) nl = 200;
        out[8] = (BYTE)nl;
        CopyMemory(out + 9, name, nl);
        sendto(g_udp, (const char *)out, 9 + nl, 0, (struct sockaddr *)&from, fl);
    }
}

static void start_discover(HWND notify)
{
    int ports[2 + PEER_MAX], np = 0, i, j;

    g_discHwnd  = notify;
    g_discUntil = GetTickCount() + DISC_MS;
    if (g_disc == INVALID_SOCKET) {
        g_disc = disc_socket();
        if (g_disc == INVALID_SOCKET) return;
        WSAEventSelect(g_disc, g_discEv, FD_READ);
    }
    ports[np++] = g_nc.port;
    if (g_nc.port != DEFAULT_PORT) ports[np++] = DEFAULT_PORT;
    for (i = 0; i < g_nc.npeers; i++) {     /* 登録した相手のポートも */
        for (j = 0; j < np && ports[j] != g_nc.peers[i].port; j++) ;
        if (j == np) ports[np++] = g_nc.peers[i].port;
    }
    disc_broadcast(g_disc, ports, np);
}

static void on_disc(void)
{
    BYTE buf[600];
    struct sockaddr_storage from;
    int  fl, n;

    for (;;) {
        Found *f;
        fl = sizeof(from);
        n  = recvfrom(g_disc, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        if (n < 9 || memcmp(buf, "IMSR!", 5) || !g_discHwnd) continue;
        f = (Found *)mem_alloc(sizeof(Found));
        if (!f) continue;
        ZeroMemory(f, sizeof(*f));
        f->port = le16(buf + 6);
        read_name(buf + 8, n - 8, f->name);
        addr_text((struct sockaddr *)&from, f->addr, 64);
        if (!PostMessageW(g_discHwnd, WM_APP_FOUND, buf[5], (LPARAM)f)) mem_free(f);
    }
}

/* ------------------------------------------------------------------ */
/*  設定の反映                                                          */
/* ------------------------------------------------------------------ */

static void stop_connectors(void)
{
    int i;
    for (i = 0; i < PEER_MAX; i++) {
        Connector *c = g_connectors[i];
        if (!c) continue;
        g_connectors[i] = NULL;
        InterlockedExchange(&c->stop, 1);
        SetEvent(c->wake);
        connector_release(c);
    }
}

static void start_connectors(void)
{
    int i;
    for (i = 0; i < PEER_MAX; i++) set_status(i, PS_OFF);
    for (i = 0; i < g_nc.npeers; i++) {
        Connector *c = (Connector *)mem_alloc(sizeof(Connector));
        HANDLE     th;
        if (!c) continue;
        ZeroMemory(c, sizeof(*c));
        c->ref  = 2;                    /* このスレッドと接続スレッド */
        c->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        c->peer = i;
        c->gen  = InterlockedIncrement(&g_gen[i]);
        c->port = g_nc.peers[i].port;
        lstrcpynW(c->host, g_nc.peers[i].host, HOST_MAX);
        th = CreateThread(NULL, 64 * 1024, connector_thread, c, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!th) { CloseHandle(c->wake); mem_free(c); continue; }
        CloseHandle(th);
        g_connectors[i] = c;
    }
}

static BOOL peers_differ(const Config *a, const Config *b)
{
    int i;
    if (a->npeers != b->npeers) return TRUE;
    for (i = 0; i < a->npeers; i++)
        if (lstrcmpiW(a->peers[i].host, b->peers[i].host) || a->peers[i].port != b->peers[i].port)
            return TRUE;
    return FALSE;
}

static void apply_config(Config *nc, BOOL first)
{
    BOOL keyChanged   = first || nc->haveKey != g_nc.haveKey || memcmp(nc->key, g_nc.key, 32);
    BOOL peersChanged = first || peers_differ(nc, &g_nc);
    BOOL listenChanged = first || nc->accept != g_listenAccept || nc->port != g_listenPort ||
                         lstrcmpiW(g_bindAddr, g_listenBind);
    int  i;

    g_nc = *nc;
    if (keyChanged) {
        if (g_nc.haveKey) CopyMemory(g_key, g_nc.key, 32);
        else              crypto_derive(L"", g_key);
    }
    if (keyChanged || peersChanged) {
        stop_connectors();
        for (i = 0; i < CONN_MAX; i++)
            if (g_conns[i] && g_conns[i]->out) conn_close(i);
    }
    if (keyChanged || listenChanged) {
        for (i = 0; i < CONN_MAX; i++)
            if (g_conns[i] && !g_conns[i]->out && (keyChanged || !g_nc.accept)) conn_close(i);
    }
    if (listenChanged) open_listen();
    if (keyChanged || peersChanged) start_connectors();
}

/* ------------------------------------------------------------------ */
/*  ループ                                                              */
/* ------------------------------------------------------------------ */

static DWORD WINAPI net_thread(LPVOID arg)
{
    BOOL running = TRUE;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    g_listenAccept = -1;
    apply_config((Config *)arg, TRUE);
    mem_free(arg);

    while (running) {
        HANDLE h[4 + CONN_MAX];
        int    map[4 + CONN_MAX];
        int    n = 0, i;
        DWORD  w, now;
        QMsg  *list, *m;

        h[n] = g_qEvent; map[n++] = -1;
        if (g_listen != INVALID_SOCKET) { h[n] = g_listenEv; map[n++] = -2; }
        if (g_udp != INVALID_SOCKET)    { h[n] = g_udpEv;    map[n++] = -3; }
        if (g_disc != INVALID_SOCKET)   { h[n] = g_discEv;   map[n++] = -4; }
        for (i = 0; i < CONN_MAX; i++) if (g_conns[i]) { h[n] = g_conns[i]->ev; map[n++] = i; }

        /* 何もつながっていなければ、知らせが来るまで眠る(見回りで起きない) */
        {
            DWORD wait = INFINITE;
            for (i = 0; i < CONN_MAX; i++) if (g_conns[i]) { wait = 250; break; }
            if (g_disc != INVALID_SOCKET) wait = 100;
            w = WaitForMultipleObjects(n, h, FALSE, wait);
        }

        /* 待ち行列 */
        AcquireSRWLockExclusive(&g_qLock);
        list = g_qHead;
        g_qHead = g_qTail = NULL;
        ReleaseSRWLockExclusive(&g_qLock);
        while ((m = list) != NULL) {
            list = m->next;
            switch (m->kind) {
            case Q_SEND: {
                Conn *c = conn_find(m->conn);
                if (c) conn_msg(c, m->type, m->data, m->len);
                break;
            }
            case Q_CONFIG:
                apply_config((Config *)m->arg, FALSE);
                mem_free((void *)m->arg);
                break;
            case Q_SOCKET:
                if (m->conn < g_nc.npeers && m->len == (int)g_gen[m->conn] && g_connectors[m->conn])
                    conn_new((SOCKET)m->arg, TRUE, m->conn);
                else
                    closesocket((SOCKET)m->arg);
                break;
            case Q_DISCOVER:
                start_discover((HWND)m->arg);
                break;
            case Q_RECONNECT:
                for (i = 0; i < PEER_MAX; i++) {
                    if (g_connectors[i] && g_peerStatus[i] != PS_READY) {
                        if (g_peerStatus[i] == PS_BADPASS || g_peerStatus[i] == PS_REFUSED)
                            set_status(i, PS_CONNECTING);
                        SetEvent(g_connectors[i]->wake);
                    }
                }
                break;
            case Q_STOP:
                running = FALSE;
                break;
            }
            q_free(m);
        }

        if (w < WAIT_OBJECT_0 + (DWORD)n) {
            int k = map[w - WAIT_OBJECT_0];
            if (k == -2) { WSAResetEvent(g_listenEv); on_accept(); }
            else if (k == -3) { WSAResetEvent(g_udpEv); on_udp(); }
            else if (k == -4) { WSAResetEvent(g_discEv); on_disc(); }
        }
        /* 合図の来ていない接続も含めて見る(取りこぼしを防ぐ。数は少ない) */
        for (i = 0; i < CONN_MAX; i++) {
            Conn *c = g_conns[i];
            WSANETWORKEVENTS ne;
            if (!c) continue;
            if (WSAEnumNetworkEvents(c->s, c->ev, &ne) == 0) {
                if (ne.lNetworkEvents & FD_READ)  conn_read(c);
                if (ne.lNetworkEvents & FD_WRITE) conn_flush(c);
                if (ne.lNetworkEvents & FD_CLOSE) { conn_read(c); c->dead = TRUE; }
            }
        }

        /* 時間の見回り */
        now = GetTickCount();
        for (i = 0; i < CONN_MAX; i++) {
            Conn *c = g_conns[i];
            if (!c) continue;
            if (c->state != CS_READY && now - c->since > HS_TIMEOUT) c->dead = TRUE;
            if (c->state == CS_READY) {
                if (c->out && now - c->lastPing >= PING_MS) {
                    c->lastPing = now;
                    conn_msg(c, M_PING, NULL, 0);
                }
                if (now - c->lastRx > DEAD_MS) {
                    log_printf(L"%s: 応答がないので切断します", c->name);
                    c->dead = TRUE;
                }
            }
            if (c->dead) conn_close(i);
        }
        if (g_disc != INVALID_SOCKET && (LONG)(now - g_discUntil) > 0) {
            closesocket(g_disc);
            g_disc = INVALID_SOCKET;
            if (g_discHwnd) PostMessageW(g_discHwnd, WM_APP_FOUND, 0, 0);     /* 終わり */
            g_discHwnd = NULL;
        }
    }

    stop_connectors();
    for (;;) {
        int i, any = 0;
        for (i = 0; i < CONN_MAX; i++) if (g_conns[i]) { conn_close(i); any = 1; }
        if (!any) break;
    }
    close_listen();
    if (g_disc != INVALID_SOCKET) closesocket(g_disc);
    return 0;
}

BOOL net_start(void)
{
    WSADATA wd;
    Config *first;

    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return FALSE;
    first = (Config *)mem_alloc(sizeof(Config));
    if (!first) return FALSE;
    *first = g_cfg;
    g_qEvent   = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_listenEv = WSACreateEvent();
    g_udpEv    = WSACreateEvent();
    g_discEv   = WSACreateEvent();
    g_thread = CreateThread(NULL, 0, net_thread, first, 0, NULL);
    if (!g_thread) { mem_free(first); return FALSE; }
    return TRUE;
}

void net_stop(void)
{
    QMsg *m;
    if (!g_thread) return;
    m = q_new(Q_STOP);
    if (m) q_push(m);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
}
